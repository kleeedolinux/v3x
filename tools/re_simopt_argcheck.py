"""Count the stack arguments a hooked function really takes (Win64: args 5+ live at [rsp+0x20..]
of the caller at the call, i.e. [rsp+0x28..] at callee entry). A 4-arg pass-through detour on such a
function hands the original garbage for arg 5+ (sim_opt diag_trades crash, 2026-11-13).

Two independent signals per target RVA:
  caller side: for every direct `call T` (E8), scan back up to 24 instructions (stopping at the
               previous call/ret/jmp) for stores to [rsp+0x20/0x28/0x30/0x38] -> that many stack args.
  callee side: track rsp from the entry through push/sub/lea-rbp prologue and flag any read of an
               entry-relative slot >= +0x28 (above the 4 home slots) in the whole .pdata function.
Usage: re_simopt_argcheck.py [EXE] RVA[=name] ...
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_sim_lib import Img
import capstone
import symbolize

MEM = re.compile(r'\[(rsp|rbp)(?: \+ (0x[0-9a-f]+|\d+))?(?: - (0x[0-9a-f]+|\d+))?\]')


def caller_side(im, md, t):
    worst = 0; sites = 0
    for c in im.callsto.get(t, []):
        f = im.func(c)
        if not f:
            continue
        sites += 1
        start = max(f[0], c - 0x90)
        ins = [i for i in md.disasm(im.tdata[start - im.tva:c - im.tva], start)]
        # resync: keep only instructions that end exactly at the call
        if not ins or ins[-1].address + ins[-1].size != c:
            continue
        for i in reversed(ins[-24:]):
            if i.mnemonic in ('call', 'ret', 'jmp'):
                break
            if i.mnemonic.startswith('mov') or i.mnemonic.startswith('vmov'):
                m = re.match(r'(?:\w+ ptr )?\[rsp \+ (0x[0-9a-f]+)\], ', i.op_str)
                if m:
                    off = int(m.group(1), 16)
                    if off in (0x20, 0x28, 0x30, 0x38):
                        worst = max(worst, (off - 0x20) // 8 + 1)
    return sites, worst


def callee_side(im, md, t):
    f = im.func(t)
    if not f:
        return None
    delta = 0; rbp = None; hits = []; eax_imm = 0
    for n, i in enumerate(md.disasm(im.tdata[f[0] - im.tva:f[1] - im.tva], f[0])):
        if n < 40:   # prologue tracking
            if i.mnemonic == 'push':
                delta += 8
            elif i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x'):
                eax_imm = int(i.op_str.split(', ')[1], 0)
            elif i.mnemonic == 'sub' and i.op_str == 'rsp, rax':
                delta += eax_imm          # __chkstk frames: mov eax,N; call chkstk; sub rsp,rax
            elif i.mnemonic == 'sub' and i.op_str.startswith('rsp, '):
                try:
                    delta += int(i.op_str.split(', ')[1], 0)
                except ValueError:
                    pass
            elif i.mnemonic == 'lea' and i.op_str.startswith('rbp, [rsp'):
                m = MEM.search(i.op_str)
                k = int(m.group(2), 0) if m and m.group(2) else -(int(m.group(3), 0) if m and m.group(3) else 0)
                rbp = delta - k            # rbp = entry_rsp - (delta - k)
            elif i.mnemonic == 'mov' and i.op_str == 'rbp, rsp':
                rbp = delta
        for m in MEM.finditer(i.op_str):
            base = m.group(1); k = int(m.group(2), 0) if m.group(2) else -(int(m.group(3), 0) if m.group(3) else 0)
            if base == 'rsp':
                rel = k - delta
            else:
                if rbp is None:
                    continue
                rel = k - rbp
            if rel >= 0x28 and not i.mnemonic.startswith('lea'):
                # only reads (destination-first syntax: source operand is the 2nd for mov)
                ops = i.op_str.split(', ')
                if len(ops) == 2 and m.group(0) in ops[1]:
                    hits.append((i.address, i.mnemonic, i.op_str, rel))
    return hits[:4]


def main():
    args = sys.argv[1:]
    exe = args.pop(0) if args and not re.match(r'^(0x)?[0-9a-fA-F]+(=.*)?$', args[0]) else symbolize.DEFAULT_EXE
    im = Img(exe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for a in args:
        h, _, name = a.partition('=')
        t = int(h, 16)
        sites, worst = caller_side(im, md, t)
        hits = callee_side(im, md, t)
        verdict = 'STACK ARGS' if worst or hits else 'ok (<=4 args)'
        print(f"{t:08x} {name:28s} direct call sites {sites:3d}, max stack args stored by callers {worst}, "
              f"callee reads above home: {len(hits) if hits is not None else '?'} -> {verdict}")
        for x in hits or []:
            print(f"      {x[0]:08x} {x[1]} {x[2]}  (entry+{x[3]:#x})")


if __name__ == '__main__':
    main()
