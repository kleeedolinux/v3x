"""Asset-streaming inventory: estimated texture memory per category and texture_quality, mesh counts.
Usage: re_stream_assets.py <game dir> [--dlc] [--quality low|medium|high|ultra]

For every .dds under <game>/gfx (and <game>/dlc/*/gfx with --dlc):
  - reads the DDS header (size, mips, format) -> resident bytes with the full mip chain
    (= the GPU allocation; the iGPU uses shared system memory, so this is also RAM/commit),
  - applies gfx/load_settings.settings (first matching path prefix; the array is the number of
    mips skipped at low/medium/high/ultra; negative = skip) -> bytes after mip skipping.
    PNG/TGA keep full resolution (per the comment in load_settings.settings).
Also counts pdxmesh entries in .asset files and .mesh bytes per category.
Numbers are UPPER bounds of what could be resident: the engine loads a texture only when something
references it (see docs/re/streaming.md)."""
import argparse, os, re, struct, collections

BS = chr(92)
CATS = [  # (category, path prefix) first match wins
    ('event_pictures', 'gfx/event_pictures'), ('loadingscreens', 'gfx/loadingscreens'),
    ('illustrations', 'gfx/interface/illustrations'), ('state_pictures', 'gfx/state_pictures'),
    ('unit_illustrations', 'gfx/unit_illustrations'), ('gui_icons', 'gfx/interface/icons'),
    ('gui_other', 'gfx/interface'), ('gui_frontend', 'gfx/frontend'),
    ('portrait_models', 'gfx/models/portraits'), ('portrait_tex', 'gfx/portraits'),
    ('coat_of_arms', 'gfx/coat_of_arms'), ('military_models', 'gfx/models/military'),
    ('building_models', 'gfx/models/buildings'), ('debug_models', 'gfx/models/debug'),
    ('other_models', 'gfx/models'), ('map_terrain', 'gfx/map/terrain'), ('map', 'gfx/map'),
    ('particles', 'gfx/particles'), ('other', 'gfx'),
]
QUAL = {'low': 0, 'medium': 1, 'high': 2, 'ultra': 3}
BPP = {b'DXT1': 4, b'DXT3': 8, b'DXT5': 8, b'ATI1': 4, b'ATI2': 8, b'BC4U': 4, b'BC5U': 8}
DX10_BPP = {71: 4, 72: 4, 74: 8, 75: 8, 77: 8, 78: 8, 80: 4, 81: 4, 83: 8, 84: 8, 95: 8, 96: 8,
            98: 8, 99: 8, 28: 32, 29: 32, 87: 32, 91: 32, 10: 64, 2: 128, 24: 32, 61: 8, 49: 16, 56: 16}


def load_settings(game):
    p = os.path.join(game, 'gfx', 'load_settings.settings')
    rules = []
    if os.path.exists(p):
        for line in open(p, encoding='utf-8', errors='ignore'):
            m = re.match(r'\s*"([^"]+)"\s*=\s*\{\s*(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s*\}', line)
            if m:
                rules.append((m.group(1).lower(), [int(m.group(i)) for i in range(2, 6)]))
    return rules


def dds_info(path):
    """(width, height, mips, bits per pixel, faces) or None."""
    try:
        with open(path, 'rb') as f:
            h = f.read(148)
    except OSError:
        return None
    if len(h) < 128 or h[:4] != b'DDS ':
        return None
    hgt, w = struct.unpack_from('<II', h, 12)
    mips = max(1, struct.unpack_from('<I', h, 28)[0])
    pf_flags = struct.unpack_from('<I', h, 80)[0]
    fourcc = h[84:88]
    bits = struct.unpack_from('<I', h, 88)[0]
    caps2 = struct.unpack_from('<I', h, 112)[0]
    faces = 6 if caps2 & 0x200 else 1
    if pf_flags & 4:
        if fourcc == b'DX10' and len(h) >= 148:
            bpp = DX10_BPP.get(struct.unpack_from('<I', h, 128)[0], 8)
        else:
            bpp = BPP.get(fourcc, 8)
    else:
        bpp = bits or 32
    return w, hgt, mips, bpp, faces


def chain_bytes(w, h, mips, bpp, faces, skip):
    """bytes of mips [skip, mips) (block-compressed formats use 4x4 blocks)."""
    tot = 0
    bc = bpp in (4, 8)
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m)
        if bc:
            mw, mh = (mw + 3) // 4 * 4, (mh + 3) // 4 * 4
        if m >= skip:
            tot += mw * mh * bpp // 8
    return tot * faces


def category(rel):
    for c, p in CATS:
        if rel.startswith(p):
            return c
    return 'other'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('--dlc', action='store_true')
    ap.add_argument('--quality', default='medium', choices=list(QUAL))
    a = ap.parse_args()
    rules = load_settings(a.game)
    q = QUAL[a.quality]
    roots = [(a.game, '')]
    if a.dlc:
        d = os.path.join(a.game, 'dlc')
        for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            roots.append((os.path.join(d, n), n + ':'))
    full = collections.Counter(); skip = collections.Counter(); cnt = collections.Counter()
    mesh_b = collections.Counter(); pdxmesh = collections.Counter(); unc = collections.Counter()
    for root, tag in roots:
        for dp, _, fs in os.walk(os.path.join(root, 'gfx')):
            relp = os.path.relpath(dp, root).replace(BS, '/').lower()
            cat = category(relp + '/')
            for f in fs:
                fl = f.lower(); p = os.path.join(dp, f); rel = relp + '/' + fl
                if fl.endswith('.dds'):
                    inf = dds_info(p)
                    if not inf:
                        continue
                    s = next((r[1][q] for r in rules if rel.startswith(r[0])), 0)
                    full[cat] += chain_bytes(*inf, 0)
                    skip[cat] += chain_bytes(*inf, min(-s, inf[2] - 1) if s < 0 else 0)
                    cnt[cat] += 1
                    if inf[3] >= 32:
                        unc[cat] += chain_bytes(*inf, 0)
                elif fl.endswith('.mesh'):
                    mesh_b[cat] += os.path.getsize(p)
                elif fl.endswith('.asset'):
                    t = open(p, encoding='utf-8', errors='ignore').read()
                    pdxmesh[cat] += len(re.findall(r'\bpdxmesh\s*=\s*\{', t))
    MB = 2 ** 20
    print(f'quality={a.quality} dlc={a.dlc} rules={len(rules)}')
    print(f'{"category":20s} {"dds":>6s} {"full MB":>9s} {"after skip":>10s} {"uncompr.":>9s} {".mesh MB":>9s} {"pdxmesh":>8s}')
    for c, _ in sorted(full.items(), key=lambda kv: -kv[1]):
        print(f'{c:20s} {cnt[c]:6d} {full[c]/MB:9.1f} {skip[c]/MB:10.1f} {unc[c]/MB:9.1f} {mesh_b[c]/MB:9.1f} {pdxmesh[c]:8d}')
    for c in set(mesh_b) - set(full):
        print(f'{c:20s} {0:6d} {0:9.1f} {0:10.1f} {0:9.1f} {mesh_b[c]/MB:9.1f} {pdxmesh[c]:8d}')
    print(f'{"TOTAL":20s} {sum(cnt.values()):6d} {sum(full.values())/MB:9.1f} {sum(skip.values())/MB:10.1f} '
          f'{sum(unc.values())/MB:9.1f} {sum(mesh_b.values())/MB:9.1f} {sum(pdxmesh.values()):8d}')


if __name__ == '__main__':
    main()
