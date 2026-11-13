"""List direct call/jmp rel32 sites targeting the given RVAs. Usage: re_threads_callers.py <exe> RVA..."""
import sys, re, struct, bisect, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); pe.parse_data_directories(directories=[3])
t = next(s for s in pe.sections if s.Name.startswith(b'.text')); d = t.get_data(); b = t.VirtualAddress
funcs = sorted((r.struct.BeginAddress, r.struct.EndAddress) for r in pe.DIRECTORY_ENTRY_EXCEPTION); st = [f[0] for f in funcs]
def fs(x):
    k = bisect.bisect_right(st, x) - 1
    return funcs[k][0] if k >= 0 and funcs[k][0] <= x < funcs[k][1] else 0
tg = {int(x, 16) for x in sys.argv[2:]}
for m in re.finditer(rb'[\xe8\xe9]', d):
    i = m.start(); x = b + i + 5 + struct.unpack_from('<i', d, i + 1)[0]
    if x in tg: print(f"{x:08x} <- {'call' if d[i]==0xe8 else 'jmp '} {b+i:08x} in func {fs(b+i):08x}")
