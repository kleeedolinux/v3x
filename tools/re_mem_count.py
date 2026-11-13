"""Count raw E8/E9 rel32 sites to given RVAs (no .pdata filter; works for leaf functions).
Usage: re_mem_count.py <exe> RVA [RVA...]"""
import sys, numpy as np, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
TB = text.VirtualAddress
T = np.frombuffer(text.get_data(), dtype=np.uint8)
tg = {}
for op in (0xE8, 0xE9):
    idx = np.nonzero(T[:-5] == op)[0]
    disp = T[idx + 1].astype(np.int64) | (T[idx + 2].astype(np.int64) << 8) | (T[idx + 3].astype(np.int64) << 16) | (T[idx + 4].astype(np.int64) << 24)
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    tg[op] = TB + idx + 5 + disp
for a in sys.argv[2:]:
    t = int(a, 16)
    print(f"{t:08x}: call={int((tg[0xE8] == t).sum())} jmp={int((tg[0xE9] == t).sum())}")
