"""Brute-force: find any rip-relative disp32 in .text that resolves to the target RVA, assuming the
displacement is the last 4 bytes of the instruction (disp at i, next-ip = i+4) or followed by an
imm8/imm32 (next-ip = i+5 / i+8). Catches lea/mov/cmp/vmov* forms pexref misses.
Usage: re_sim_anyref.py <exe> HEX [HEX...]"""
import sys
import numpy as np
import pefile
from re_sim_lib import Img

exe, *hs = sys.argv[1:]
im = Img(exe)
d = np.frombuffer(im.tdata, dtype=np.uint8)
n = len(d) - 8
disp = (d[0:n].astype(np.int64) | (d[1:n + 1].astype(np.int64) << 8) |
        (d[2:n + 2].astype(np.int64) << 16) | (d[3:n + 3].astype(np.int64) << 24))
disp = np.where(disp >= 2 ** 31, disp - 2 ** 32, disp)
pos = np.arange(n, dtype=np.int64) + im.tva
for h in hs:
    t = int(h, 16)
    for extra in (4, 5, 8):
        hits = np.nonzero(pos + extra + disp == t)[0]
        for k in hits[:20]:
            a = int(pos[k])
            f = im.func(a)
            print(f"{t:08x} <- disp@{a:08x} (+{extra}) func {f[0]:08x}" if f else f"{t:08x} <- disp@{a:08x} (+{extra})")
