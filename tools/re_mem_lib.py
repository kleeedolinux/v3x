"""Memory-RE helpers for victoria3.exe (worker re-memory).
Usage:
  re_mem_lib.py <exe> calls RVA [RVA...]   count direct call/jmp rel32 sites to RVA (+ list first 20)
  re_mem_lib.py <exe> iat RVA [RVA...]     count `call/jmp [rip+X]` and `mov reg,[rip+X]` sites to an IAT slot
  re_mem_lib.py <exe> func RVA             print .pdata function range containing RVA
  re_mem_lib.py <exe> dis RVA [N]          disassemble N instructions from RVA (default: whole function)
  re_mem_lib.py <exe> str TEXT | 0xRVA...  string RVA(s) + lea/mov xrefs + enclosing functions
  re_mem_lib.py <exe> callers RVA          enclosing functions of all direct call sites (unique, counted)
"""
import sys, re, struct, bisect, collections
import pefile, capstone

exe = sys.argv[1]
pe = pefile.PE(exe, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
TB = text.VirtualAddress
T = text.get_data()
pdata = next(s for s in pe.sections if s.Name.startswith(b'.pdata'))
P = pdata.get_data()
FN = []
for i in range(0, len(P) - 12, 12):
    b, e, u = struct.unpack_from('<III', P, i)
    if b == 0: break
    FN.append((b, e, u))
FN.sort()
FB = [f[0] for f in FN]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
RIP_LEA = re.compile(b'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')
RIP_MOV = re.compile(b'[\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]')
RIP_CALL = re.compile(b'\xff[\x15\x25]')


def chained_root(idx):
    """Follow UNW_FLAG_CHAININFO to the primary function entry."""
    b, e, u = FN[idx]
    for _ in range(16):
        sec = next((s for s in pe.sections if s.VirtualAddress <= u < s.VirtualAddress + s.Misc_VirtualSize), None)
        if sec is None: break
        d = sec.get_data()
        o = u - sec.VirtualAddress
        if not ((d[o] >> 3) & 4): break
        cnt = d[o + 2]
        co = o + 4 + ((cnt + 1) & ~1) * 2
        b, e, u = struct.unpack_from('<III', d, co)
    return b, e


def func(rva):
    i = bisect.bisect_right(FB, rva) - 1
    if i < 0 or not (FN[i][0] <= rva < FN[i][1]): return None
    return chained_root(i)


def rel32_sites(target):
    out = []
    for op in (b'\xe8', b'\xe9'):
        for m in re.finditer(re.escape(op), T):
            i = m.start()
            if i + 5 > len(T): continue
            if TB + i + 5 + struct.unpack_from('<i', T, i + 1)[0] == target:
                out.append((TB + i, op == b'\xe8'))
    return out


def riprel_sites(target, pats=(RIP_CALL, RIP_MOV)):
    out = []
    for pat in pats:
        ln = 6 if pat is RIP_CALL else 7
        do = 2 if pat is RIP_CALL else 3
        for m in pat.finditer(T):
            i = m.start()
            if TB + i + ln + struct.unpack_from('<i', T, i + do)[0] == target: out.append(TB + i)
    return out


def dis(rva, n=None):
    f = func(rva)
    end = f[1] if (f and n is None) else rva + 16 * (n or 40)
    k = 0
    for ins in md.disasm(T[rva - TB:end - TB], rva):
        print(f"  {ins.address:08x}: {ins.bytes.hex():<22} {ins.mnemonic} {ins.op_str}")
        k += 1
        if n and k >= n: break


def fmt(x):
    f = func(x)
    return f"{x:08x}(fn {f[0]:08x})" if f else f"{x:08x}"


cmd = sys.argv[2]
args = sys.argv[3:]
if cmd == 'calls':
    for a in args:
        t = int(a, 16); s = rel32_sites(t)
        print(f"{t:08x}: {sum(1 for x in s if x[1])} call + {sum(1 for x in s if not x[1])} jmp sites")
        for x, c in s[:20]: print(f"   {'call' if c else 'jmp '} @ {fmt(x)}")
elif cmd == 'iat':
    for a in args:
        t = int(a, 16); s = riprel_sites(t)
        print(f"{t:08x}: {len(s)} rip-rel sites")
        fs = collections.Counter(func(x)[0] if func(x) else 0 for x in s)
        for f, c in fs.most_common(40): print(f"   fn {f:08x} x{c}")
elif cmd == 'func':
    f = func(int(args[0], 16)); print(f and (hex(f[0]), hex(f[1]), f[1] - f[0]))
elif cmd == 'dis':
    dis(int(args[0], 16), int(args[1]) if len(args) > 1 else None)
elif cmd == 'callers':
    t = int(args[0], 16); s = rel32_sites(t)
    fs = collections.Counter(func(x)[0] if func(x) else 0 for x, c in s)
    print(f"{t:08x}: {len(s)} sites in {len(fs)} functions")
    for f, c in fs.most_common(60): print(f"   fn {f:08x} x{c}")
elif cmd == 'str':
    if args[0].startswith('0x'):
        hits = [int(a, 16) for a in args]
    else:
        needle = args[0].encode() + b'\0'
        hits = []
        for sec in pe.sections:
            d = sec.get_data()
            for m in re.finditer(re.escape(needle), d):
                if m.start() and d[m.start() - 1] != 0: continue
                hits.append(sec.VirtualAddress + m.start())
    for r in hits:
        xs = riprel_sites(r, (RIP_LEA, RIP_MOV))
        print(f"str {r:08x}: xrefs " + ", ".join(fmt(x) for x in xs))
