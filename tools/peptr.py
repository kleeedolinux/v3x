"""Find absolute 64-bit pointers to an RVA (or to a string) in any section. Usage: peptr.py <exe> (--rva HEX|--str TEXT)"""
import argparse, struct, pefile
from pexref import find_string
ap = argparse.ArgumentParser(); ap.add_argument('exe'); ap.add_argument('--rva'); ap.add_argument('--str'); a = ap.parse_args()
pe = pefile.PE(a.exe, fast_load=True); ib = pe.OPTIONAL_HEADER.ImageBase
ts = [int(a.rva, 16)] if a.rva else find_string(pe, a.str)
for t in ts:
    needle = struct.pack('<Q', ib + t)
    for s in pe.sections:
        d = s.get_data(); i = d.find(needle)
        while i != -1:
            print(f"ptr to {t:08x} at {s.Name.rstrip(b'\0').decode()} rva {s.VirtualAddress+i:08x}")
            i = d.find(needle, i + 1)
