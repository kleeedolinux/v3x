import re
"""Minimal minidump reader: exception code/address, module-relative fault, C++ exception type
name, and exe return-address candidates on the faulting thread's stack. Usage: dmpinfo.py <dmp>"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', d, 0)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', d, dir_rva + i * 12)
    streams.setdefault(t, (rva, size))
def u16str(rva):
    n = struct.unpack_from('<I', d, rva)[0]; return d[rva + 4:rva + 4 + n].decode('utf-16le')
mods = []
rva, _ = streams[4]
for i in range(struct.unpack_from('<I', d, rva)[0]):
    base, size, _, _, name_rva = struct.unpack_from('<QIIII', d, rva + 4 + i * 108)
    mods.append((base, size, u16str(name_rva).split(chr(92))[-1]))
def where(a):
    for b, s, n in mods:
        if b <= a < b + s: return f"{n}+0x{a - b:x}"
    return hex(a)
mem = []  # memory list (stream 5) and memory64 (stream 9)
if 5 in streams:
    rva, _ = streams[5]
    for i in range(struct.unpack_from('<I', d, rva)[0]):
        start, size, loc = struct.unpack_from('<QII', d, rva + 4 + i * 16)
        mem.append((start, size, loc))
if 9 in streams:
    rva, _ = streams[9]
    n, base_rva = struct.unpack_from('<QQ', d, rva)
    off = base_rva
    for i in range(n):
        start, size = struct.unpack_from('<QQ', d, rva + 16 + i * 16)
        mem.append((start, size, off)); off += size
def read(a, n):
    for s, sz, loc in mem:
        if s <= a and a + n <= s + sz: return d[loc + a - s: loc + a - s + n]
    return None

def type_name(ib, ti):
    try:
        cta_rva = struct.unpack('<i', read(ti + 12, 4))[0]
        n = struct.unpack('<i', read(ib + cta_rva, 4))[0]
        names = []
        for k in range(n):
            ct_rva = struct.unpack('<i', read(ib + cta_rva + 4 + 4 * k, 4))[0]
            td_rva = struct.unpack('<i', read(ib + ct_rva + 4, 4))[0]
            names.append((read(ib + td_rva + 16, 120) or b'').split(bytes(1))[0].decode(errors='replace'))
        return names
    except Exception as e:
        return ['?' + str(e)]
def scan_throws():
    """No exception stream: find in-memory EXCEPTION_RECORDs of MSVC C++ throws."""
    pat = struct.pack('<I', 0xE06D7363)
    seen = set()
    for start, size, loc in mem:
        blob = d[loc:loc + size]
        for m in re.finditer(re.escape(pat), blob):
            o = m.start()
            if o + 56 > len(blob) or o % 4: continue
            code, flags, rec, addr, npar = struct.unpack_from('<IIQQI', blob, o)
            if npar != 4: continue
            par = struct.unpack_from('<4Q', blob, o + 32)
            if par[0] != 0x19930520: continue
            key = (par[2], par[3])
            if key in seen: continue
            seen.add(key)
            print(f"C++ throw at {where(addr)} object={par[1]:#x} types={type_name(par[3], par[2])}")
            obj = read(par[1] + 8, 8)
            if obj:
                msgp = struct.unpack('<Q', obj)[0]
                msg = read(msgp, 96)
                if msg: print("   what():", msg.split(bytes(1))[0].decode(errors='replace'))
    print(f"{len(seen)} distinct throw records")

exe = next(m for m in mods if m[2].lower() == 'victoria3.exe')
if 6 not in streams:
    scan_throws(); sys.exit()
rva, _ = streams[6]
tid = struct.unpack_from('<I', d, rva)[0]
code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', d, rva + 8)
params = struct.unpack_from('<15Q', d, rva + 8 + 32)[:nparams]
print(f"thread {tid} exception 0x{code:08X} at {where(addr)}")
print("params", [hex(p) for p in params])
if code == 0xE06D7363 and nparams >= 4:  # MSVC C++ throw: params[2]=ThrowInfo RVA-based, params[3]=image base
    ib = params[3]; ti = params[2]
    cta = read(ti + 12, 4)
    if cta:
        cta_rva = struct.unpack('<i', cta)[0]
        first = read(ib + cta_rva + 4, 4)
        ct_rva = struct.unpack('<i', first)[0]
        td_rva = struct.unpack('<i', read(ib + ct_rva + 4, 4))[0]
        raw = read(ib + td_rva + 16, 120) or b''
        print("C++ exception type:", raw.split(b'\0')[0].decode(errors='replace'))
# stack scan of faulting thread
rva, _ = streams[3]
for i in range(struct.unpack_from('<I', d, rva)[0]):
    t = struct.unpack_from('<IIIIQQII', d, rva + 4 + i * 48)
    if t[0] == tid:
        start, size, loc = t[5], t[6], t[7]
        stack = d[loc:loc + size]
        hits = [struct.unpack_from('<Q', stack, j)[0] for j in range(0, len(stack) - 7, 8)]
        hits = [h - exe[0] for h in hits if exe[0] <= h < exe[0] + exe[1]]
        print("exe addresses on stack:", ' '.join(hex(h) for h in hits[:60]))
