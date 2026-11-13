"""Asset-streaming RE helpers for victoria3.exe (worker asset-streaming).
Reuses the cached xref index of re_sim_lib.py (tools/re_sim_index.pkl).
Usage:
  re_stream_lib.py <exe> dis RVA [N]        disassemble from RVA (skipdata; whole .pdata function if N omitted)
  re_stream_lib.py <exe> xs RVA             strings / data referenced (lea/mov rip) inside the function at RVA
  re_stream_lib.py <exe> str TEXT...        string RVAs + xrefs + enclosing functions
  re_stream_lib.py <exe> callers RVA        direct E8 call sites to RVA with enclosing functions
  re_stream_lib.py <exe> rtti PREFIX        RTTI classes (primary vtables) whose name starts with PREFIX
"""
import os, re, struct, sys, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone
from re_sim_lib import Img  # noqa: E402

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.skipdata = True


def fn_of(I, rva):
    i = bisect.bisect_right(I.fstart, rva) - 1
    if i >= 0 and I.fstart[i] <= rva < I.fend[i]:
        return I.fstart[i], I.fend[i]
    return None


def read(I, rva, n):
    return I.pe.get_data(rva, n)


def cstr(I, rva, n=200):
    try:
        b = read(I, rva, n)
    except Exception:
        return None
    z = b.find(b'\0')
    if z < 4:
        return None
    s = b[:z]
    if all(32 <= c < 127 for c in s):
        return s.decode()
    return None


def dis(I, rva, n=None):
    f = fn_of(I, rva)
    end = f[1] if (f and n is None) else rva + 16 * (n or 60)
    code = read(I, rva, end - rva)
    k = 0
    for ins in md.disasm(code, rva):
        extra = ''
        m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
        if m:
            t = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1))
            s = cstr(I, t)
            extra = f'   ; {t:08x}' + (f' "{s[:90]}"' if s else '')
        print(f'{ins.address:08x}: {ins.mnemonic} {ins.op_str}{extra}')
        k += 1
        if n is not None and k >= n:
            break


def xs(I, rva):
    f = fn_of(I, rva)
    if not f:
        print('no function'); return
    code = read(I, f[0], f[1] - f[0])
    for ins in md.disasm(code, f[0]):
        m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
        if m:
            t = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1))
            s = cstr(I, t)
            print(f'{ins.address:08x} -> {t:08x} {ins.mnemonic} ' + (f'"{s[:100]}"' if s else ''))
        elif ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
            print(f'{ins.address:08x} call {ins.op_str}')


def find_str(I, s):
    out = []
    needle = s.encode() + b'\0'
    for sec in I.pe.sections:
        d = sec.get_data()
        for m in re.finditer(re.escape(needle), d):
            if m.start() == 0 or d[m.start() - 1] == 0:
                out.append(sec.VirtualAddress + m.start())
    return out


def main():
    I = Img(sys.argv[1])
    cmd, args = sys.argv[2], sys.argv[3:]
    if cmd == 'dis':
        dis(I, int(args[0], 16), int(args[1]) if len(args) > 1 else None)
    elif cmd == 'xs':
        xs(I, int(args[0], 16))
    elif cmd == 'str':
        for s in args:
            for r in find_str(I, s):
                for x in I.xref.get(r, []):
                    print(f'{s!r} @{r:08x} xref {x:08x} fn {fn_of(I, x)}')
                if not I.xref.get(r):
                    print(f'{s!r} @{r:08x} (no lea/mov xref)')
    elif cmd == 'callers':
        t = int(args[0], 16)
        for x in I.callsto.get(t, []):
            f = fn_of(I, x)
            print(f'{x:08x} in {f[0]:08x}' if f else f'{x:08x}')
    elif cmd == 'rtti':
        import subprocess  # noqa
        print('use tools/rtti_dump.py --prefix', args[0])


if __name__ == '__main__':
    main()
