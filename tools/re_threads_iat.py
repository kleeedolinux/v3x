"""Find call sites of imported functions (call [rip+X], jmp [rip+X], mov reg,[rip+X]) and their
enclosing function (from .pdata). Usage: re_threads_iat.py <exe> NAME [NAME...]"""
import sys, struct, bisect, re, pefile

def main():
    exe, names = sys.argv[1], sys.argv[2:]
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[1, 3])
    iat = {}
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        for i in e.imports:
            if i.name and i.name.decode() in names:
                iat[i.address - pe.OPTIONAL_HEADER.ImageBase] = i.name.decode()
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    data, base = text.get_data(), text.VirtualAddress
    funcs = sorted((r.struct.BeginAddress, r.struct.EndAddress) for r in pe.DIRECTORY_ENTRY_EXCEPTION)
    starts = [f[0] for f in funcs]
    def fstart(rva):
        k = bisect.bisect_right(starts, rva) - 1
        return funcs[k][0] if k >= 0 and funcs[k][0] <= rva < funcs[k][1] else None
    hits = []
    for m in re.finditer(rb'\xff[\x15\x25]', data):
        i = m.start(); t = base + i + 6 + struct.unpack_from('<i', data, i + 2)[0]
        if t in iat: hits.append((iat[t], base + i, 'call' if data[i+1] == 0x15 else 'jmp'))
    for m in re.finditer(rb'[\x48\x4c]\x8b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', data):
        i = m.start(); t = base + i + 7 + struct.unpack_from('<i', data, i + 3)[0]
        if t in iat: hits.append((iat[t], base + i, 'mov'))
    for n, rva, k in sorted(hits):
        fs = fstart(rva)
        print(f"{n:32s} {k:4s} site={rva:08x} func={fs:08x}" if fs else f"{n:32s} {k:4s} site={rva:08x} func=?")

main()
