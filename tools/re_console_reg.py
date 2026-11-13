"""console_plus RE helper: every runtime console command registration (call to
CConsoleCmdManager::Register, found as the function called with the manager singleton under the
exclusive lock) and what it puts in the SCommandData on the stack: OnExecute (+0xb0), autocomplete
callback (+0xb8) and the strings the registering code references (name, description, arg names).

Usage: re_console_reg.py <victoria3.exe> <register_rva_hex> [--cb]
       (1.15.1: register = 3dc2ad0)"""
import bisect, re, struct, sys
import pefile, capstone
from capstone import x86

exe, REG = sys.argv[1], int(sys.argv[2], 16)
only_cb = '--cb' in sys.argv
pe = pefile.PE(exe, fast_load=True)
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
tx = sec['.text']; t = tx.get_data(); tva = tx.VirtualAddress
pd = sec['.pdata'].get_data()
fs = sorted(struct.unpack_from('<II', pd, i) for i in range(0, len(pd) - 11, 12) if struct.unpack_from('<I', pd, i)[0])
starts = [a for a, _ in fs]
def func(r):
    k = bisect.bisect_right(starts, r) - 1
    return fs[k] if k >= 0 and fs[k][0] <= r < fs[k][1] else None
def in_text(r): return tva <= r < tva + len(t)
def cstr(r):
    for n in ('.rdata', '.data'):
        s = sec[n]; d = s.get_data(); o = r - s.VirtualAddress
        if 0 <= o < len(d):
            j = d.find(b'\0', o, o + 300)
            if j <= o: return None
            try: v = d[o:j].decode('ascii')
            except UnicodeDecodeError: return None
            return v if v.isprintable() else None
    return None

sites = []
for m in re.finditer(rb'\xe8', t):
    i = m.start()
    if i + 5 <= len(t) and tva + i + 5 + struct.unpack_from('<i', t, i + 1)[0] == REG:
        sites.append(tva + i)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
byfn = {}
for s in sites:
    f = func(s)
    if f: byfn.setdefault(f, []).append(s)
total = withcb = 0
for (fs0, fe0), ss in sorted(byfn.items()):
    ins = list(md.disasm(t[fs0 - tva:fe0 - tva], fs0))
    # rbp = rsp_new + delta (lea rbp,[rsp+d] before sub rsp,N)
    delta = None; d0 = None
    for k in ins[:20]:
        if k.mnemonic == 'lea' and k.op_str.startswith('rbp, [rsp'):
            d0 = k.operands[1].mem.disp
        if k.mnemonic == 'sub' and k.op_str.startswith('rsp,') and d0 is not None:
            delta = k.operands[1].imm + d0
    def norm(op):
        if op.type != x86.X86_OP_MEM: return None
        b = op.mem.base
        if b == x86.X86_REG_RSP: return op.mem.disp
        if b == x86.X86_REG_RBP and delta is not None: return op.mem.disp + delta
        return None
    prev = 0
    idx = {k.address: n for n, k in enumerate(ins)}
    for s in ss:
        e = idx.get(s)
        if e is None: continue
        win = ins[prev:e]; prev = e + 1
        B = None
        for k in reversed(win[-6:]):
            if k.mnemonic == 'lea' and k.op_str.startswith('rdx,'):
                B = norm(k.operands[1]); break
        lea_reg = {}; stores = {}; strs = []
        for k in win:
            if k.mnemonic in ('lea', 'mov', 'vmovups', 'vmovsd', 'movzx', 'vmovdqu') and len(k.operands) == 2:
                src = k.operands[1]
                if src.type == x86.X86_OP_MEM and src.mem.base == x86.X86_REG_RIP:
                    T = k.address + k.size + src.mem.disp
                    if k.mnemonic == 'lea' and in_text(T):
                        lea_reg[k.operands[0].reg] = T
                    else:
                        c = cstr(T)
                        if c and (not strs or not strs[-1].endswith(c)): strs.append(c)
                dst = k.operands[0]
                if k.mnemonic == 'mov' and dst.type == x86.X86_OP_MEM and src.type == x86.X86_OP_REG and src.reg in lea_reg:
                    o = norm(dst)
                    if o is not None: stores[o] = lea_reg[src.reg]
        total += 1
        ex = stores.get(B + 0xb0) if B is not None else None
        cb = stores.get(B + 0xb8) if B is not None else None
        if cb: withcb += 1
        if only_cb and not cb: continue
        print(f'{s:08x} exec={(ex or 0):08x} cb={(cb or 0):08x} | ' + ' | '.join(x[:60] for x in strs[:6]))
print(f'# {total} registrations, {withcb} with an autocomplete callback', file=sys.stderr)
