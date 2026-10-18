"""Run the built synthetic fixtures on Windows without launching Journey."""
import argparse
import os
from pathlib import Path
import subprocess
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-root', type=Path, help='Optional installed Journey Data folder (read only)')
    args = parser.parse_args()
    if os.name != 'nt':
        raise SystemExit('Run these Windows fixtures on Windows after building.')
    logs = ROOT / 'build/test-results'
    logs.mkdir(parents=True, exist_ok=True)
    def run(name, command):
        result = subprocess.run(command, cwd=ROOT / 'build', capture_output=True,
                                text=True, timeout=60)
        (logs / f'{name}.log').write_text(result.stdout + result.stderr)
        print(name, result.returncode, result.stdout, result.stderr, flush=True)
        result.check_returncode()

    for name in ('inspection', 'json', 'input', 'renderer', 'faces', 'shell', 'game-data', 'attach'):
        executable = {'faces': 'face_tests.exe', 'game-data': 'game_data_tests.exe'}.get(name, f'{name}_tests.exe')
        command = [str(ROOT / 'build' / executable)]
        run(name, command)
    if args.data_root:
        with tempfile.TemporaryDirectory(prefix='hull-data-tests-') as temporary:
            fixture = Path(temporary)
            index = fixture / 'inspection'
            index.mkdir()
            run('game-files', [str(ROOT / 'build/game_data_tests.exe'),
                               str(args.data_root.resolve()), str(index)])
            for name in ('attach_tests.exe', 'fixture_target.exe', 'journey_hull_viewer.dll'):
                shutil.copyfile(ROOT / 'build' / name, fixture / name)
            run('data-navigation', [str(fixture / 'attach_tests.exe'), '--with-index'])


if __name__ == '__main__':
    main()
