"""For each direct caller function of the given RVA(s), print function range and a few strings it references.
Usage: re_sim_callerinfo.py <exe> HEX [HEX...] [-n N]"""
import sys
from re_sim_lib import Img

BS = chr(92)
exe, *hs = sys.argv[1:]
n = 4
if '-n' in hs:
    i = hs.index('-n'); n = int(hs[i + 1]); hs = hs[:i] + hs[i + 2:]
im = Img(exe)
for h in hs:
    t = int(h, 16); seen = set()
    print(f"== {t:08x}: {len(im.callsto.get(t, []))} call sites")
    for c in im.callsto.get(t, []):
        f = im.func(c)
        if not f or f[0] in seen:
            continue
        seen.add(f[0])
        ss = [s for _, _, s in im.strs_in(f[0], 5)]
        src = [s.split(BS)[-1] for s in ss if 'mnt' + BS + 'gsg' in s]
        other = [s for s in ss if 'mnt' + BS + 'gsg' not in s]
        print(f"  {f[0]:08x} size {f[1]-f[0]:6d} src={sorted(set(src))[:3]} strs={other[:n]}")
