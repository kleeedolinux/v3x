"""Dump ASCII strings from a PE with their RVA. Usage: pestrings.py <exe> [minlen] > out.txt"""
import re, sys, pefile
exe = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
pe = pefile.PE(exe, fast_load=True)
for s in pe.sections:
    name = s.Name.rstrip(b'\0').decode()
    if name not in ('.rdata', '.data'):
        continue
    data = s.get_data()
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % n, data):
        print(f"{s.VirtualAddress + m.start():08x} {name} {m.group().decode()}")
