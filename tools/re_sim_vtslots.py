"""Dump vtable slots for tick-task vtables until the next RTTI COL / non-code pointer.
Usage: re_sim_vtslots.py <exe> <tt.txt>"""
import sys, struct, pefile
exe, tt = sys.argv[1:]
p = pefile.PE(exe, fast_load=True); IB = p.OPTIONAL_HEADER.ImageBase
t = p.sections[0]; lo, hi = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
for line in open(tt):
    if line.startswith('#'): continue
    f = line.split(); name = f[0]
    vt = int([x for x in f if x.startswith('vt=')][0][3:], 16)
    slots = []
    for i in range(16):
        v = struct.unpack('<Q', p.get_data(vt + 8 * i, 8))[0] - IB
        if not (lo <= v < hi): break
        slots.append(v)
    print(f"{name:48s} {f[1]:7s} n={len(slots)} " + ' '.join(f"{s:08x}" for s in slots))
