"""Linear disassembly of N bytes at RVA (ignores .pdata). Usage: re_threads_raw.py <exe> RVA NBYTES [RVA NBYTES...]"""
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1], fast_load=True); pe.parse_data_directories(directories=[1])
IB = pe.OPTIONAL_HEADER.ImageBase
iat = {i.address - IB: i.name.decode() for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports if i.name}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
a = sys.argv[2:]
for k in range(0, len(a), 2):
    r, n = int(a[k], 16), int(a[k + 1], 16)
    print(f"; raw {r:08x}+{n:#x}")
    for ins in md.disasm(pe.get_data(r, n), r):
        note = ''
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                note = f' ; [{iat[t]}]' if t in iat else f' ; ->{t:08x}'
        print(f"{ins.address:08x}  {ins.mnemonic:6s} {ins.op_str}{note}")
