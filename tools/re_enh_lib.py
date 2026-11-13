"""enhance RE helper (worker enhance). Reuses the cached index of re_sim_lib.Img.
Runs several commands in one process (loading the exe once):
  re_enh_lib.py <exe> "cmd args" ["cmd args" ...]
commands:
  dis RVA [N]      annotated disassembly (strings, rip-relative data, call targets);
                   without N: the whole .pdata function containing RVA
  str TEXT         xrefs + enclosing functions of an exact string
  rva RVA          xrefs (lea/mov rip-relative) to RVA
  callers RVA      direct call sites + enclosing functions
  strs RVA         strings referenced inside the function containing RVA
  fn RVA           function bounds
  q8 RVA [N]       N qwords at RVA (data sections, file content)
  ptr RVA          absolute 64-bit pointers to RVA in .rdata/.data (vtable slots, tables)
  anyref RVA       every instruction with a rip-relative operand to RVA (any opcode, verified by capstone)
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('RE_SIM_CACHE', 'D:/v3x_tmp/enhance/re_enh_index.pkl')  # never overwrite re_sim's cache
import capstone
from re_sim_lib import Img


def sec_of(im, rva):
    for n, s in im.secs.items():
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return n, s
    return None, None


def cstr(im, rva):
    n, s = sec_of(im, rva)
    if n not in ('.rdata', '.data'):
        return None
    d = s.get_data(); o = rva - s.VirtualAddress
    if o >= len(d):
        return None
    j = d.find(b'\0', o, o + 200)
    if j - o < 3:
        return None
    try:
        v = d[o:j].decode('ascii')
    except UnicodeDecodeError:
        return None
    return v if v.isprintable() else None


def ann(im, ins):
    out = ''
    if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16)
        out = f'  ; -> {t:08x}'
    if 'rip +' in ins.op_str or 'rip -' in ins.op_str:
        # compute target
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                s = cstr(im, t)
                n, _ = sec_of(im, t)
                out += f'  ; [{t:08x} {n}]' + (f' "{s[:90]}"' if s else '')
    return out


def dis(im, rva, n=None):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    if n is None:
        f = im.func(rva)
        s, e = f if f else (rva, rva + 600)
    else:
        s, e = rva, rva + n * 15
    k = 0
    for ins in md.disasm(im.tdata[s - im.tva:e - im.tva], s):
        print(f'{ins.address:08x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}{ann(im, ins)}')
        k += 1
        if n and k >= n:
            break


def anyref(im, target, quiet=False):
    """list of (rva, function start or 0, capstone insn) with a rip-relative operand == target"""
    import numpy as np
    out = []
    d = im.tdata; va = im.tva
    a = np.frombuffer(d[:len(d) - len(d) % 1], dtype=np.uint8)
    n = len(d) - 4
    disp = (a[0:n].astype(np.int64) | (a[1:n + 1].astype(np.int64) << 8) | (a[2:n + 2].astype(np.int64) << 16)
            | (a[3:n + 3].astype(np.int64) << 24))
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    pos = np.arange(n, dtype=np.int64) + va + 4
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    seen = set()
    for k in (0, 1, 2, 4):   # immediate bytes after disp32
        for i in np.nonzero(pos + disp + k == target)[0]:
            # try instruction starts 2..9 bytes before the disp
            for back in range(2, 10):
                st = int(i) - back
                if st < 0 or st in seen:
                    continue
                ins = next(md.disasm(d[st:st + 16], va + st), None)
                if ins is None or ins.size != back + 4 + k:
                    continue
                if any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP and
                       ins.address + ins.size + op.mem.disp == target for op in ins.operands):
                    seen.add(st); f = im.func(va + st)
                    out.append((va + st, f[0] if f else 0, ins))
                    if not quiet:
                        print(f'  {va + st:08x} fn {f[0] if f else 0:08x}: {ins.mnemonic} {ins.op_str}')
                    break
    return sorted(out, key=lambda r: r[0])


def main():
    exe = sys.argv[1]
    im = Img(exe)
    for line in sys.argv[2:]:
        a = line.split(None, 1)
        cmd, rest = a[0], (a[1] if len(a) > 1 else '')
        print(f'==== {line}')
        if cmd == 'dis':
            p = rest.split()
            dis(im, int(p[0], 16), int(p[1]) if len(p) > 1 else None)
        elif cmd == 'str':
            for t in im.find_string(rest):
                xs = im.xref.get(t, [])
                print(f'{t:08x}: ' + ', '.join(f'{x:08x}(fn {im.func(x)[0]:08x})' if im.func(x) else f'{x:08x}' for x in xs))
        elif cmd == 'rva':
            t = int(rest, 16)
            for x in im.xref.get(t, []):
                f = im.func(x); print(f'  {x:08x} fn {f[0]:08x}' if f else f'  {x:08x}')
        elif cmd == 'callers':
            t = int(rest, 16); cs = im.callsto.get(t, [])
            print(f'{t:08x}: {len(cs)} call sites')
            for c in cs[:60]:
                f = im.func(c); print(f'  {c:08x} in {f[0]:08x}' if f else f'  {c:08x}')
        elif cmd == 'strs':
            for x, t, s in im.strs_in(int(rest, 16), 3):
                print(f'{x:08x} -> {t:08x} {s[:120]}')
        elif cmd == 'fn':
            f = im.func(int(rest, 16)); print(f and f'{f[0]:08x}-{f[1]:08x} size {f[1]-f[0]}')
        elif cmd == 'ptr':
            t = int(rest, 16) + im.base; nd = struct.pack('<Q', t)
            for nm in ('.rdata', '.data'):
                sc = im.secs[nm]; dd = sc.get_data(); i = dd.find(nd)
                while i >= 0:
                    print(f'  {nm} {sc.VirtualAddress + i:08x}'); i = dd.find(nd, i + 1)
        elif cmd == 'anyref':
            anyref(im, int(rest, 16))
        elif cmd == 'q8':
            p = rest.split(); r = int(p[0], 16); n = int(p[1]) if len(p) > 1 else 8
            nm, s = sec_of(im, r); d = s.get_data(); o = r - s.VirtualAddress
            for i in range(n):
                v = struct.unpack_from('<Q', d, o + 8 * i)[0]
                print(f'{r + 8 * i:08x}: {v:016x}' + (f'  (rva {v - im.base:08x})' if im.base <= v < im.base + 0x7000000 else ''))


if __name__ == '__main__':
    main()
