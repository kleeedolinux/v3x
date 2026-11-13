"""Dump every MSVC RTTI vtable of victoria3.exe as 'vtable_rva offset class' lines (worker script).

Usage: re_script_rtti.py [exe] [out.txt]   (default out: tools/re_script_tmp/rtti.txt)
Same COL algorithm as tools/rtti_dump.py, but all classes and all offsets, so a vtable RVA seen
in disassembly can be named with grep. Read-only."""
import os, struct, sys
import pefile

here = os.path.dirname(os.path.abspath(__file__))
exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', '..', 'binaries', 'victoria3.exe')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 're_script_tmp', 'rtti.txt')
pe = pefile.PE(exe, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
rd = [s for s in pe.sections if s.Name.startswith(b'.rdata')][0]
lo, hi = rd.VirtualAddress, rd.VirtualAddress + rd.Misc_VirtualSize
cols = {}
for o in range(lo, hi - 24, 4):
    sig, off, cd, td, chd, me = struct.unpack_from('<IIIIII', img, o)
    if sig == 1 and me == o and lo <= td < len(img):
        name = img[td + 16:td + 16 + 200].split(b'\0')[0].decode('latin1')
        cols[o] = (off, name)
lines = []
for o in range(lo, hi - 8, 8):
    q = struct.unpack_from('<Q', img, o)[0]
    c = q - ib
    if c in cols:
        off, name = cols[c]
        lines.append(f'{o + 8:08x} {off:x} {name}')
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w') as f:
    f.write('\n'.join(lines) + '\n')
print(len(cols), 'COLs,', len(lines), 'vtables ->', out)
