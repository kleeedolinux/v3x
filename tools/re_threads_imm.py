"""Find functions containing an imm32/disp32 byte pattern (e.g. TLS offset). Usage: re_threads_imm.py <exe> HEX"""
import sys, re, struct, bisect, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); pe.parse_data_directories(directories=[3])
t = next(s for s in pe.sections if s.Name.startswith(b'.text')); d = t.get_data(); b = t.VirtualAddress
funcs = sorted((r.struct.BeginAddress, r.struct.EndAddress) for r in pe.DIRECTORY_ENTRY_EXCEPTION); st = [f[0] for f in funcs]
def fs(x):
    k = bisect.bisect_right(st, x) - 1
    return funcs[k][0] if k >= 0 and funcs[k][0] <= x < funcs[k][1] else 0
pat = struct.pack('<I', int(sys.argv[2], 16)); c = {}
for m in re.finditer(re.escape(pat), d):
    f = fs(b + m.start()); c[f] = c.get(f, 0) + 1
for f, n in sorted(c.items()): print(f"{f:08x} {n}")
