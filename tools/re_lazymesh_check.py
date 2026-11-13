"""Offline anchor check for plugins/lazy_mesh (mirrors resolve_all() in lazy_mesh.c; no code runs).
Usage: python tools/re_lazymesh_check.py [path/to/victoria3.exe]
Prints each anchor's RVA and OK/FAIL; exit code 0 only if all 6 anchors resolve exactly like the plugin
would. Keep the tables below in sync with A[] / STR_* / LOAD_RDATA_CALL in plugins/lazy_mesh/lazy_mesh.c."""
import os, re, struct, sys, bisect
import pefile

EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'binaries', 'victoria3.exe')
STR_LOAD = b'gfx/models/mesh_not_found_fallback_object.mesh'
STR_ANIM = b'Could not find animation set {} declared as import in mesh type {}'
STR_BLEND = b'Could not find blend shape "{}" for attribute "{}" in entity "{}"'
RTTI_MESHTYPE = b'.?AVCMeshType@NPdxMesh@@'
RTTI_MESHOBJECT = b'.?AVCMeshObject@NPdxMesh@@'
SHAPES = {
    'load':  [(0x00, '40 53 56 57 41 54 41 55 41 56 41 57'), (0x46, '4C 8B E9'), (0x72, '41 39 5D 4C 0F 85')],
    'anim':  [(0x00, '48 89 4C 24 08 55 53 56 57 41 54'), (0x2D, '48 8D B1 48 01 00 00'), (0x44, '4C 39 61 38 0F 84')],
    'pass':  [(0x00, '48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B DA E8'), (0x20, 'E8'), (0x60, '48 8B 4B 28 E8')],
    'ctor':  [(0x00, '48 89 5C 24 20 48 89 4C 24 08 55 56 57 41 54 41'), (0x62, '48 89 51 50'), (0x71, '48 8D 05 ?? ?? ?? ?? 48 89 01')],
    'rdata': [(0x00, '48 89 54 24 10 48 89 4C 24 08 53 41 56 41 57'), (0x13, '48 8B 1A 4C 8B FA 4C 8B F1')],
    'blend': [(0x00, '48 89 5C 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57'), (0x21, '4D 8B E9 49 8B F0 48 8B DA 4C 8B F1')],
}
LOAD_RDATA_CALL = '49 8D 95 D0 01 00 00 49 8B CD E8'
LOAD_BODY_SPAN = 0x1C00
EXPECT_1151 = {'load': 0x3adcfd0, 'anim': 0x3ae1400, 'pass': 0x3ab3f10, 'ctor': 0x3ad80a0, 'rdata': 0x3ad7860, 'blend': 0x37a9520}

pe = pefile.PE(EXE, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
tx = sec['.text']; TLO, THI = tx.VirtualAddress, tx.VirtualAddress + tx.Misc_VirtualSize
rd = sec['.rdata']; RLO, RHI = rd.VirtualAddress, rd.VirtualAddress + rd.Misc_VirtualSize

pd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
RF = sorted(struct.unpack_from('<III', img, pd.VirtualAddress + 12 * k) for k in range(pd.Size // 12))
RS = [r[0] for r in RF]


def function_start(rva):
    k = bisect.bisect_right(RS, rva) - 1
    if k < 0 or not (RF[k][0] <= rva < RF[k][1]): return None
    b, e, u = RF[k]
    for _ in range(32):  # follow UNW_FLAG_CHAININFO like the loader's function_start
        if not ((img[u] >> 3) & 4): break
        co = u + 4 + ((img[u + 2] + 1) & ~1) * 2
        b, e, u = struct.unpack_from('<III', img, co)
    return b


def match(rva, off, pat):
    for i, tok in enumerate(pat.split()):
        if tok in ('?', '??'): continue
        if img[rva + off + i] != int(tok, 16): return False
    return True


def shape_ok(fn, name):
    return fn is not None and all(match(fn, o, p) for o, p in SHAPES[name])


def find_string(s):
    for name in ('.rdata', '.data'):
        S = sec[name]; d = S.get_data()
        for m in re.finditer(re.escape(s + b'\0'), d):
            if m.start() == 0 or d[m.start() - 1] == 0: return S.VirtualAddress + m.start()
    return None


T = img[TLO:THI]
XPAT = re.compile(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')


def xrefs(target):
    out = []
    for m in XPAT.finditer(T):
        i = m.start()
        if TLO + i + 7 + struct.unpack_from('<i', T, i + 3)[0] == target: out.append(TLO + i)
    return out


def by_string(s, name):
    r = find_string(s)
    if r is None: return None
    for x in xrefs(r)[:16]:
        f = function_start(x)
        if shape_ok(f, name): return f
    return None


def vtable(mangled):
    td = img.find(b'\0' + mangled + b'\0')
    if td < 0: return None
    td = td + 1 - 16
    for c in range(RLO & ~3, RHI - 24, 4):
        if struct.unpack_from('<I', img, c)[0] == 1 and struct.unpack_from('<I', img, c + 12)[0] == td \
                and struct.unpack_from('<I', img, c + 20)[0] == c and struct.unpack_from('<I', img, c + 4)[0] == 0:
            i = img.find(struct.pack('<Q', IB + c), RLO, RHI)
            return i + 8 if i >= 0 else None
    return None


def rel32(at, ln):
    return at + ln + struct.unpack_from('<i', img, at + ln - 4)[0]


res = {}
vt_type, vt_obj = vtable(RTTI_MESHTYPE), vtable(RTTI_MESHOBJECT)
f = by_string(STR_LOAD, 'load')
if f is not None and (vt_type is None or struct.unpack_from('<Q', img, vt_type + 8 * 6)[0] - IB != f): f = None
res['load'] = f
res['anim'] = by_string(STR_ANIM, 'anim')
res['pass'] = None
if res['anim'] is not None:
    for m in re.finditer(rb'\xe8', T):
        i = TLO + m.start()
        if rel32(i, 5) != res['anim']: continue
        fn = function_start(i)
        if fn is not None and i == fn + 0x64 and shape_ok(fn, 'pass'): res['pass'] = fn; break
res['ctor'] = None
if vt_obj is not None:
    for x in xrefs(vt_obj)[:8]:
        fn = function_start(x)
        if fn is not None and x == fn + 0x71 and shape_ok(fn, 'ctor') and rel32(x, 7) == vt_obj: res['ctor'] = fn; break
res['rdata'] = None
if res['load'] is not None:
    for o in range(LOAD_BODY_SPAN - 16):
        if match(res['load'], o, LOAD_RDATA_CALL):
            t = rel32(res['load'] + o + 10, 5)
            if shape_ok(t, 'rdata'): res['rdata'] = t
            break
res['blend'] = by_string(STR_BLEND, 'blend')

bad = 0
for k in ('load', 'anim', 'pass', 'ctor', 'rdata', 'blend'):
    v = res[k]
    note = '' if v is None or v == EXPECT_1151[k] else '  (differs from 1.15.1)'
    print(f"{k:6s} {'OK  ' if v is not None else 'FAIL'} {v if v is not None else 0:08x}{note}")
    bad += v is None
print(f"vtables: CMeshType {vt_type or 0:08x}  CMeshObject {vt_obj or 0:08x}")
print('ALL ANCHORS OK' if not bad else f'{bad} ANCHOR(S) FAILED -> lazy_mesh fails closed (lazy needs all 6)')
sys.exit(1 if bad else 0)
