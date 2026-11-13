"""List every rip-relative reference to the debug_mode flag byte (default exe+0x5c0efe7), with the
enclosing function, writer/reader, strings used in the function, direct callers, and the samples of
that function in a devtools profile csv (inclusive = on the stack).
Usage: re_enh_debug.py <exe> [flag_rva_hex] [profile.csv]"""
import csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_enh_lib as L

exe = sys.argv[1]
flag = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x5c0efe7
prof = {}
if len(sys.argv) > 3:
    for r in csv.DictReader(open(sys.argv[3])):
        if r['rva'].startswith('0x') and r['module'] == 'victoria3.exe':
            prof[int(r['rva'], 16)] = (int(r['samples']), int(r['inclusive']))
im = L.Img(exe)
refs = L.anyref(im, flag, quiet=True)
print(f'{len(refs)} references to {flag:08x}')
for a, f, ins in refs:
    w = 'WRITE' if ins.mnemonic.startswith(('mov byte', 'set')) or (ins.mnemonic == 'mov' and ins.op_str.startswith('byte ptr [rip')) or ins.mnemonic.startswith('set') else 'read'
    fs = im.func(a)
    size = fs[1] - fs[0] if fs else 0
    strs = [s for _, _, s in im.strs_in(a, 4)][:6] if fs else []
    callers = im.callsto.get(f, [])
    cf = sorted({im.func(c)[0] for c in callers if im.func(c)})
    p = prof.get(f, (0, 0))
    print(f'{a:08x} {w:5} fn {f:08x} size {size:5} self {p[0]:7} incl {p[1]:8} callers {len(callers):3} '
          f'{[hex(x) for x in cf[:4]]} | {ins.mnemonic} {ins.op_str}')
    for s in strs:
        print(f'          "{s[:110]}"')
