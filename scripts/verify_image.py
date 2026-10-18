"""Read-only inspection of a supplied EXE; never patches or launches it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    data = args.image.read_bytes()
    assert data[:2] == b'MZ'
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe+4] == b'PE\0\0'
    machine, count = struct.unpack_from('<HH', data, pe+4)
    optional = struct.unpack_from('<H', data, pe+20)[0]
    assert machine == 0x8664 and struct.unpack_from('<H', data, pe+24)[0] == 0x20b
    size = struct.unpack_from('<I', data, pe+24+56)[0]
    base = struct.unpack_from('<Q', data, pe+24+24)[0]
    profile = json.loads((ROOT/'src/image_profile.json').read_text())
    assert size >= profile['required_image_size']
    sections = [struct.unpack_from('<IIII', data, pe+24+optional+i*40+8) for i in range(count)]
    for rva, raw in profile['entries'] + profile['camera_entries'] + profile.get('input_entries',[]) + profile.get('fill_entries',[]) + profile.get('depth_entries',[]):
        expected = bytes.fromhex(raw)
        found = False
        for _, start, length, offset in sections:
            if start <= rva and rva + len(expected) <= start + length:
                actual = data[offset + rva-start:offset + rva-start + len(expected)]
                assert actual == expected, f'Code mismatch at RVA {rva:x}'
                found = True
                break
        assert found, f'Unmapped RVA {rva:x}'
    print(json.dumps({'path': str(args.image.resolve()), 'sha256': hashlib.sha256(data).hexdigest(),
                      'architecture': 'x64', 'imageBase': hex(base), 'fileVersion': None,
                      'matchedGuards': len(profile['entries']),
                      'matchedCameraGuards': len(profile['camera_entries']),
                      'matchedInputGuards': len(profile.get('input_entries',[])),
                      'matchedFillGuards': len(profile.get('fill_entries',[])),
                      'matchedDepthGuards': len(profile.get('depth_entries',[]))}, indent=2))


if __name__ == '__main__':
    main()
