"""Memory-oriented minidump summary: stream list, thread count, module sizes, and (if present)
MemoryInfoList totals (committed/reserved by type), MiscInfo process memory counters.
Usage: re_mem_dmp.py <minidump.dmp>"""
import sys, struct, collections

d = open(sys.argv[1], 'rb').read()
sig, ver, n, dirr = struct.unpack_from('<IIII', d, 0)
S = {}
for i in range(n):
    t, sz, rva = struct.unpack_from('<III', d, dirr + i * 12)
    S.setdefault(t, (rva, sz))
NAMES = {3: 'ThreadList', 4: 'ModuleList', 5: 'MemoryList', 6: 'Exception', 7: 'SystemInfo', 9: 'Memory64List',
         12: 'HandleData', 15: 'MiscInfo', 16: 'MemoryInfoList', 17: 'ThreadInfoList', 21: 'SystemMemoryInfo',
         22: 'ProcessVmCounters', 24: 'ThreadNames', 0xcafe0000: '?'}
print('streams:', ', '.join(f"{NAMES.get(t, t)}({sz})" for t, (r, sz) in sorted(S.items())))
MB = 1 << 20
if 3 in S:
    print('threads:', struct.unpack_from('<I', d, S[3][0])[0])
def u16(r):
    k = struct.unpack_from('<I', d, r)[0]; return d[r + 4:r + 4 + k].decode('utf-16le')
if 4 in S:
    r = S[4][0]; k = struct.unpack_from('<I', d, r)[0]
    mods = []
    for i in range(k):
        base, size, _, _, nr = struct.unpack_from('<QIIII', d, r + 4 + i * 108)
        mods.append((size, u16(nr).split('\\')[-1], base))
    print(f'modules: {k}, image total {sum(m[0] for m in mods) / MB:.0f} MiB; largest:')
    for size, name, base in sorted(mods, reverse=True)[:15]:
        print(f'   {size / MB:7.1f} MiB  {name}')
if 15 in S:
    r, sz = S[15]
    flags = struct.unpack_from('<I', d, r + 4)[0]
    print(f'MiscInfo size {sz} flags {flags:#x}')
    if sz >= 0x2f0 + 0x5c and (flags & 0x100):  # MINIDUMP_MISC1_PROCESS_INFO... v4+ has process VM counters at +0x2f0? (best effort)
        pass
if 22 in S:
    r, sz = S[22]
    rev, fl, pf = struct.unpack_from('<HHI', d, r)
    nq = (sz - 8) // 8
    vals = struct.unpack_from('<%dQ' % nq, d, r + 8)
    names = ['PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage',
             'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage',
             'PeakVirtualSize', 'VirtualSize', 'PrivateUsage(commit)', 'PrivateWorkingSetSize', 'SharedCommitUsage', 'JobSharedCommitUsage',
             'JobPrivateCommitUsage', 'JobPeakPrivateCommitUsage', 'JobPrivateCommitLimit', 'JobTotalCommitLimit']
    print(f'ProcessVmCounters rev {rev} flags {fl:#x} PageFaultCount {pf}')
    for nm, v in zip(names, vals):
        print(f'   {nm}: {v / MB:.0f} MiB')
if 21 in S:
    r, sz = S[21]
    print(f'SystemMemoryInfo stream present ({sz} bytes)')
if 16 in S:
    r, sz = S[16]
    hs, es, cnt = struct.unpack_from('<IIQ', d, r)
    tot = collections.Counter(); big = []
    for i in range(cnt):
        base, abase, aprot, _, rsize, state, prot, typ = struct.unpack_from('<QQIIQIII', d, r + hs + i * es)
        tot[(state, typ)] += rsize
        if state == 0x1000 and typ == 0x20000: big.append((rsize, base))
    st = {0x1000: 'COMMIT', 0x2000: 'RESERVE', 0x10000: 'FREE'}; ty = {0x20000: 'PRIVATE', 0x40000: 'MAPPED', 0x1000000: 'IMAGE', 0: '-'}
    for (s, t), v in sorted(tot.items()):
        print(f'   {st.get(s, s):8} {ty.get(t, t):8} {v / MB:10.0f} MiB')
    byalloc = collections.Counter(); resv = collections.Counter()
    for i in range(cnt):
        base, abase, aprot, _, rsize, state, prot, typ = struct.unpack_from('<QQIIQIII', d, r + hs + i * es)
        if typ == 0x20000 and state in (0x1000, 0x2000):
            byalloc[(abase, state)] += rsize
    seg = collections.Counter()
    for (ab, stt), v in byalloc.items():
        cls = 'align32MiB(mimalloc?)' if ab % (32 << 20) == 0 else ('align4MiB' if ab % (4 << 20) == 0 else 'other')
        seg[(cls, st.get(stt))] += v
    prot = collections.Counter()
    for i in range(cnt):
        base, abase, aprot, _, rsize, state, pr, typ = struct.unpack_from('<QQIIQIII', d, r + hs + i * es)
        if typ == 0x20000 and state == 0x1000:
            cls = 'a32' if abase % (32 << 20) == 0 else 'other'
            prot[(cls, pr)] += rsize
    print('   committed private by (class, protect):')
    for k, v in sorted(prot.items(), key=lambda x: -x[1])[:10]: print(f'     {k[0]:6} prot={k[1]:#06x} {v / MB:9.0f} MiB')
    print('   private memory by allocation-base alignment:')
    for k, v in sorted(seg.items()): print(f'     {k[0]:22} {k[1]:8} {v / MB:9.0f} MiB')
    print('   largest committed private regions:')
    for size, base in sorted(big, reverse=True)[:15]:
        print(f'     {base:#016x} {size / MB:8.1f} MiB')
