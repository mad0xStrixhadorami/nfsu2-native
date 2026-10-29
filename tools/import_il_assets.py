#!/usr/bin/env python3
"""Convert the Israeli mesh assets (GLB, Y-up) into the game's compact .ilm files (Z-up).

usage: import_il_assets.py <MeshAssets dir> <out dir>

Only the .glb files are read (parsed here, nothing from the asset folders is executed).  For every GLB the
node tree is flattened, each primitive becomes a sub-mesh tied to a material, the axes are turned into the
game's frame (x east, y north, z up: game = (x, -z, y) of the glTF), and the embedded textures are written
once, by content, to <out>/tex/<name>.png.

.ilm layout (little endian):
  char magic[4] = "ILM2"; u32 nMat; u32 nSub; float bbox[6] (min xyz, max xyz, game frame)
  nMat x { char name[32]; float rgb[3] (sRGB 0..1); float emis[3]; float emisStrength; float alpha;
           u32 flags (1 emissive, 2 blend, 4 two-sided); char tex[32] (png stem, "" = none);
           float metal, rough }
  nSub x { u32 mat; char part[16] (Furniture / Islands / Markings / Signals / Signs / ...); u32 nv; u32 ni; nv x { float p[3], n[3], uv[2] }; ni x u32 }
"""
import hashlib, json, math, os, struct, sys
from io import BytesIO

COMP = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def srgb(c):
    c = max(0.0, min(1.0, c))
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def read_glb(path):
    d = open(path, 'rb').read()
    if d[:4] != b'glTF':
        raise ValueError('not a GLB: ' + path)
    j = None
    binchunk = b''
    o = 12
    while o + 8 <= len(d):
        n, t = struct.unpack_from('<II', d, o)
        body = d[o + 8:o + 8 + n]
        if t == 0x4E4F534A:
            j = json.loads(body)
        elif t == 0x004E4942:
            binchunk = body
        o += 8 + n
    return j, binchunk


def accessor(j, b, idx):
    a = j['accessors'][idx]
    bv = j['bufferViews'][a['bufferView']]
    fmt, size = COMP[a['componentType']]
    nc = NCOMP[a['type']]
    stride = bv.get('byteStride') or size * nc
    base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    out = []
    for i in range(a['count']):
        off = base + i * stride
        v = struct.unpack_from('<%d%s' % (nc, fmt), b, off)
        if a.get('normalized'):
            m = {'b': 127.0, 'B': 255.0, 'h': 32767.0, 'H': 65535.0}[fmt]
            v = tuple(max(x / m, -1.0) for x in v)
        out.append(v)
    return out


def mat_mul(a, b):  # column-major 4x4
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def node_matrix(n):
    if 'matrix' in n:
        return list(n['matrix'])
    t = n.get('translation', [0, 0, 0])
    q = n.get('rotation', [0, 0, 0, 1])
    s = n.get('scale', [1, 1, 1])
    x, y, z, w = q
    r = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0, 0, 0, 0, 1]
    for c in range(3):
        for k in range(3):
            r[c * 4 + k] *= s[c]
    r[12], r[13], r[14] = t
    return r


def xform(m, p, w=1.0):
    return (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12] * w,
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13] * w,
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] * w)


def to_game(p):  # glTF (x, y up, z) -> game (x, y north, z up)
    return (p[0], -p[2], p[1])


def pad(s, n):
    b = s.encode('ascii', 'replace')[:n - 1]
    return b + b'\0' * (n - len(b))


def convert(glb, outdir, texdir):
    j, b = read_glb(glb)
    # textures: write each embedded image once
    tex_names = []
    from PIL import Image
    for im in j.get('images', []):
        bv = j['bufferViews'][im['bufferView']]
        raw = b[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']]
        stem = im.get('name') or 'tex'
        pic = Image.open(BytesIO(raw))
        pic.load()
        name = stem
        dst = os.path.join(texdir, name + '.png')
        buf = BytesIO()
        pic.save(buf, 'PNG', optimize=True)
        if os.path.exists(dst) and hashlib.sha1(open(dst, 'rb').read()).digest() != hashlib.sha1(buf.getvalue()).digest():
            name = stem + '_' + hashlib.sha1(buf.getvalue()).hexdigest()[:6]
            dst = os.path.join(texdir, name + '.png')
        open(dst, 'wb').write(buf.getvalue())
        tex_names.append(name)

    def tex_of(tinfo):
        if not tinfo:
            return ''
        src = j['textures'][tinfo['index']]['source']
        return tex_names[src]

    mats = []
    for m in j.get('materials', []):
        pbr = m.get('pbrMetallicRoughness', {})
        base = pbr.get('baseColorFactor', [1, 1, 1, 1])
        emis = m.get('emissiveFactor', [0, 0, 0])
        es = m.get('extensions', {}).get('KHR_materials_emissive_strength', {}).get('emissiveStrength', 1.0)
        flags = 0
        if any(e > 0.001 for e in emis):
            flags |= 1
        if m.get('alphaMode') == 'BLEND' or base[3] < 0.99:
            flags |= 2
        if m.get('doubleSided'):
            flags |= 4
        tex = tex_of(pbr.get('baseColorTexture')) or tex_of(m.get('emissiveTexture'))
        mats.append(dict(name=m.get('name', 'mat'), rgb=[srgb(c) for c in base[:3]], emis=[srgb(e) for e in emis], es=es,
                         alpha=base[3], flags=flags, tex=tex, metal=pbr.get('metallicFactor', 0.0),
                         rough=pbr.get('roughnessFactor', 1.0)))
    if not mats:
        mats.append(dict(name='default', rgb=[0.7] * 3, emis=[0] * 3, es=1.0, alpha=1.0, flags=4, tex='', metal=0, rough=1))

    subs = []
    mn = [1e30] * 3
    mx = [-1e30] * 3

    def walk(ni, parent):
        n = j['nodes'][ni]
        m = mat_mul(parent, node_matrix(n))
        if 'mesh' in n:
            part = (j['meshes'][n['mesh']].get('name') or n.get('name') or '').rsplit('_', 1)[-1]
            for prim in j['meshes'][n['mesh']]['primitives']:
                if prim.get('mode', 4) != 4:
                    continue
                pos = accessor(j, b, prim['attributes']['POSITION'])
                nor = accessor(j, b, prim['attributes']['NORMAL']) if 'NORMAL' in prim['attributes'] else None
                uv = accessor(j, b, prim['attributes']['TEXCOORD_0']) if 'TEXCOORD_0' in prim['attributes'] else None
                idx = [i[0] for i in accessor(j, b, prim['indices'])] if 'indices' in prim else list(range(len(pos)))
                verts = []
                for k, p in enumerate(pos):
                    gp = to_game(xform(m, p))
                    if nor:
                        nn = to_game(xform(m, nor[k], 0.0))
                        ln = math.sqrt(sum(c * c for c in nn)) or 1.0
                        nn = tuple(c / ln for c in nn)
                    else:
                        nn = (0.0, 0.0, 1.0)
                    t = uv[k] if uv else (0.0, 0.0)
                    verts.append(gp + nn + (t[0], t[1]))
                    for q in range(3):
                        mn[q] = min(mn[q], gp[q])
                        mx[q] = max(mx[q], gp[q])
                subs.append((prim.get('material', 0), verts, idx, part))
        for c in n.get('children', []):
            walk(c, m)

    ident = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    scene = j['scenes'][j.get('scene', 0)]
    for r in scene['nodes']:
        walk(r, ident)

    stem = os.path.splitext(os.path.basename(glb))[0]
    out = bytearray(b'ILM2') + struct.pack('<II', len(mats), len(subs)) + struct.pack('<6f', *mn, *mx)
    for m in mats:
        out += pad(m['name'], 32)
        out += struct.pack('<3f3f2fI', *m['rgb'], *m['emis'], m['es'], m['alpha'], m['flags'])
        out += pad(m['tex'], 32)
        out += struct.pack('<2f', m['metal'], m['rough'])
    for mi, verts, idx, part in subs:
        out += struct.pack('<I', mi) + pad(part, 16) + struct.pack('<II', len(verts), len(idx))
        for v in verts:
            out += struct.pack('<8f', *v)
        out += struct.pack('<%dI' % len(idx), *idx)
    open(os.path.join(outdir, stem + '.ilm'), 'wb').write(out)
    tris = sum(len(i) // 3 for _, _, i, _ in subs)
    print('%-44s %2d mats %2d subs %6d tris  %s..%s  %.2f MB' % (
        stem, len(mats), len(subs), tris, [round(x, 1) for x in mn], [round(x, 1) for x in mx], len(out) / 1e6))


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    texdir = os.path.join(out, 'tex')
    os.makedirs(texdir, exist_ok=True)
    for root, _, files in sorted(os.walk(src)):
        for f in sorted(files):
            if f.lower().endswith('.glb'):
                convert(os.path.join(root, f), out, texdir)


if __name__ == '__main__':
    main()
