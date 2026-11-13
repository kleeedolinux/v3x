"""Find code xrefs (RIP-relative lea/mov) to an RVA or string; optionally disassemble around them.
Usage: pexref.py <exe> (--rva HEX | --str TEXT) [--dis BEFORE AFTER]"""
import argparse, re, struct, pefile, capstone

def load(exe):
    pe = pefile.PE(exe, fast_load=True)
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    return pe, text, text.get_data()

def find_string(pe, s):
    out = []
    needle = s.encode() + b'\0'
    for sec in pe.sections:
        d = sec.get_data()
        for m in re.finditer(re.escape(needle), d):
            if m.start() == 0 or d[m.start() - 1] == 0:
                out.append(sec.VirtualAddress + m.start())
    return out

PAT = re.compile(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', re.S)

def xrefs(text, data, target):
    base = text.VirtualAddress
    res = []
    for m in PAT.finditer(data):
        i = m.start()
        disp = struct.unpack_from('<i', data, i + 3)[0]
        if base + i + 7 + disp == target:
            res.append(base + i)
    return res

def dis(pe, text, data, rva, before, after):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    off = rva - text.VirtualAddress
    start = max(0, off - before)
    # resync: try starting points until one lands exactly on rva
    for s in range(start, off + 1):
        addrs = [ins.address for ins in md.disasm(data[s:off + after], text.VirtualAddress + s)]
        if rva in addrs:
            break
    for ins in md.disasm(data[s:off + after], text.VirtualAddress + s):
        mark = '>>' if ins.address == rva else '  '
        print(f"{mark} {ins.address:08x}: {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('exe'); ap.add_argument('--rva'); ap.add_argument('--str')
    ap.add_argument('--dis', nargs=2, type=int)
    a = ap.parse_args()
    pe, text, data = load(a.exe)
    targets = [int(a.rva, 16)] if a.rva else find_string(pe, a.str)
    for t in targets:
        xs = xrefs(text, data, t)
        print(f"target {t:08x}: {len(xs)} xrefs {[hex(x) for x in xs]}")
        if a.dis:
            for x in xs:
                print('-' * 60); dis(pe, text, data, x, *a.dis)
