"""Resolve every mem_tuner anchor on victoria3.exe offline, mirroring plugins/mem_tuner/mt_core.h.
Usage: python tools/re_mem_check.py <victoria3.exe>      exit code 0 = all anchors OK"""
import sys, struct, bisect, pefile

NAMES = ["show_errors", "show_stats", "verbose", "eager_commit", "deprecated_eager_region_commit",
         "deprecated_reset_decommits", "large_os_pages", "reserve_huge_os_pages", "reserve_huge_os_pages_at",
         "reserve_os_memory", "deprecated_segment_cache", "page_reset", "abandoned_page_decommit",
         "deprecated_segment_reset", "eager_commit_delay", "decommit_delay", "use_numa_nodes", "limit_os_alloc",
         "os_tag", "max_errors", "max_warnings", "max_segment_reclaim", "allow_decommit",
         "segment_decommit_delay", "decommit_extend_delay", "destroy_on_exit"]
SIG_GNH = bytes.fromhex("4053 4883EC20 488D0D".replace(" ", ""))
SIG_COLLECT = bytes.fromhex("48895C2408 57 4883EC20 0FB6F9".replace(" ", ""))

pe = pefile.PE(sys.argv[1], fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
M = pe.get_memory_mapped_image()
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
ok = True

def rd32(r): return struct.unpack_from('<i', M, r)[0]
def cstr(r):
    e = M.find(b'\0', r, r + 128); return M[r:e].decode('latin1') if e > 0 else None
def find_str(s):
    t = sec['.rdata']; b = s.encode() + b'\0'; i = M.find(b, t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize)
    while i > 0 and M[i - 1] != 0: i = M.find(b, i + 1, t.VirtualAddress + t.Misc_VirtualSize)
    return i if i > 0 else None
def find_xref(target):
    t = sec['.text']; lo, hi = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
    for op in (b'\x8d', b'\x8b'):
        for pre in (b'\x48', b'\x4c'):
            for modrm in range(0x05, 0x40, 8):
                pat = pre + op + bytes([modrm]); i = M.find(pat, lo, hi)
                while i >= 0:
                    if i + 7 + rd32(i + 3) == target: return i
                    i = M.find(pat, i + 1, hi)
    return None
P = sec['.pdata']; PD = M[P.VirtualAddress:P.VirtualAddress + P.Misc_VirtualSize]
FN = sorted(struct.unpack_from('<III', PD, i) for i in range(0, len(PD) - 12, 12) if struct.unpack_from('<I', PD, i)[0])
FB = [f[0] for f in FN]
def fstart(r):
    i = bisect.bisect_right(FB, r) - 1
    if i < 0 or not FN[i][0] <= r < FN[i][1]: return None
    b, e, u = FN[i]
    for _ in range(16):
        if not (M[u] >> 3) & 4: break
        b, e, u = struct.unpack_from('<III', M, u + 4 + ((M[u + 2] + 1) & ~1) * 2)
    return b
def check(name, cond, val):
    global ok
    ok &= bool(cond); print(f"{'OK  ' if cond else 'FAIL'} {name:28} {val}")

# 1. option table
s0 = find_str("show_errors"); d = sec['.data']; table = []
for off in range(d.VirtualAddress + 0x10, d.VirtualAddress + d.Misc_VirtualSize - 8, 8):
    if struct.unpack_from('<Q', M, off)[0] == IB + s0:
        t = off - 0x10
        good = all(struct.unpack_from('<i', M, t + i * 0x20 + 8)[0] == i and
                   cstr(struct.unpack_from('<Q', M, t + i * 0x20 + 0x10)[0] - IB) == NAMES[i] for i in range(26))
        if good: table.append(t)
check("option table (unique)", len(table) == 1, ' '.join(hex(t) for t in table))
if table:
    vals = {NAMES[i]: struct.unpack_from('<ii', M, table[0] + i * 0x20) for i in range(26)}
    print("     defaults:", ', '.join(f"{k}={v[0]}" for k, v in vals.items() if not k.startswith('deprecated')))
# 2. mi_collect
ps = find_str("process done: 0x%zx\n"); px = find_xref(ps) if ps else None; pf = fstart(px) if px else None
col = None
if pf:
    for i in range(pf, px):
        if M[i] == 0xB1 and M[i + 1] == 1 and M[i + 2] == 0xE8: col = i + 7 + rd32(i + 3); break
check("mi_process_done", pf, pf and hex(pf))
check("mi_collect", col and M[col:col + len(SIG_COLLECT)] == SIG_COLLECT, col and hex(col))
# 3. get_new_handler / error handler
os_ = find_str("out of memory in 'new'"); ox = find_xref(os_) if os_ else None; of = fstart(ox) if ox else None
gnh = None
if of:
    for i in range(of, ox):
        if M[i] == 0xE8:
            t = i + 5 + rd32(i + 1)
            if 0 < t < len(M) and M[t:t + len(SIG_GNH)] == SIG_GNH: gnh = t; break
check("mi_heap_try_new", of, of and hex(of))
check("std::get_new_handler", gnh, gnh and hex(gnh))
em = None
if ox and M[ox:ox + 3] == b'\x48\x8d\x15' and M[ox + 7] == 0xB9 and rd32(ox + 8) == 12 and M[ox + 12] == 0xE8:
    em = ox + 17 + rd32(ox + 13)
check("_mi_error_message", em, em and hex(em))
eh = None
if em:
    for i in range(em, em + 0x200 - 18):
        c = M[i:i + 19]
        if c[:3] == b'\x48\x8b\x05' and c[7:12] == b'\x8b\xcf\x48\x8b\x15':
            h = i + 7 + rd32(i + 3); a = i + 16 + rd32(i + 12); j = 17 if c[16] == 0x90 else 16
            if c[j:j + 2] == b'\xff\xd0' and a == h + 8: eh = h; break
check("mi_error_handler global", eh, eh and hex(eh))
# transition
ts = find_str("Transition {} took: {} seconds"); tx = find_xref(ts) if ts else None; tf = fstart(tx) if tx else None
check("transition fn", tf, tf and hex(tf))
print("ALL OK" if ok else "SOME ANCHORS FAILED")
sys.exit(0 if ok else 1)
