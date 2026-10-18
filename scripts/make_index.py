"""Generate object/reference indexes from an extracted Journey Data folder.

Parse level tables without executing Lua or changing the input files.
Write one .hvi file per level to the specified output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
from index_readers.assets import DataSource


def clean(value):
    return str(value).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')


def export(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    counts = {}
    for level in source.levels():
        scene = source.level(level['id'])
        objects = scene['objects']
        provenance = hashlib.sha256(json.dumps(scene['sources'], sort_keys=True).encode()).hexdigest()
        lines = ['HVI1\t'+level['id']+'\t'+provenance]
        for h, o in sorted(objects.items()):
            kind = o['kind']
            if kind == 'Creature' and o.get('type') != 'Creature':
                kind = 'DynamicNode'
            description = {'source': o['source']}
            if 'positionNote' in o:
                description['positionNote'] = o['positionNote']
            if 'data' in o:
                description['authored'] = o['data']
            else:
                description.update({k: o[k] for k in ('position', 'bounds', 'mask', 'active') if k in o})
            detail = json.dumps(description, ensure_ascii=False, separators=(', ', ': '))
            # Refuse oversized descriptions rather than silently cutting JSON.
            if len(detail.encode()) > 60000:
                raise ValueError('Object description exceeds index limit: '+h)
            lines.append('\t'.join(['O', h, kind, clean(o['name']), clean(o.get('type', '')), detail]))
        for h, o in sorted(objects.items()):
            for r in o['refs']:
                lines.append('\t'.join(['R', h, r['hash'], clean(r['field'])]))
        output = '\n'.join(lines)+'\n'
        if len(output.encode()) > 32*1024*1024:
            raise ValueError('Level index exceeds limit')
        with (destination/(level['id']+'.hvi')).open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(output)
        counts[level['id']] = len(objects)
    return counts


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    print(json.dumps(export(DataSource(args.data_root), args.output), indent=2))
