"""Enumerate C++ catch handlers (MSVC FH4 / __CxxFrameHandler4 metadata) and report which functions
catch a given exception type (by RTTI name) or catch(...).
Usage: re_mem_catch.py <exe> [".?AVbad_alloc@std@@" ...]"""
import sys, struct, collections
import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
IMG = pe.get_memory_mapped_image()
want = sys.argv[2:] or ['.?AVbad_alloc@std@@', '.?AVexception@std@@']

def u8(r): return IMG[r]
def i32(r): return struct.unpack_from('<i', IMG, r)[0]
def u32(r): return struct.unpack_from('<I', IMG, r)[0]
NEG = [1, 2, 1, 3, 1, 2, 1, 4, 1, 2, 1, 3, 1, 2, 1, 5]
SH = [25, 18, 25, 11, 25, 18, 25, 4, 25, 18, 25, 11, 25, 18, 25, 0]

def rdu(p):
    lb = IMG[p] & 0xF
    n = NEG[lb]
    v = struct.unpack_from('<I', IMG, p + n - 4)[0] >> SH[lb]
    return v, p + n

def tdname(td):
    s = IMG[td + 0x10: td + 0x110].split(b'\0')[0]
    return s.decode('latin1')

pd = next(s for s in pe.sections if s.Name.startswith(b'.pdata'))
P = pd.get_data()
handlers = collections.Counter()
ents = []
for i in range(0, len(P) - 12, 12):
    b, e, u = struct.unpack_from('<III', P, i)
    if not b: break
    flags = IMG[u] >> 3
    if not (flags & 3) or (flags & 4): continue
    cnt = IMG[u + 2]
    h = u + 4 + ((cnt + 1) & ~1) * 2
    hr = u32(h)
    handlers[hr] += 1
    ents.append((b, e, hr, h + 4))
print("top EH handlers:", [(f"{h:08x}", c) for h, c in handlers.most_common(6)])
EHS = {int(x, 16) for x in __import__('os').environ.get('EH_HANDLERS', '').split(',') if x} or {handlers.most_common(1)[0][0]}
res = collections.defaultdict(list)
nerr = 0
for b, e, hr, hd in ents:
    if hr not in EHS: continue
    try:
        fi = u32(hd)
        p = fi
        hdr = IMG[p]; p += 1
        if hdr & 4: _, p = rdu(p)
        if hdr & 8: p += 4
        if not (hdr & 0x10): continue
        tbm = i32(p)
        n, q = rdu(tbm)
        for _ in range(n):
            _, q = rdu(q); _, q = rdu(q); _, q = rdu(q)
            ha = i32(q); q += 4
            m, r = rdu(ha)
            for _ in range(m):
                hh = IMG[r]; r += 1
                if hh & 1: _, r = rdu(r)
                td = 0
                if hh & 2: td = i32(r); r += 4
                if hh & 4: _, r = rdu(r)
                r += 4  # dispOfHandler
                cc = (hh >> 4) & 3
                for _ in range(cc):
                    if hh & 8: r += 4
                    else: _, r = rdu(r)
                res[tdname(td) if td else '...'].append(b)
    except Exception:
        nerr += 1
print(f"parse errors: {nerr}")
for k, v in sorted(res.items(), key=lambda x: -len(x[1]))[:25]:
    print(f"{len(v):6d} catch {k}")
for w in want:
    fs = sorted(set(res.get(w, [])))
    print(f"== functions catching {w}: {len(fs)}")
    for f in fs[:60]: print(f"   {f:08x}")
