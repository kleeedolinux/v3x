"""Rank the hottest victoria3.exe functions by self samples and classify their data-structure pattern.

Heuristic features from the disassembly (capstone) of each function body (.pdata range, or the
leaf block for no-.pdata rows): lock prefixes / pause (spin locks), calls to __RTDynamicCast
(0x43f0c64), FNV-1a constants (hash maps), crc32, rep movs/stos (memcpy/memset), sorted-vector
binary search (shr 1 + cmov halving loop), linear scans (loop with constant stride compare),
virtual calls, the MSVC thread-safe-static guard (gs:[0x58] + _Init_thread_header), strlen loops.
Prints a markdown table: rank, rva, self samples, self %, class, top caller, string hints.

Usage: re_ds_rank.py [--profile CSV] [--edges CSV] [--top 100] [--md]
"""
import argparse, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_ds_lib as L
import symbolize
import capstone

RTDYNCAST = 0x43f0c64
MI_NEW = 0x43d95c0
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def body(rva):
    """(start, end, is_block). devtools reports a function start, or a 64-byte block (rva & 63 == 0,
    not a .pdata begin) when the sample had no usable unwind info (leaf code)."""
    f = L.func(rva)
    if f and f[0] == rva:
        return f[0], f[1], False
    if rva & 63 == 0 or not f:
        return rva, rva + 0x40, True
    return f[0], f[1], False


def features(b, e, block):
    """block: the 64 sampled bytes (decoded from the start of the leaf function that contains them);
    function: from its start to the first int3 padding (covers the code after a short .pdata range)."""
    if block:
        d = L.img().read(b - 0x40, 0x40)
        st = 0
        for i in range(0x3f, 0, -1):
            if d[i] == 0xcc:
                st = i + 1
                break
        start, end = b - 0x40 + st, e
    else:
        start, end = b, max(e, b + 0x40)
    ins = []
    for i in MD.disasm(L.img().read(start, min(end - start + 0x400, 8192)), start):
        if i.mnemonic == 'int3' or (i.address >= end and i.mnemonic in ('ret', 'jmp')):
            ins.append(i)
            break
        ins.append(i)
    f = dict(n=len(ins), lock=0, pause=0, dyncast=0, fnv=0, crc=0, rep=0, bsearch=0, vcall=0,
             tls_static=0, strlen=0, back=0, call_new=0, xadd=0, bytecmp=0)
    prev = None
    for i in ins:
        m, o = i.mnemonic, i.op_str
        if 'lock' in m or m.startswith('xchg'):
            f['lock'] += 1
        if 'xadd' in m:
            f['xadd'] += 1
        if m == 'pause':
            f['pause'] += 1
        if m.startswith('rep'):
            f['rep'] += 1
        if m == 'crc32':
            f['crc'] += 1
        if '0x811c9dc5' in o or '0x1000193' in o or '0x100000001b3' in o:
            f['fnv'] += 1
        if m == 'call':
            if o == hex(RTDYNCAST):
                f['dyncast'] += 1
            if o == hex(MI_NEW):
                f['call_new'] += 1
            if o.startswith('qword ptr [r') and '+' in o:
                f['vcall'] += 1
        if m == 'jmp' and o.startswith('r') and prev and prev.mnemonic == 'mov':
            f['vcall'] += 1
        if m == 'shr' and o.endswith(', 1'):
            f['bsearch'] += 1
        if 'gs:[0x58]' in o:
            f['tls_static'] += 1
        if m == 'cmp' and o.startswith('byte ptr') and o.endswith(', 0'):
            f['strlen'] += 1
        if m == 'cmp' and 'byte ptr' in o and not o.endswith(', 0'):
            f['bytecmp'] += 1
        if m.startswith('j') and m != 'jmp' and o.startswith('0x'):
            try:
                if int(o, 16) < i.address:
                    f['back'] += 1
            except ValueError:
                pass
        prev = i
    return f


def classify(f, size):
    if f['dyncast']:
        return 'RTTI dynamic_cast'
    if f['pause'] and f['lock']:
        return 'spin lock / wait'
    if f['lock'] and f['xadd']:
        return 'atomic refcount / RW lock'
    if f['rep'] and f['n'] < 20:
        return 'memcpy/memset'
    if f['crc']:
        return 'hash (crc32)'
    if f['fnv']:
        return 'hash map probe (FNV-1a)'
    if f['bsearch'] and f['back']:
        return 'sorted-vector binary search'
    if f['tls_static'] and f['n'] < 40:
        return 'static singleton getter'
    if f['strlen'] and f['back'] and f['n'] < 40:
        return 'strlen / string scan'
    if f['bytecmp'] and f['back']:
        return 'string/byte compare loop'
    if f['back'] and f['n'] < 60:
        return 'linear scan / small loop'
    if f['vcall'] and f['n'] < 40:
        return 'virtual dispatch thunk'
    if f['n'] < 12:
        return 'trivial getter'
    return 'logic'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--profile', default=L.PROFILE)
    ap.add_argument('--edges', default=L.EDGES)
    ap.add_argument('--top', type=int, default=100)
    a = ap.parse_args()
    fn = L.load_profile(a.profile)
    down, up = L.load_edges(a.edges)
    total = sum(s for s, _ in fn.values()) or 1
    rows = sorted(fn.items(), key=lambda kv: -kv[1][0])[:a.top]
    print(f'# {os.path.basename(a.profile)}: {len(fn)} exe functions, {total} exe self samples\n')
    print('| # | rva | self | self% | size | class | top caller (edge samples) | hints |')
    print('|---|---|---|---|---|---|---|---|')
    for k, (rva, (s, inc)) in enumerate(rows, 1):
        b, e, blk = body(rva)
        f = features(b, e, blk)
        cls = classify(f, e - b)
        callers = up.get(rva, [])
        top = ''
        if callers:
            cs, c = callers[0]
            top = f'{c:x} ({cs})' if not isinstance(c, tuple) else f'{c[0]}+{c[1]:x}'
        h = [] if blk else symbolize.hints(L.img(), b, e, 2)
        hint = ' '.join(h)[:80].replace('|', '/')
        print(f'| {k} | {rva:x}{" (64B block)" if blk else ""} | {s} | {100.0 * s / total:.2f} | {"-" if blk else e - b} | {cls} | {top} | {hint} |')


if __name__ == '__main__':
    main()
