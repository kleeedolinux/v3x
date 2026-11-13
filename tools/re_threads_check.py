"""Resolve every cpu_sched anchor on victoria3.exe offline, mirroring plugins/cpu_sched/cpu_sched.c.
The byte signatures are read from plugins/cpu_sched/cs_core.h, so the two cannot drift apart.
Usage: python tools/re_threads_check.py <victoria3.exe>      exit code 0 = all anchors OK"""
import os, re, sys, struct, bisect, pefile, capstone

HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'plugins', 'cpu_sched', 'cs_core.h')
src = open(HDR, encoding='utf-8').read()
def sig(name):
    body = re.search(r'CS_SIG_%s\[\]\s*=\s*\{(.*?)\};' % name, src, re.S).group(1)
    b = bytes(int(t, 0) for t in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body))
    m = re.search(r'CS_MASK_%s\[\]\s*=\s*"([x?]+)"' % name, src).group(1)
    assert len(b) == len(m), f"{name}: {len(b)} bytes vs mask {len(m)}"
    return b, m
SIG = {n: sig(n) for n in ('INIT', 'ADDWORKER', 'WORKER')}

pe = pefile.PE(sys.argv[1], fast_load=True)
M = pe.get_memory_mapped_image()
sec = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
T = sec['.text']; TLO, THI = T.VirtualAddress, T.VirtualAddress + T.Misc_VirtualSize
ok = True

def rd32(r): return struct.unpack_from('<i', M, r)[0]
def match(r, name):
    b, m = SIG[name]
    return 0 <= r and r + len(b) <= len(M) and all(c == '?' or M[r + i] == b[i] for i, c in enumerate(m))
def find_str(s):
    t = sec['.rdata']; b = s.encode() + b'\0'; i = M.find(b, t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize)
    while i > 0 and M[i - 1] != 0: i = M.find(b, i + 1, t.VirtualAddress + t.Misc_VirtualSize)
    return i if i > 0 else None
def xrefs(target):
    out = []
    for op in (b'\x8d', b'\x8b'):
        for pre in (b'\x48', b'\x4c'):
            for modrm in range(0x05, 0x40, 8):
                pat = pre + op + bytes([modrm]); i = M.find(pat, TLO, THI)
                while i >= 0:
                    if i + 7 + rd32(i + 3) == target: out.append(i)
                    i = M.find(pat, i + 1, THI)
    return sorted(out)
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
def check(name, cond, val=''):
    global ok
    ok &= bool(cond); print(f"{'OK  ' if cond else 'FAIL'} {name:34} {val}")
def scan_all(name):
    b, m = SIG[name]; fixed = b[:m.index('?')] if '?' in m else b; hits = []
    i = M.find(fixed, TLO, THI)
    while i >= 0:
        if match(i, name): hits.append(i)
        i = M.find(fixed, i + 1, THI)
    return hits
cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); cs.detail = True
def relocatable(r, need=14):
    """True if the first instructions covering `need` bytes have no RIP-relative operand or relative branch
    (so MinHook's trampoline copies them verbatim)."""
    n = 0
    for ins in cs.disasm(bytes(M[r:r + 32]), r):
        if 'rip' in ins.op_str or ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_RET):
            return False, ins.mnemonic + ' ' + ins.op_str
        n += ins.size
        if n >= need: return True, f"{n} bytes"
    return False, "decode failed"

# 1. scheduler init
s = find_str("Spawning {} normal worker threads"); x = xrefs(s) if s else []
f = fstart(x[0]) if len(x) == 1 else None
check('"Spawning {} normal worker threads"', s and len(x) == 1, f"str {s and hex(s)} xrefs {[hex(i) for i in x]}")
check("scheduler init (CS_SIG_INIT)", f is not None and match(f, 'INIT'), f and hex(f))
if f: r, why = relocatable(f); check("  prologue relocatable", r, why)
check("  signature unique in .text", len(scan_all('INIT')) == 1, [hex(i) for i in scan_all('INIT')])

# 2. worker entry
s = find_str("{}_{} {}"); x = xrefs(s) if s else []
check('"{}_{} {}"', s and len(x) == 1, f"str {s and hex(s)} xrefs {[hex(i) for i in x]}")
add = w = None
if len(x) == 1:
    for i in range(x[0], x[0] + 0x100 - 4):
        if M[i] == 0xE8 and match(i + 5 + rd32(i + 1), 'ADDWORKER'): add = i + 5 + rd32(i + 1); break
check("AddWorker (call after xref)", add, add and hex(add))
if add:
    for i in range(add, add + 0x400 - 11):
        if M[i:i + 3] == b'\x48\x8d\x0d' and M[i + 7] == 0xE8 and match(i + 7 + rd32(i + 3), 'WORKER'): w = i + 7 + rd32(i + 3); break
check("worker entry (lea rcx; call)", w, w and hex(w))
if w:
    r, why = relocatable(w); check("  prologue relocatable", r, why)
    check("  function start == entry", fstart(w) == w, hex(fstart(w) or 0))
    tls = struct.unpack_from('<I', M, w + 0x1e)[0]
    print(f"     task-thread TLS offset 0x{tls:x}, kind @+0x3c, index @+0x38")
check("  signature unique in .text", len(scan_all('WORKER')) == 1, [hex(i) for i in scan_all('WORKER')])
print("ALL OK" if ok else "SOME ANCHORS FAILED")
sys.exit(0 if ok else 1)
