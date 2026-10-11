"""Run the built synthetic fixtures on Windows without launching Journey."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name != 'nt':
        raise SystemExit('Run these Windows fixtures on Windows after building.')
    logs = ROOT / 'build/test-results'
    logs.mkdir(parents=True, exist_ok=True)
    for name in ('renderer', 'attach'):
        result = subprocess.run([str(ROOT / 'build' / f'{name}_tests.exe')],
                                cwd=ROOT / 'build', capture_output=True,
                                text=True, timeout=60)
        (logs / f'{name}.log').write_text(result.stdout + result.stderr)
        print(name, result.returncode, result.stdout, result.stderr, flush=True)
        result.check_returncode()


if __name__ == '__main__':
    main()
