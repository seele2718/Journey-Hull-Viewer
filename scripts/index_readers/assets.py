"""Read-only readers for generated Journey PC level data. Never executes Lua."""
from __future__ import annotations

import gzip
import base64
import hashlib
import math
import re
import struct
from collections import Counter
from pathlib import Path
from .meshes import UnsupportedMesh, is_affine, read_mesh, transform_point
from .textures import UnsupportedTexture, dds_info, dds_png

MAX_FILE = 64 * 1024 * 1024
HASH = re.compile(r"^[0-9a-f]{32}$")
LEVEL_NAMES = {
    "Graveyard": "Chapter Select / Entrance", "Barrens": "Broken Bridge",
    "Desert": "Pink Desert", "Canyon": "Sunken City", "Cave": "Underground",
    "Ruins": "Tower", "Mountain": "Snow", "Summit": "Paradise",
    "Credits": "Credits", "Bryan": "Bryan", "Matt": "Matt", "Chris": "Chris",
}


class AssetError(ValueError):
    pass


def read_terrain(raw):
    """Decode the authored PC height grid, in world units (not live dune sim)."""
    if len(raw) != 1048576:
        raise AssetError("Unsupported terrain layout")
    # Native sampling normalizes unsigned 16-bit heights, then scales to 1000.
    return {"width": 256, "depth": 512, "spacing": 2,
            "heights": [value * (1000 / 65535)
                        for value in struct.unpack_from(">131072H", raw)]}


class LuaTableReader:
    """Parse generated instance-table assignments without executing Lua."""
    token = re.compile(
        r"\s+|--\[\[.*?\]\]|--[^\n]*|"
        r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|'''
        r"-?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?|"
        r"[A-Za-z_]\w*|[{}\[\],;=]", re.S)

    def __init__(self, text):
        self.tokens = []
        end = 0
        for m in self.token.finditer(text):
            if m.start() != end:
                raise AssetError(f"Unsupported Lua syntax near {text[end:end+35]!r}")
            end = m.end()
            t = m.group()
            if not t.isspace() and not t.startswith("--"):
                self.tokens.append(t)
        if text[end:].strip():
            raise AssetError("Unsupported trailing Lua text")
        self.at = 0

    def peek(self, offset=0):
        return self.tokens[self.at+offset] if self.at+offset < len(self.tokens) else None

    def take(self, expected=None):
        t = self.peek()
        if t is None or (expected is not None and t != expected):
            raise AssetError(f"Expected {expected!r}, found {t!r}")
        self.at += 1
        return t

    @staticmethod
    def string(token):
        escapes = {"n": "\n", "r": "\r", "t": "\t", "a": "\a", "b": "\b",
                   "f": "\f", "v": "\v", "\\": "\\", '"': '"', "'": "'"}
        def replace(m):
            s = m.group()[1:]
            if s.isdigit():
                n = int(s)
                if n > 255:
                    raise AssetError("Invalid Lua byte escape")
                return chr(n)
            if s not in escapes:
                raise AssetError(f"Unsupported Lua escape {s!r}")
            return escapes[s]
        return re.sub(r"\\(?:\d{1,3}|.)", replace, token[1:-1])

    def value(self, depth=0):
        if depth > 80:
            raise AssetError("Lua data nesting exceeds limit")
        t = self.take()
        if t == "{":
            result, ordinal = {}, 1
            while self.peek() != "}":
                if self.peek() is None:
                    raise AssetError("Unterminated Lua table")
                if self.peek() == "[":
                    self.take("["); key = self.value(depth+1); self.take("]"); self.take("=")
                elif self.peek(1) == "=":
                    key = self.take(); self.take("=")
                else:
                    key = ordinal; ordinal += 1
                if not isinstance(key, (str, int)) or key in result:
                    raise AssetError("Invalid or duplicate Lua key")
                result[key] = self.value(depth+1)
                if self.peek() not in (",", ";", "}"):
                    raise AssetError("Expected table separator")
                if self.peek() != "}":
                    self.take()
            self.take("}")
            if result and set(result) == set(range(1, len(result)+1)):
                return [result[i] for i in range(1, len(result)+1)]
            return result
        if t[0] in "\"'":
            return self.string(t)
        if t == "Names":
            self.take("["); key = self.value(depth+1); self.take("]")
            if not isinstance(key, str):
                raise AssetError("Non-string Names reference")
            return {"$ref": key}
        if t in ("true", "false", "nil"):
            return {"true": True, "false": False, "nil": None}[t]
        try:
            n = float(t) if any(c in t for c in ".eE") else int(t)
        except ValueError as e:
            raise AssetError(f"Unexpected Lua identifier {t!r}") from e
        if not math.isfinite(n):
            raise AssetError("Non-finite numeric value")
        return n

    def read(self, expected):
        self.take(expected); self.take("="); data = self.value()
        if self.peek() == ";":
            self.take()
        if self.peek() is not None or not isinstance(data, dict):
            raise AssetError("Expected one generated instance-table assignment")
        if data.get("ObjectType") != expected:
            raise AssetError("Wrong instance-table ObjectType")
        numeric = sorted(k for k in data if isinstance(k, int))
        if numeric != list(range(1, len(numeric)+1)):
            raise AssetError("Instance array is not contiguous from index 1")
        entries = [data[k] for k in numeric]
        if any(not isinstance(x, dict) for x in entries):
            raise AssetError("Non-object instance entry")
        return entries


def read_bytes(path):
    if path.stat().st_size > MAX_FILE:
        raise AssetError(f"File too large: {path.name}")
    if path.suffix == ".gz":
        with gzip.open(path, "rb") as f:
            raw = f.read(MAX_FILE+1)
    else:
        raw = path.read_bytes()
    if len(raw) > MAX_FILE:
        raise AssetError("Uncompressed file exceeds limit")
    return raw


def finite(values):
    if not all(isinstance(x, (int, float)) and math.isfinite(x) for x in values):
        raise AssetError("Non-finite coordinates")
    return [round(float(x), 5) for x in values]


def string_at(raw, offset, size=64):
    return raw[offset:offset+size].split(b"\0", 1)[0].decode("utf-8", "replace")


def read_hulls(raw):
    if len(raw) < 16 or struct.unpack_from("<QQ", raw) != (16, len(raw)):
        raise AssetError("Invalid hull file header")
    objects, at = [], 16
    while at < len(raw):
        if len(objects) >= 20000 or at+208 > len(raw):
            raise AssetError("Invalid hull list")
        nxt = struct.unpack_from("<Q", raw, at+152)[0] or len(raw)
        if not at+208 <= nxt <= len(raw):
            raise AssetError("Invalid hull next link")
        pointers = struct.unpack_from("<4Q", raw, at+80)
        counts = struct.unpack_from("<4I", raw, at+112)
        tri, faces, edges, vertices = counts
        # Serialized triangle and edge indices are bytes, not uint16.
        if vertices > 256 or not vertices or tri > 100000 or edges > 100000:
            raise AssetError("Unsupported hull geometry counts")
        for p, size in zip(pointers, (tri*3, faces, edges*2, vertices*16)):
            if not at <= p <= nxt or p+size > nxt:
                raise AssetError("Hull geometry pointer outside record")
        points = [finite(struct.unpack_from("<3f", raw, pointers[3]+i*16))
                  for i in range(vertices)]
        triangles = list(raw[pointers[0]:pointers[0]+tri*3])
        edge_pairs = list(raw[pointers[2]:pointers[2]+edges*2])
        if any(i >= vertices for i in triangles+edge_pairs):
            raise AssetError("Hull index out of range")
        lo = finite(struct.unpack_from("<3f", raw, at+16))
        hi = finite(struct.unpack_from("<3f", raw, at+32))
        if any(a > b for a, b in zip(lo, hi)):
            raise AssetError("Inverted hull bounds")
        objects.append({"hash": string_at(raw, at+160, 32), "kind": "Hull",
                        "name": "Hull", "mask": struct.unpack_from("<I", raw, at+132)[0],
                        "enabled": bool(struct.unpack_from("<I", raw, at+128)[0]),
                        "bounds": [lo, hi], "position": [(a+b)/2 for a, b in zip(lo, hi)],
                        "vertices": points, "triangles": triangles, "edges": edge_pairs,
                        "positionNote": "Authored hull bounds centre"})
        at = nxt
    return objects


def read_decorations(raw):
    if len(raw) < 16:
        raise AssetError("Truncated decoration file")
    version, count = struct.unpack_from("<II", raw)
    if version != 1 or count > 20000 or len(raw) != 16+2112*count:
        raise AssetError("Unsupported decoration file layout")
    result = []
    for i in range(count):
        rec = raw[16+i*2112:16+(i+1)*2112]
        matrix = finite(struct.unpack_from("<16f", rec, 16))
        parameters = []
        for offset in range(384, 2096-111, 112):
            name = string_at(rec, offset, 16)
            if not name:
                break
            kind = struct.unpack_from('<I', rec, offset+16)[0]
            param = {'name': name, 'type': kind}
            if kind in (0, 1):
                values = struct.unpack_from('<4f', rec, offset-80)
                chosen = values[:1] if kind == 0 else values
                if not all(math.isfinite(v) for v in chosen):
                    raise AssetError('Non-finite material parameter')
                param['value'] = list(chosen)
            else:
                if kind == 2:
                    param['texture'] = string_at(rec, offset-64, 64)
                    param['note'] = 'Texture resource name' if param['texture'] else 'No texture resource name'
                else:
                    param['rawValueHex'] = rec[offset-80:offset-64].hex()
                    param['note'] = 'Unknown parameter type'
            parameters.append(param)
        result.append({"hash": string_at(rec, 112, 32), "kind": "Decoration",
                       "name": string_at(rec, 176), "shader": string_at(rec, 240),
                       "texture": string_at(rec, 320), "matrix": matrix,
                       "parameters": parameters,
                       "axisLengths": [math.sqrt(sum(v*v for v in matrix[k:k+3])) for k in (0,4,8)],
                       "yaw": math.degrees(math.atan2(matrix[8],matrix[10])),
                       "position": matrix[12:15],
                       "positionNote": "Authored instance origin, not mesh bounds or live position"})
    return result


def references(value, prefix=""):
    if isinstance(value, dict):
        if set(value) == {"$ref"}:
            yield prefix, value["$ref"]
        else:
            for key, v in value.items():
                # Clump Objects and Timeline TriggerName use strings, not Names[].
                if key == "TriggerName" and isinstance(v, str) and HASH.fullmatch(v):
                    yield f"{prefix}.{key}".lstrip("."), v
                elif key == "Objects" and isinstance(v, list):
                    for i, h in enumerate(v):
                        if isinstance(h, str) and HASH.fullmatch(h):
                            yield f"{prefix}.Objects[{i+1}]".lstrip("."), h
                else:
                    yield from references(v, f"{prefix}.{key}".lstrip("."))
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from references(v, f"{prefix}[{i+1}]")


class DataSource:
    TABLES = ("Trigger", "Clump", "Rail", "Marker", "Timeline", "DynamicNode",
              "ParticleEmitter", "SoundEmitter", "CameraKeyFrame", "EnvNode", "Jet", "Sign")

    def __init__(self, data_root, overlay_roots=()):
        self.root = Path(data_root).resolve(strict=True)
        self.roots = [self.root]+[Path(p).resolve(strict=True) for p in overlay_roots]
        self.texture_sources = {}
        if any(not p.is_dir() for p in self.roots):
            raise AssetError('Data sources must be directories')
        self.scripts = self.root/"Scripts"
        if not self.scripts.is_dir():
            raise AssetError("Data root must contain Scripts/Level_* directories")

    def levels(self):
        return [{"id": "Level_"+key, "name": name} for key, name in LEVEL_NAMES.items()
                if any((root/'Scripts'/('Level_'+key)).is_dir() for root in self.roots)]

    def find(self, *relative_paths):
        # Root priority comes before archive preference: a loose mod table must
        # override an archived base table. Last overlay wins, file by file.
        for root in reversed(self.roots):
            for relative in relative_paths:
                path = root/relative
                if path.is_file():
                    self.checked(path)
                    return path
        return None

    def source_name(self, path):
        for i, root in reversed(list(enumerate(self.roots))):
            if path.is_relative_to(root):
                return ('Base/' if i == 0 else f'Overlay {i}/')+str(path.relative_to(root))
        raise AssetError('Source outside configured data roots')

    def find_table(self, folder, name):
        archived = ['__'+name+'.lua.gz', '__'+name+'.lua']
        for i, root in reversed(list(enumerate(self.roots))):
            # Overlay Lua files are replacements; an accompanying archived
            # reference must not silently replace the mod's edited loose file.
            choices = [name+'.lua']+archived if i else archived+[name+'.lua']
            for filename in choices:
                path = root/folder/filename
                if path.is_file():
                    self.checked(path)
                    return path
        return None

    def checked(self, path):
        path = path.resolve(strict=True)
        if not any(path.is_relative_to(root) for root in self.roots):
            raise AssetError("Asset symlink leaves selected data root")
        return path

    def texture_png(self, level, alias):
        path = self.texture_sources.get(level, {}).get(alias)
        if not path:
            raise AssetError('Texture not in the loaded level manifest')
        return dds_png(read_bytes(self.checked(path)), terrain_shadow=alias=='__terrain')

    def catalogue(self):
        """Build a compact read-only asset index independently of scene buffers."""
        assets = {}
        def entry(name):
            return assets.setdefault(name, {'name': name, 'usages': [], 'maxObservedCopies': 0})
        for root in self.roots:
            for path in (root/'Meshes').glob('*.D3D11x64'):
                if re.fullmatch(r'[A-Za-z0-9_\-]+', path.stem):
                    entry(path.stem)
        for level in self.levels():
            path = self.find(Path('Scripts')/level['id']/'DecorationMeshInstances.lua.bin')
            if not path:
                continue
            counts = Counter(o['name'] for o in read_decorations(read_bytes(self.checked(path))))
            for name, count in counts.items():
                item = entry(name)
                item['usages'].append({**level, 'count': count})
                item['maxObservedCopies'] = max(item['maxObservedCopies'], count)
        for name, item in assets.items():
            if not re.fullmatch(r'[A-Za-z0-9_\-]+', name):
                item['status'] = 'Invalid mesh asset name'; continue
            path = self.find(Path('Meshes')/(name+'.D3D11x64'))
            try:
                if not path:
                    raise UnsupportedMesh('Mesh asset not available')
                mesh = read_mesh(read_bytes(self.checked(path)))
                item.update(bounds=mesh['bounds'], dimensions=[b-a for a,b in zip(*mesh['bounds'])],
                            vertices=mesh['vertexCount'], triangles=len(mesh['indices'])//3,
                            status='Supported static geometry')
            except UnsupportedMesh as exc:
                item['status'] = str(exc)
            if path:
                item['source'] = self.source_name(path)
        return {'assets': sorted(assets.values(), key=lambda o:o['name'].lower()),
                'note': 'Counts describe the configured level data, not proven engine copy limits. Dimensions are asset-local, before instance scale.',
                'dataSources': ['Base']+[f'Overlay {i}' for i in range(1,len(self.roots))]}

    def level(self, level):
        if level not in {x["id"] for x in self.levels()}:
            raise AssetError("Unknown level")
        folder, objects, sources, warnings = Path('Scripts')/level, {}, [], []

        def read(path):
            raw = read_bytes(self.checked(path))
            sources.append({"file": self.source_name(path),
                            "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
            return raw

        def add(obj):
            h = obj["hash"]
            if not HASH.fullmatch(h) or h in objects:
                raise AssetError(f"Duplicate or invalid level object hash: {h}")
            obj["refs"], obj["incoming"] = [], []
            objects[h] = obj

        for table in self.TABLES:
            name = table+"Instances"
            # Prefer the archived generated source; loose files are fallback only.
            path = self.find_table(folder, name)
            if not path:
                warnings.append(f"{name} is not available")
                continue
            for ordinal, entry in enumerate(LuaTableReader(read(path).decode("utf-8-sig")).read(name)):
                kind = {"DynamicNode": "Creature", "CameraKeyFrame": "Camera",
                        "ParticleEmitter": "Emitter", "SoundEmitter": "Sound",
                        "EnvNode": "Environment"}.get(table, table)
                obj = {"hash": entry["ObjectName"], "kind": kind,
                       "name": entry.get("GardenerName", "").split("|")[-1] or kind,
                       "gardener": entry.get("GardenerName", ""),
                       "type": entry.get("Type", table), "data": entry,
                       "source": self.source_name(path)}
                if table == "Trigger":
                    obj["index"] = ordinal
                if isinstance(entry.get("Position"), list) and len(entry["Position"]) == 3:
                    obj["position"] = finite(entry["Position"])
                    obj["positionNote"] = "Authored position, not live state"
                if table == "Rail":
                    obj["points"] = [finite(p.get("Position") if isinstance(p, dict) else p)
                                     for p in entry.get("RailDataPoints", [])]
                    if any(len(p) != 3 for p in obj["points"]):
                        raise AssetError("Invalid rail control point")
                    obj["closed"] = bool(entry.get("Closed"))
                    if obj["points"]:
                        obj["position"] = obj["points"][0]
                        obj["positionNote"] = "First authored rail control point"
                add(obj)

        for filename, reader in (("HullInstances.lua.bin", read_hulls),
                                 ("DecorationMeshInstances.lua.bin", read_decorations)):
            path = self.find(folder/filename)
            if path:
                for obj in reader(read(path)):
                    obj["source"] = self.source_name(path); add(obj)
            else:
                warnings.append(f"{filename} is not available")

        for obj in objects.values():
            for field, target in references(obj.get("data", {})):
                ref = {"field": field, "hash": target}
                obj["refs"].append(ref)
                if target in objects:
                    objects[target]["incoming"].append({"field": field, "hash": obj["hash"]})

        # Only walk containers, not arbitrary trigger chains, to assign spatial links.
        def spatial(target, visited=None):
            visited = set() if visited is None else visited
            if target in visited or len(visited) >= 256 or target not in objects:
                return []
            visited.add(target); ob = objects[target]
            if "position" in ob and ob["kind"] != "Trigger":
                return [target]
            if ob["kind"] == "Clump":
                return [h for r in ob["refs"] for h in spatial(r["hash"], visited)]
            return []

        fields = {"hulls", "activationRegion", "pos", "position", "initialPos", "rail",
                  "rails", "waterLevel", "meshes", "objects", "crea", "creature"}
        for obj in objects.values():
            if obj["kind"] == "Trigger":
                anchors = []
                for ref in obj["refs"]:
                    field = ref["field"].split(".")[-1]
                    if field in fields:
                        anchors.extend({"hash": h, "field": ref["field"]}
                                       for h in spatial(ref["hash"]))
                obj["anchors"] = anchors[:128]
                if anchors:
                    obj["position"] = objects[anchors[0]["hash"]]["position"]
                    obj["positionNote"] = "Linked object's position; trigger has no independent XYZ"

        terrain = None
        terrain_path = self.find(Path('Terrain')/level/'TerrainData.bin.gz',
                                 Path('Terrain')/level/'TerrainData.bin')
        if terrain_path:
            raw = read(terrain_path)
            if len(raw) == 1048576:
                terrain = read_terrain(raw)
            else:
                warnings.append("Terrain layout not supported; terrain omitted")
        else:
            warnings.append("Terrain data is not available")

        # Load each supported scenery asset once. Instance matrices and hashes
        # remain separate: repeated architecture shares geometry, not identity.
        meshes, previews, geometry_bytes = {}, {}, 0
        for obj in objects.values():
            if obj['kind'] != 'Decoration':
                continue
            name, shader = obj['name'], obj['shader']
            if (not shader.startswith('Mesh') or shader == 'MeshShadow'
                    or any(x in shader for x in ('Dude', 'Billboard', 'Cloth'))):
                obj['meshPreview'] = 'Origin only: effect or animated material'
                continue
            if not is_affine(obj['matrix']):
                obj['meshPreview'] = 'Origin only: unsupported instance transform'
                continue
            if name not in previews:
                path = None
                try:
                    if not re.fullmatch(r'[A-Za-z0-9_\-]+', name):
                        raise UnsupportedMesh('Invalid mesh asset name')
                    path = self.find(Path('Meshes')/(name+'.D3D11x64'))
                    if not path:
                        raise UnsupportedMesh('Mesh asset not available')
                    mesh = read_mesh(read(path))
                    size = (len(mesh['positions'])*4+len(mesh['indices'])*2
                            +sum(len(mesh.get(k,[]))*4 for k in ('uv','aoUv'))+len(mesh.get('colors',[])))
                    if geometry_bytes+size > 96*1024*1024:
                        raise UnsupportedMesh('Level scenery budget reached')
                    geometry_bytes += size
                    for key in ('uv','aoUv'):
                        if key in mesh:
                            values=mesh.pop(key)
                            mesh[key]=base64.b64encode(struct.pack(f'<{len(values)}f',*values)).decode('ascii')
                    if 'colors' in mesh:
                        mesh['colors']=base64.b64encode(bytes(mesh['colors'])).decode('ascii')
                    positions = struct.pack(f"<{len(mesh['positions'])}f", *mesh.pop('positions'))
                    indices = struct.pack(f"<{len(mesh['indices'])}H", *mesh.pop('indices'))
                    meshes[name] = {**mesh, 'source': self.source_name(path),
                                    'positions': base64.b64encode(positions).decode('ascii'),
                                    'indices': base64.b64encode(indices).decode('ascii'),
                                    'encoding': 'base64-f32le-u16le'}
                    previews[name] = 'Static mesh geometry; no live animation'
                except (UnsupportedMesh, FileNotFoundError) as exc:
                    previews[name] = 'Origin only: '+str(exc)
            obj['meshPreview'] = previews[name]
            if name in meshes:
                lo, hi = meshes[name]['bounds']
                corners = [transform_point([x,y,z], obj['matrix'])
                           for x in (lo[0],hi[0]) for y in (lo[1],hi[1]) for z in (lo[2],hi[2])]
                obj['bounds'] = [[min(p[k] for p in corners) for k in range(3)],
                                 [max(p[k] for p in corners) for k in range(3)]]
                obj['mesh'] = name
        scenery_count = sum('mesh' in o for o in objects.values())
        textures, texture_paths = {}, {}
        aliases={p['texture'] for o in objects.values() for p in o.get('parameters',[])
                 if p.get('texture') and p['type']==2}
        # Exact duplicated-stem resource convention only; never fuzzy-match an
        # arbitrary similarly named file. Missing aliases are explicit fallbacks.
        for alias in sorted(aliases)[:256]+['__terrain']:
            if not re.fullmatch(r'[A-Za-z0-9_\-]+',alias):
                continue
            path = self.find(Path('Textures')/level/'Bin'/'DuneColorShadowDuneColorShadow.dds') if alias=='__terrain' else self.find(Path('Textures/Bin')/(alias+alias+'.dds'))
            if not path:
                continue
            try:
                raw=read(path);info=dds_info(raw)
            except UnsupportedTexture:
                continue
            texture_paths[alias]=path
            textures[alias]={k:info[k] for k in ('width','height','format')}
            textures[alias].update(url=f'/api/texture/{level}/{alias}',source=self.source_name(path),
                                   sha256=hashlib.sha256(raw).hexdigest())
        self.texture_sources[level]=texture_paths
        return {"level": level, "name": LEVEL_NAMES[level[6:]], "objects": objects,
                "terrain": terrain, "meshes": meshes, "sceneryCount": scenery_count,
                "textures": textures,
                "dataSources": ['Base']+[f'Overlay {i}' for i in range(1,len(self.roots))],
                "sources": sources, "warnings": warnings,
                "counts": dict(Counter(o["kind"] for o in objects.values())),
                "notes": ["Static authored data, not current game state.",
                          f"Scenery: {scenery_count} supported static instances. Game Look previews supported base textures/terrain colour with simplified lighting; no live animation. Unsupported assets remain searchable origins.",
                          "Rail lines join authored control points, not native interpolation.",
                          "Trigger dots use linked geometry; references are not guaranteed execution edges."]}
