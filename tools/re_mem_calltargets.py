"""One-pass histogram of direct call (E8) / jmp (E9) rel32 targets in .text, restricted to targets that
are .pdata function starts (filters most false positives). Writes JSON {target_rva: [calls, jmps]}.
Usage: re_mem_calltargets.py <exe> <out.json> [lo_rva hi_rva]   (prints top targets in [lo,hi))"""
import sys, json, struct, collections
import numpy as np
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
TB = text.VirtualAddress
T = np.frombuffer(text.get_data(), dtype=np.uint8)
P = pe.get_data(next(s for s in pe.sections if s.Name.startswith(b'.pdata')).VirtualAddress,
                next(s for s in pe.sections if s.Name.startswith(b'.pdata')).Misc_VirtualSize)
starts = np.frombuffer(P[:len(P) // 12 * 12], dtype=np.uint32).reshape(-1, 3)[:, 0]
starts = np.unique(starts[starts != 0])
res = {}
for op, slot in ((0xE8, 0), (0xE9, 1)):
    idx = np.nonzero(T[:-5] == op)[0]
    disp = (T[idx + 1].astype(np.int64) | (T[idx + 2].astype(np.int64) << 8) |
            (T[idx + 3].astype(np.int64) << 16) | (T[idx + 4].astype(np.int64) << 24))
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    tgt = TB + idx + 5 + disp
    ok = np.isin(tgt, starts)
    u, c = np.unique(tgt[ok], return_counts=True)
    for t, n in zip(u.tolist(), c.tolist()):
        res.setdefault(t, [0, 0])[slot] = n
json.dump({f"{k:08x}": v for k, v in res.items()}, open(sys.argv[2], 'w'))
lo, hi = (int(sys.argv[3], 16), int(sys.argv[4], 16)) if len(sys.argv) > 4 else (0, 1 << 40)
top = sorted(((v[0] + v[1], k, v) for k, v in res.items() if lo <= k < hi), reverse=True)[:40]
for n, k, v in top:
    print(f"{k:08x} calls={v[0]} jmps={v[1]}")
