"""Read supported static geometry from PC Phyre 4611 mesh containers.

Unsupported vertex streams and node hierarchies raise UnsupportedMesh.
"""
import math
import struct


class UnsupportedMesh(ValueError):
    pass


IDENTITY = (1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.)


def transform_point(point, matrix):
    return [sum(point[i]*matrix[4*i+k] for i in range(3))+matrix[12+k]
            for k in range(3)]


def is_affine(matrix):
    if not all(math.isfinite(v) and abs(v) < 1e6 for v in matrix):
        return False
    if any(matrix[i] != 0 for i in (3, 7, 11)) or matrix[15] != 1:
        return False
    a, b, c = matrix[:3], matrix[4:7], matrix[8:11]
    det = (a[0]*(b[1]*c[2]-b[2]*c[1])-a[1]*(b[0]*c[2]-b[2]*c[0])
           + a[2]*(b[0]*c[1]-b[1]*c[0]))
    return abs(det) > 1e-8


def read_mesh(raw):
    if len(raw) < 128 or raw[:5] != b'RYHPT' or raw[12:16] != b'4611':
        raise UnsupportedMesh('Unsupported mesh container')
    index_bytes, vertex_bytes = struct.unpack_from('<II', raw, 0x48)
    metadata_end = len(raw)-index_bytes-vertex_bytes
    if (metadata_end < 128 or not index_bytes or index_bytes % 6 or not vertex_bytes
            or index_bytes > 12*1024*1024):
        raise UnsupportedMesh('Unsupported geometry buffers')
    # The asset namespace begins after reflection/type metadata. Limit scanning
    # to this small metadata region, never search positions in vertex payloads.
    namespace = raw.find(b'Data/Meshes/', 128, metadata_end)
    if namespace < 0 or metadata_end-namespace > 256*1024:
        raise UnsupportedMesh('Asset namespace not located')
    marker = raw.find(b'PNode\0PMesh\0PMeshInstance\0', namespace, metadata_end)
    if marker < 0:
        raise UnsupportedMesh('Unsupported mesh/node instance layout')
    start = (namespace+3)//4*4
    streams = []
    for offset in range(start, marker-80, 4):
        stride, count = struct.unpack_from('<II', raw, offset)
        if (stride in range(12, 97, 4) and 0 < count <= 65536
                and stride*count == vertex_bytes
                and struct.unpack_from('<Q', raw, offset+72)[0] == vertex_bytes):
            streams.append((offset, stride, count))
    if len(streams) != 1:
        raise UnsupportedMesh('Multiple or unsupported vertex streams')
    stream_offset, stride, count = streams[0]
    indices = list(struct.unpack_from(f'<{index_bytes//2}H', raw, metadata_end))
    if max(indices) >= count:
        raise UnsupportedMesh('Index buffer is not a supported uint16 triangle list')
    vertices_start = metadata_end+index_bytes
    points = [struct.unpack_from('<3f', raw, vertices_start+i*stride) for i in range(count)]
    if not all(math.isfinite(v) and abs(v)<1e6 for point in points for v in point):
        raise UnsupportedMesh('Invalid vertex positions')

    # Serialized stream elements: byte offset, zero name-pointer relocation,
    # packed type/semantic-index. Support only this five-element form:
    # float3 position, packed normal, RGBA8 colour, half4 ST0, half2/half4 ST1.
    # A plausible stride alone is not sufficient evidence for UV coordinates.
    layouts = []
    if stride in (32,36):
        for offset in range(max(start, marker-800), marker-119, 4):
            entries=[struct.unpack_from('<QQQ',raw,offset+i*24) for i in range(5)]
            expected=(2,39,19,7,261 if stride==32 else 263)
            if (tuple(e[0] for e in entries)==(0,12,16,20,28)
                    and all(e[1]==0 for e in entries) and tuple(e[2] for e in entries)==expected):
                layouts.append(offset)
    attributes = {}
    if len(layouts)==1:
        uv=[struct.unpack_from('<4e',raw,vertices_start+i*stride+20) for i in range(count)]
        if all(math.isfinite(v) for p in uv for v in p):
            attributes={'uv': [v for p in uv for v in p[:2]],
                        'aoUv': [v for p in uv for v in p[2:]],
                        'colors': [v for i in range(count) for v in raw[vertices_start+i*stride+16:vertices_start+i*stride+20]],
                        'attributeOffset': layouts[0],
                        'uvLayout': 'Declared half4 ST0 at byte 20; RGBA8 colour at byte 16'}

    # PNode's reflected local-matrix field is at +32 in its 112-byte record.
    # Its preceding pointer slots and following name/padding slots are zero in
    # the serialized representation; relocation fixups live elsewhere.
    nodes = []
    for offset in range((stream_offset+3)//4*4, marker-80, 4):
        if raw[offset-32:offset] != b'\0'*32 or raw[offset+64:offset+80] != b'\0'*16:
            continue
        matrix = struct.unpack_from('<16f', raw, offset)
        if is_affine(matrix):
            nodes.append((offset, matrix))
    if not nodes or any(b[0]-a[0] != 112 for a, b in zip(nodes, nodes[1:])):
        raise UnsupportedMesh('Node hierarchy is not a supported static chain')
    nonidentity = [node for node in nodes if any(abs(a-b)>1e-6 for a,b in zip(node[1], IDENTITY))]
    if nonidentity:
        # A root plus one mesh node can be localized unambiguously. More complex
        # nonidentity skeleton/segment mappings need a full skin/hierarchy reader.
        if len(nodes) != 2 or len(nonidentity) != 1 or nonidentity[0][0] != nodes[-1][0]:
            raise UnsupportedMesh('Nontrivial animated or multi-node transforms')
        local_matrix = nonidentity[0][1]
        points = [transform_point(p, local_matrix) for p in points]
        placement = 'Stored mesh-node transform applied before instance transform'
    else:
        local_matrix = IDENTITY
        placement = 'Identity mesh-node transforms'
    lo = [min(p[k] for p in points) for k in range(3)]
    hi = [max(p[k] for p in points) for k in range(3)]
    return {'positions': [v for p in points for v in p], 'indices': indices,
            **attributes,
            'bounds': [lo, hi], 'stride': stride, 'vertexCount': count,
            'localMatrix': list(local_matrix), 'nodeCount': len(nodes),
            'streamOffset': stream_offset, 'placement': placement}
