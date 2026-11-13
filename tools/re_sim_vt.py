"""Resolve the MSVC RTTI class name of the vtable containing a slot RVA.
Usage: re_sim_vt.py <exe> SLOT_RVA_HEX [...]   (slot = address of a function pointer inside a vtable)
Walks back from the slot to the qword that points at a RTTI Complete Object Locator (signature 1),
then prints class name, vtable start and slot index."""
import struct, sys
import pefile

exe, *hs = sys.argv[1:]
pe = pefile.PE(exe, fast_load=True); ib = pe.OPTIONAL_HEADER.ImageBase
rd = next(s for s in pe.sections if s.Name.startswith(b'.rdata'))
lo, hi = rd.VirtualAddress, rd.VirtualAddress + rd.Misc_VirtualSize


def q(r): return struct.unpack('<Q', pe.get_data(r, 8))[0]
def d(r): return struct.unpack('<I', pe.get_data(r, 4))[0]


def col_name(p):
    r = p - ib
    if not (lo <= r < hi):
        return None
    try:
        if d(r) != 1:
            return None
        td = d(r + 12)
        name = pe.get_data(td + 16, 300).split(b'\0')[0].decode('ascii', 'replace')
        return name if name.startswith('.?A') else None
    except Exception:
        return None


for h in hs:
    slot = int(h, 16)
    for k in range(0, 400):
        r = slot - 8 * k
        n = col_name(q(r - 8))
        if n:
            print(f"slot {slot:08x}: class {n}  vtable {r:08x}  index {k}  fn {q(slot)-ib:08x}")
            break
    else:
        print(f"slot {slot:08x}: no COL found")
