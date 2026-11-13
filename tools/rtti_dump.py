"""Static MSVC x64 RTTI dumper (same algorithm as include/v3x_rtti.h).

Usage: rtti_dump.py [exe] [--prefix CTickTask_] [--slots 8] [--slot 6] [--quiet]

1. One pass over .rdata: every 4-aligned CompleteObjectLocator with signature==1 whose
   pSelf RVA (+20) equals its own RVA; pTypeDescriptor RVA at +12, vtable offset at +4.
2. One pass over .rdata: every 8-aligned qword that is ImageBase+COL -> vtable = that qword + 8.
3. TypeDescriptor name = TD + 16 ('.?AVName@@'). Keep primary vtables (offset 0) whose
   class name starts with the prefix; print vtable RVA and slot RVAs, then an analysis of
   the chosen slot (default 6): target size from .pdata, sharing, first instructions,
   and ABI hints (xmm arg reads, stack-arg reads, float returns)."""
import argparse, bisect, os, struct, sys
from collections import Counter
import pefile

ap = argparse.ArgumentParser()
ap.add_argument('exe', nargs='?', default=os.path.join(os.path.dirname(__file__), '..', '..', 'binaries', 'victoria3.exe'))
ap.add_argument('--prefix', default='CTickTask_')
ap.add_argument('--slots', type=int, default=8)
ap.add_argument('--slot', type=int, default=6)
ap.add_argument('--quiet', action='store_true', help='only the summary')
a = ap.parse_args()

pe = pefile.PE(a.exe, fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
rd = sec['.rdata']; rlo, rhi = rd.VirtualAddress, rd.VirtualAddress + rd.Misc_VirtualSize
tx = sec['.text']; tlo, thi = tx.VirtualAddress, tx.VirtualAddress + tx.Misc_VirtualSize
rdata = img[rlo:rhi]

def d(r): return struct.unpack_from('<I', img, r)[0]
def q(r): return struct.unpack_from('<Q', img, r)[0]

# 1. COLs
cols = {}  # col_rva -> (td_rva, offset)
dw = memoryview(rdata)[: (len(rdata) // 4) * 4].cast('I')
for i in range(0, len(dw) - 6):
    if dw[i] == 1 and dw[i + 5] == rlo + 4 * i:
        cols[rlo + 4 * i] = (dw[i + 3], dw[i + 1])
# 2. vtables
vts = {}  # col_rva -> vtable rva (first one)
start = (8 - rlo % 8) % 8
qw = memoryview(rdata)[start: start + ((len(rdata) - start) // 8) * 8].cast('Q')
lo_p, hi_p = ib + rlo, ib + rhi
for i, v in enumerate(qw):
    if lo_p <= v < hi_p and (v - ib) in cols and (v - ib) not in vts:
        vts[v - ib] = rlo + start + 8 * i + 8

def tdname(td):
    end = img.find(b'\0', td + 16, td + 16 + 4096)
    return img[td + 16:end].decode('ascii', 'replace')

# pdata
pd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
rf = [struct.unpack_from('<III', img, pd.VirtualAddress + 12 * k) for k in range(pd.Size // 12)]
rf.sort(); rstarts = [x[0] for x in rf]
def fsize(rva):
    k = bisect.bisect_right(rstarts, rva) - 1
    if k >= 0 and rf[k][0] == rva: return rf[k][1] - rf[k][0]
    return None

want = '.?AV' + a.prefix
rows = []
for col, vt in vts.items():
    td, off = cols[col]
    if off: continue
    n = tdname(td)
    if n.startswith(want) and n.endswith('@@'):  # '.?AVName@@' or '.?AVName@?A0x1234abcd@@' (anon ns)
        slots = []
        for s in range(a.slots):
            p = q(vt + 8 * s)
            slots.append(p - ib if ib + tlo <= p < ib + thi else None)
        rows.append((n[4:n.index('@')], vt, slots))
rows.sort()
total_td = sum(1 for _ in range(0))
print(f"COLs: {len(cols)}  vtables: {len(vts)}  classes '{a.prefix}*' (primary vtables): {len(rows)}")

if not a.quiet:
    for n, vt, slots in rows:
        print(f"{n:60s} vt {vt:08x}  " + ' '.join(f"{s:08x}" if s is not None else '--------' for s in slots))

# slot analysis
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
cs = Cs(CS_ARCH_X86, CS_MODE_64)
S = a.slot
tg = Counter(r[2][S] for r in rows if len(r[2]) > S and r[2][S] is not None)
print(f"\nslot {S}: {sum(tg.values())} vtables, {len(tg)} distinct targets")
for s in range(a.slots):
    c = Counter(r[2][s] for r in rows if r[2][s] is not None)
    top = c.most_common(1)
    print(f"  slot {s}: {len(c):4d} distinct; most shared target {top[0][0]:08x} x{top[0][1]}" if top else f"  slot {s}: none")

hookable = 0; tiny = 0; shared = 0; nopdata = 0; xmmarg = 0; stackarg = 0; fret = 0
samples = []
for t, cnt in sorted(tg.items()):
    sz = fsize(t)
    code = img[t:t + 96]
    ins = list(cs.disasm(code, ib + t))[:12]
    txt = '; '.join(f"{i.mnemonic} {i.op_str}".strip() for i in ins[:4])
    # ABI hints within the first instructions
    body = ' | '.join(f"{i.mnemonic} {i.op_str}" for i in ins)
    xr = any(('xmm1' in i.op_str.split(',', 1)[-1] or 'xmm2' in i.op_str.split(',', 1)[-1] or 'xmm3' in i.op_str.split(',', 1)[-1]) and not i.mnemonic.startswith('movaps xmmword ptr [rsp')
             for i in ins if i.mnemonic.startswith(('movss', 'movsd', 'cvt', 'comis', 'ucomis', 'addss', 'mulss')))
    sa = any('[rsp + 0x28]' in i.op_str.split(',', 1)[-1] or '[rsp + 0x30]' in i.op_str.split(',', 1)[-1] for i in ins[:2])
    flags = []
    if sz is None: nopdata += 1; flags.append('no-pdata')
    # a .pdata entry <16 bytes is usually only the first chunk of a shrink-wrapped function;
    # call it tiny only if the code really ends (ret / jmp) inside those bytes
    if sz is not None and sz < 16 and any(i.mnemonic in ('ret', 'jmp') and i.address - ib + i.size <= t + sz for i in ins):
        tiny += 1; flags.append('tiny')
    elif sz is None and ins and ins[0].mnemonic in ('ret', 'jmp'): tiny += 1; flags.append('stub')
    if cnt > 8: shared += 1; flags.append(f'shared x{cnt}')
    if xr: xmmarg += 1; flags.append('xmm-arg?')
    if sa: stackarg += 1; flags.append('stack-arg?')
    ok = 'tiny' not in flags and 'stub' not in flags and cnt <= 8 and not sa
    hookable += ok
    if len(samples) < 12 or not ok:
        samples.append(f"  {t:08x} size {sz if sz is not None else '?':>6} x{cnt} {','.join(flags):18s} {txt}")
print(f"targets: hookable {hookable}, tiny {tiny}, shared>8 {shared}, no-pdata {nopdata}, xmm-arg? {xmmarg}, stack-arg? {stackarg}")
n8 = sum(1 for r in rows if len(r[2]) > S + 1 and r[2][S + 1] is not None and r[2][S + 1] not in (r[2][1], r[2][2]))
print(f"vtables with a code slot {S + 1} too: {n8} (for these slot {S} is usually the shared wrapper above; the body is slot {S + 1})")
print("note: in 1.15.1 slot 6 is called by RunTask (exe+0x146d010) as Prepare; hook RunTask to time whole tasks")
print("sample / flagged slot targets:")
print('\n'.join(samples))
