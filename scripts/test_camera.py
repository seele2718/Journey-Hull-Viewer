"""Optional Lua syntax/behavior test of the exact embedded fixed commands."""
import ast
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'src/camera.h').read_text()

def literals(text):
    return ''.join(ast.literal_eval(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', text))

prefix = literals(re.search(r'const char \*prefix = (.*?);\n', source, re.S)[1])
suffix = literals(re.search(r'const char \*suffix =(.*?);\n', source, re.S)[1])
actions = re.findall(r'case (HV_CAM_\w+): return ("(?:\\.|[^"\\])*");', source)
assert len(actions) == 8
program = 'local commands = {}\n'
for name, text in actions:
    command = ('local x,y,z=123.5,-4,800; ' if name == 'HV_CAM_MOVE' else '') + prefix + ast.literal_eval(text) + suffix
    assert ']=]' not in command
    program += f'commands.{name} = assert(load([=[{command}]=]))\n'
program += (root / 'tests/camera.lua').read_text()
result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'lua', '-'],
                        input=program, text=True, capture_output=True)
log = result.stdout + result.stderr
(root / 'build/test-results').mkdir(exist_ok=True)
(root / 'build/test-results/camera-lua.log').write_text(log)
print(log, end='')
result.check_returncode()
