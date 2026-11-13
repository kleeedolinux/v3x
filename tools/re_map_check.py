"""Offline anchor check for plugins/map_cache (mirrors resolve_all() in map_cache.c; no code runs).
Usage: python tools/re_map_check.py [path/to/victoria3.exe]
Prints each anchor's RVA and OK/FAIL; exit code 0 only if all 3 anchors resolve exactly like the plugin
would. Keep STR_CT / SHAPES / CT_RES_CALL / GOL_CT_CALL in sync with A[] in plugins/map_cache/map_cache.c."""
import os, re, struct, sys, bisect
import pefile

EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'binaries', 'victoria3.exe')
STR_CT = b'Could not find texture'
SHAPES = {
    'create': [(0x000, '48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 48 89 4C 24 08 55 41 54 41 55 41 56 41 57'),
               (0x02C, '41 8B F9 45 8B F0 4C 8B E2 4C 8B F9'),
               (0x204, '3B 5D DC 7E 0F 8B D3 48 8D 4D D0 E8')],
    'getload': [(0x000, '48 89 4C 24 08 53 56 57 48 83 EC 40 41 8B D8 48 8B F2 48 8B F9'),
                (0x035, '48 8D 0D ?? ?? ?? ?? E8'),
                (0x051, '44 8B CB 48 8B D6 48 8D 4C 24 78 E8'),
                (0x086, 'F0 0F C1 43 08 83 F8 01 75 1C 48 8B 4B 10 48 85 C9 74 09 48 8B 01 48 8B D3 FF 50 08 48 8B 03 48 8B CB FF 50 08')],
    'reserve': [(0x000, '48 89 5C 24 10 57 48 83 EC 20 48 8B D9 8B FA 3B 51 08 7E')],
}
CT_RES_CALL, GOL_CT_CALL = 0x20F, 0x05C
EXPECT_1151 = {'create': 0x3c5af20, 'getload': 0x3c5b4f0, 'reserve': 0x847fe0}

pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
tx = sec['.text']; TLO, THI = tx.VirtualAddress, tx.VirtualAddress + tx.Misc_VirtualSize
pd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
RF = sorted(struct.unpack_from('<III', img, pd.VirtualAddress + 12 * k) for k in range(pd.Size // 12))
RS = [r[0] for r in RF]


def function_start(rva):
    k = bisect.bisect_right(RS, rva) - 1
    if k < 0 or not (RF[k][0] <= rva < RF[k][1]): return None
    b, e, u = RF[k]
    for _ in range(32):
        if not ((img[u] >> 3) & 4): break
        b, e, u = struct.unpack_from('<III', img, u + 4 + ((img[u + 2] + 1) & ~1) * 2)
    return b


def match(at, pat):
    return all(t in ('?', '??') or img[at + i] == int(t, 16) for i, t in enumerate(pat.split()))


def shape_ok(fn, name):
    return fn is not None and all(match(fn + o, p) for o, p in SHAPES[name])


def find_string(s):
    for name in ('.rdata', '.data'):
        S = sec[name]; d = S.get_data()
        for m in re.finditer(re.escape(s + b'\0'), d):
            if m.start() == 0 or d[m.start() - 1] == 0: return S.VirtualAddress + m.start()
    return None


T = img[TLO:THI]
XPAT = re.compile(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')


def xrefs(target):
    return [TLO + m.start() for m in XPAT.finditer(T)
            if TLO + m.start() + 7 + struct.unpack_from('<i', T, m.start() + 3)[0] == target]


def rel32(at):
    return at + 5 + struct.unpack_from('<i', img, at + 1)[0]


found = {'create': None, 'getload': None, 'reserve': None}
s = find_string(STR_CT)
for x in (xrefs(s)[:16] if s else []):
    f = function_start(x)
    if shape_ok(f, 'create'): found['create'] = f; break
ct = found['create']
if ct:
    for m in re.finditer(b'\xe8', T):
        at = TLO + m.start()
        if at + 5 > THI or rel32(at) != ct: continue
        f = function_start(at)
        if f is not None and at == f + GOL_CT_CALL and shape_ok(f, 'getload'): found['getload'] = f; break
    r = rel32(ct + CT_RES_CALL)
    if shape_ok(r, 'reserve'): found['reserve'] = r

bad = 0
for k, v in found.items():
    note = '' if v is None or v == EXPECT_1151[k] else '  (differs from 1.15.1)'
    print(f"{k:8s} {'OK  ' if v else 'FAIL'} {v or 0:08x}{note}")
    bad += v is None
if ct: print(f"reserve return address checked by the plugin: {ct + CT_RES_CALL + 5:08x}")
print('ALL ANCHORS OK' if not bad else f'{bad} ANCHOR(S) FAILED')
sys.exit(1 if bad else 0)
