"""Find any rip-relative disp32 in .text resolving to RVA (any opcode; insn end = disp end + 0..4).
Usage: re_hist_anyref.py <exe> HEXRVA..."""
import sys, struct, numpy as np, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
t = next(s for s in pe.sections if s.Name.startswith(b'.text'))
d = np.frombuffer(t.get_data(), dtype=np.uint8)
va = t.VirtualAddress
n = len(d) - 4
disp = (d[0:n].astype(np.int64) | (d[1:n+1].astype(np.int64) << 8) | (d[2:n+2].astype(np.int64) << 16) | (d[3:n+3].astype(np.int64) << 24))
disp = np.where(disp >= 2**31, disp - 2**32, disp)
pos = np.arange(n, dtype=np.int64) + va
for h in sys.argv[2:]:
    tgt = int(h, 16)
    for k in range(0, 5):
        hits = np.nonzero(pos + 4 + k + disp == tgt)[0]
        for i in hits[:20]:
            print(f"{tgt:08x}: disp at {va+i:08x} (insn end +{k})")
