"""List string literals (lea rip-rel into .rdata that decode as ASCII) and direct calls inside a function.
Usage: re_mem_fnstr.py <exe> FN_RVA [FN_RVA...]"""
import sys, struct, capstone, pefile, bisect
pe = pefile.PE(sys.argv[1], fast_load=True)
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
TB = text.VirtualAddress; T = text.get_data()
pd = next(s for s in pe.sections if s.Name.startswith(b'.pdata')).get_data()
FN = sorted(struct.unpack_from('<II', pd, i) for i in range(0, len(pd) - 12, 12) if struct.unpack_from('<I', pd, i)[0])
FB = [f[0] for f in FN]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = False
def s_at(rva):
    try: b = pe.get_data(rva, 160)
    except Exception: return None
    z = b.split(b'\0')[0]
    if len(z) >= 4 and all(32 <= c < 127 for c in z): return z.decode()
    return None
for a in sys.argv[2:]:
    f = int(a, 16); i = bisect.bisect_right(FB, f) - 1; end = FN[i][1]
    print(f"== fn {f:08x}-{end:08x}")
    for ins in md.disasm(T[f - TB:end - TB], f):
        if ins.mnemonic == 'lea' and 'rip' in ins.op_str:
            d = struct.unpack_from('<i', ins.bytes, 3)[0]; r = ins.address + ins.size + d
            s = s_at(r)
            if s: print(f"  {ins.address:08x} str {r:08x} {s[:100]!r}")
        elif ins.mnemonic == 'call' and ins.op_str.startswith('qword ptr [rip'):
            d = struct.unpack_from('<i', ins.bytes, 2)[0]; r = ins.address + 6 + d
            print(f"  {ins.address:08x} icall [{r:08x}]")
