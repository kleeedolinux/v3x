"""console_plus RE helper: locate the engine's transactional database getters.

Usage: re_console_db.py <victoria3.exe> <DatabaseClass> [...]   e.g. CCountryDefinitionDatabase

For each class: RTTI TypeDescriptor of CTransactionResource<unique_ptr<Class>> -> COL -> vtable;
the static object in .data whose first qword is ImageBase+vtable (statically initialised,
relocated); then every function that references obj with a
rip-relative lea (the getter template: TLS transaction scope -> slot -> DB*)."""
import re, struct, sys, bisect
import pefile

exe = sys.argv[1]
pe = pefile.PE(exe, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
img = pe.get_memory_mapped_image()
tx = sec['.text']; tva = tx.VirtualAddress; t = img[tva:tva + tx.Misc_VirtualSize]
rd = sec['.rdata']
pd = sec['.pdata'].get_data()
fs = sorted(struct.unpack_from('<II', pd, i) for i in range(0, len(pd) - 11, 12) if struct.unpack_from('<I', pd, i)[0])
starts = [a for a, _ in fs]
def func(r):
    k = bisect.bisect_right(starts, r) - 1
    return fs[k][0] if k >= 0 and fs[k][0] <= r < fs[k][1] else 0

def find_vtable(mangled):
    td_name = mangled.encode() + b'\0'
    p = img.find(td_name)
    if p < 0: return None
    td = p - 16
    rlo, rhi = rd.VirtualAddress, rd.VirtualAddress + rd.Misc_VirtualSize
    pk = struct.pack('<I', td)
    o = img.find(pk, rlo, rhi)
    while o >= 0:
        col = o - 12
        if col % 4 == 0 and struct.unpack_from('<I', img, col)[0] == 1 and struct.unpack_from('<I', img, col + 20)[0] == col:
            q = img.find(struct.pack('<Q', ib + col), rlo, rhi)
            if q >= 0: return q + 8
        o = img.find(pk, o + 1, rhi)
    return None

LEA = re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)
def lea_targets():
    d = {}
    for m in LEA.finditer(t):
        i = m.start(); T = tva + i + 7 + struct.unpack_from('<i', t, i + 3)[0]
        d.setdefault(T, []).append(tva + i)
    return d
leas = lea_targets()

for cls in sys.argv[2:]:
    mangled = f'.?AV?$CTransactionResource@V?$unique_ptr@V{cls}@@U?$default_delete@V{cls}@@@std@@@std@@@NPdxTransactions@@'
    vt = find_vtable(mangled)
    print(f'== {cls}: vtable {vt and hex(vt)}')
    if not vt: continue
    dd = sec['.data']; lo = dd.VirtualAddress; hi = lo + dd.Misc_VirtualSize
    o = img.find(struct.pack('<Q', ib + vt), lo, hi)
    while o >= 0:
        if o % 8 == 0:
            users = sorted({func(x) for x in leas.get(o, [])})
            print(f'   static object {o:08x}: getter(s) {[hex(u) for u in users][:12]}')
        o = img.find(struct.pack('<Q', ib + vt), o + 1, hi)
