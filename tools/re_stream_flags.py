"""For each direct call to a texture-load function, print the immediate loaded into r9d (flags)
in the preceding instructions, plus the caller function and nearest referenced string.
Usage: re_stream_flags.py <exe> <target RVA hex> [reg regex, default r9d|r9b|r9]"""
import sys, os, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_stream_lib import Img, md, fn_of, read, cstr

I = Img(sys.argv[1]); t = int(sys.argv[2], 16)
REG = (sys.argv[3] if len(sys.argv) > 3 else 'r9d|r9b|r9') + '$'
hist = collections.Counter()
for site in sorted(I.callsto.get(t, [])):
    f = fn_of(I, site)
    lo = max(f[0] if f else site - 0x60, site - 0x60)
    ins = list(md.disasm(read(I, lo, site - lo + 5), lo))
    flags = '?'; s = ''
    for i in ins:
        if i.address >= site: break
        if re.match(REG, i.op_str.split(',')[0]) and i.mnemonic in ('mov', 'xor', 'lea', 'or'):
            flags = i.mnemonic + ' ' + i.op_str
        m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', i.op_str)
        if m:
            tt = i.address + i.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
            c = cstr(I, tt)
            if c: s = c
    hist[flags] += 1
    print(f'{site:08x} fn {f[0] if f else 0:08x} flags[{flags}] str[{s[:70]}]')
print(hist.most_common())
