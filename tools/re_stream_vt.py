"""Which RTTI class owns the vtable slot at an .rdata RVA? Walks back to vtable[-1] (COL pointer).
Usage: re_stream_vt.py <exe> RVA [RVA...]   -> class name, vtable RVA, slot index (byte offset)"""
import sys, struct, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); ib = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
def q(r): return struct.unpack_from('<Q', img, r)[0]
def d(r): return struct.unpack_from('<I', img, r)[0]
for a in sys.argv[2:]:
    r = int(a, 16)
    for k in range(0, 400):
        p = r - 8 * k - 8
        v = q(p) - ib
        if 0 < v < len(img) - 24 and d(v) == 1 and d(v + 20) == v:
            td = d(v + 12); nm = img[td + 16: td + 200].split(b'\0')[0].decode(errors='replace')
            print(f'{r:08x}: class {nm} vtable {p + 8:08x} slot {k} (+0x{8 * k:x}) col_off {d(v + 4)}')
            break
    else:
        print(f'{r:08x}: no COL within 400 slots')
