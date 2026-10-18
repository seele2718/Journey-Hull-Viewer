"""Build the Windows x64 viewer from this self-contained source tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def generate_guards(build):
    profile = json.loads((ROOT / 'src/image_profile.json').read_text())
    rows = []
    for rva, raw in profile['entries']:
        data = bytes.fromhex(raw)
        assert 0 < len(data) <= 32
        rows.append('{%s, %d, {%s}}' % (hex(rva), len(data), ','.join(hex(x) for x in data)))
    (build / 'image_guards.h').write_text('''/* Generated; localized compatibility checks, not an EXE hash allowlist. */
typedef struct image_guard { uint32_t rva, size; BYTE bytes[32]; } image_guard;
static const image_guard k_image_guards[] = {
''' + ',\n'.join(rows) + '''
};
static BOOL verify_image_guards(BYTE *image) {
  if (!readable_range(image, sizeof(IMAGE_DOS_HEADER))) return FALSE;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return FALSE;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
  if (!readable_range(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->OptionalHeader.SizeOfImage < ''' + str(profile['required_image_size']) + ''') return FALSE;
  for (unsigned i = 0; i < sizeof(k_image_guards)/sizeof(k_image_guards[0]); ++i) {
    const image_guard *g = k_image_guards + i;
    if ((uint64_t)g->rva + g->size > nt->OptionalHeader.SizeOfImage ||
        !readable_range(image + g->rva, g->size) ||
        !bytes_equal(image + g->rva, g->bytes, g->size)) return FALSE;
  }
  return TRUE;
}
''')
    camera_rows = []
    for rva, raw in profile['camera_entries']:
        data = bytes.fromhex(raw)
        assert 0 < len(data) <= 32
        camera_rows.append('{%s, %d, {%s}}' % (hex(rva), len(data), ','.join(hex(x) for x in data)))
    with (build / 'image_guards.h').open('a') as header:
        header.write('static const image_guard k_camera_guards[] = {\n' + ',\n'.join(camera_rows) + '''
};
static BOOL verify_camera_guards(BYTE *image) {
  for (unsigned i = 0; i < sizeof(k_camera_guards)/sizeof(k_camera_guards[0]); ++i) {
    const image_guard *g = k_camera_guards + i;
    if (!image || !readable_range(image + g->rva, g->size) ||
        !bytes_equal(image + g->rva, g->bytes, g->size)) return FALSE;
  }
  return TRUE;
}
''')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', action='store_true')
    parser.add_argument('--local-index', action='store_true',
                        help='Include generated inspection files for local testing; do not redistribute game data')
    args = parser.parse_args()
    if args.local_index and not args.package:
        parser.error('--local-index requires --package')
    version = (ROOT / 'VERSION').read_text().strip()
    assert re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?', version)
    destination = ROOT / 'dist/candidates' / version
    if args.package and destination.exists():
        raise FileExistsError(f'Immutable candidate already exists: {destination}')
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    (build / 'version.h').write_text(f'#define HV_VERSION "{version}"\n')
    generate_guards(build)
    profile = json.loads((ROOT/'src/image_profile.json').read_text())
    rows=[]
    for rva,raw in profile['fill_entries']:
        data=bytes.fromhex(raw)
        rows.append('{%s,%d,{%s}}'%(hex(rva),len(data),','.join(hex(x) for x in data)))
    with (build/'image_guards.h').open('a') as header:
        header.write('static const image_guard k_fill_guards[] = {\n'+',\n'.join(rows)+'\n};\n')
        header.write('static inline BOOL verify_fill_guards(BYTE *image) {\n')
        header.write('  if (!image) return FALSE;\n')
        header.write('  for (unsigned i=0;i<sizeof(k_fill_guards)/sizeof(*k_fill_guards);i++) {\n')
        header.write('    const image_guard *g=k_fill_guards+i;\n')
        header.write('    if (!readable_range(image+g->rva,g->size)||!bytes_equal(image+g->rva,g->bytes,g->size)) return FALSE; }\n  return TRUE;\n}\n')
    entries = [(rva,bytes.fromhex(raw)) for rva,raw in profile['input_entries']]
    with (build/'image_guards.h').open('a') as header:
        header.write('static inline BOOL verify_input_guards(BYTE *image) {\n')
        for rva,raw in entries:
            header.write('  { const BYTE expected[] = {'+','.join(hex(x) for x in raw)+'};\n')
            header.write(f'    if (!readable_range(image+{rva},sizeof(expected)) || !bytes_equal(image+{rva},expected,sizeof(expected))) return FALSE; }}\n')
        header.write('  return TRUE;\n}\n')
    cc = os.environ.get('CC', 'x86_64-w64-mingw32-gcc')
    common = [cc, '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', '-static-libgcc',
              '-I', str(build), '-I', str(ROOT / 'third_party/minhook/include'),
              '-Wl,--nxcompat,--dynamicbase,--no-insert-timestamp,--strip-all']
    hook_sources = [str(ROOT / 'third_party/minhook/src' / p)
                    for p in ('hook.c', 'buffer.c', 'trampoline.c', 'hde/hde64.c')]
    dll = build / 'journey_hull_viewer.dll'
    exe = build / 'JourneyHullViewer.exe'
    resource=build/'panel_resource.o'
    subprocess.run([os.environ.get('WINDRES','x86_64-w64-mingw32-windres'),
                    '-I',str(ROOT/'src'),str(ROOT/'src/panel.rc'),str(resource)],check=True)
    panel_libs=['-luser32','-lgdi32','-lshell32','-lcomctl32','-luxtheme']
    data_sources=[str(ROOT/'third_party/puff/puff.c')]
    subprocess.run(common + ['-shared', str(ROOT / 'src/renderer.c')] + hook_sources +
                   ['-ldxguid','-o', str(dll)], check=True)
    subprocess.run(common + ['-municode', '-mwindows', str(ROOT / 'src/panel.c'),str(resource)]+data_sources+
                            panel_libs+['-o', str(exe)], check=True)
    # Fixtures run in their own processes, never inside Journey.
    subprocess.run(common + [str(ROOT / 'tests/renderer_tests.c')] + hook_sources +
                   ['-ldxguid','-o', str(build / 'renderer_tests.exe')], check=True)
    subprocess.run(common + ['-municode', str(ROOT / 'tests/attach_tests.c'),str(resource)]+data_sources+
                            panel_libs+['-o', str(build / 'attach_tests.exe')], check=True)
    subprocess.run(common + [str(ROOT/'tests/json_tests.c'),'-o',str(build/'json_tests.exe')],check=True)
    subprocess.run(common + [str(ROOT/'tests/face_tests.c'),'-o',str(build/'face_tests.exe')],check=True)
    subprocess.run(common + [str(ROOT/'tests/shell_tests.c')]+hook_sources+['-ldxguid','-o',str(build/'shell_tests.exe')],check=True)
    subprocess.run(common + ['-municode',str(ROOT/'tests/game_data_tests.c')]+data_sources+['-o',str(build/'game_data_tests.exe')],check=True)
    subprocess.run(common + ['-municode',str(ROOT/'tests/panel_data_tests.c'),str(resource)]+data_sources+panel_libs+['-o',str(build/'panel_data_tests.exe')],check=True)
    subprocess.run(common + [str(ROOT / 'tests/target.c'), '-Wl,--image-base,0x140000000',
                            '-o', str(build / 'fixture_target.exe')], check=True)
    subprocess.run(common + [str(ROOT / 'tests/inspection_math.c'),
                            '-o', str(build / 'inspection_tests.exe')], check=True)
    subprocess.run(common + [str(ROOT / 'tests/input_tests.c')] + hook_sources +
                   ['-ldxguid','-o',str(build/'input_tests.exe')],check=True)
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in sorted(ROOT.rglob('*'))
              if p.is_file() and p.relative_to(ROOT).parts[0] not in ('build', 'dist', '.git')
              and '__pycache__' not in p.parts and p.name != '.DS_Store'}
    metadata = {'version': version,
                'sourceRevision': None, 'inputs': inputs,
                'compiler': subprocess.check_output([cc, '--version'], text=True).splitlines()[0],
                'artifacts': {p.name: sha(p) for p in (exe, dll)}}
    (build / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata['artifacts'], indent=2))
    if args.package:
        destination.mkdir(parents=True)
        for p in (exe, dll, ROOT / 'README.md', ROOT / 'CHANGELOG.md', ROOT / 'VERSION', ROOT / 'THIRD_PARTY.md'):
            shutil.copyfile(p, destination / p.name)
        shutil.copyfile(ROOT / 'third_party/minhook/LICENSE.txt', destination / 'MinHook-LICENSE.txt')
        shutil.copyfile(ROOT / 'third_party/puff/puff.h', destination / 'Puff-LICENSE.txt')
        shutil.copytree(ROOT / 'docs', destination / 'docs')
        if args.local_index:
            shutil.copytree(build/'inspection',destination/'inspection')
            metadata['localOnlyInspectionData'] = True
        metadata['artifacts'] = {str(p.relative_to(destination)): sha(p) for p in destination.rglob('*') if p.is_file()}
        (destination / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(destination)


if __name__ == '__main__':
    main()
