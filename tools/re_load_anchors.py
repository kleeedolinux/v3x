"""Offline anchor resolution for plugins/fast_load (mirrors fast_load.c ANCHORS + checks).
Usage: python re_load_anchors.py <victoria3.exe> [--bytes]
For each anchor: string RVA -> first xref whose enclosing function passes the shape checks ->
function RVA. Prints OK/FAIL per check. Exit code 0 only if every anchor resolves (= the plugin
would install every hook). Same algorithm as src/core.c: exact NUL-terminated string in
.rdata/.data, RIP-relative lea/mov (REX 48/4C, 8D/8B, modrm rm=101), .pdata function start."""
import sys, struct, bisect, re
import pefile

# keep in sync with ANCHORS[] in plugins/fast_load/fast_load.c
# (name, anchor string, [(fn offset, hex pattern with ??)], extra)
ANCHORS = [
    ('transition', 'Transition {} took: {} seconds',
     [(0x00, '48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55'),
      (0x23, '48 8B 01 48 8B 90 50 01 00 00'),      # rax=[rcx]; rdx=[rax+0x150]
      (0xFB, '48 8D 87 68 01 00 00'),                # lea rax,[rdi+0x168] (transition name)
      (0xDD, '48 8B 87 50 01 00 00 C5 FA 11 90 28 01 00 00')]),  # [[task+0x150]+0x128] = seconds
    ('db_report', 'Database Node Init Time: {} - {} ms - {} ms including dependencies',
     [(0x29, '48 8B 41 08 48 8B 58 70 48 63 40 7C'),  # graph=[rcx+8]; nodes=[+0x70]; n=[+0x7c]
      (0x148, '4D 8B 8D A0 00 00 00'),               # r9=[node+0xa0] own ms
      (0x152, '49 8B 85 C0 00 00 00 49 8D 8D B0 00 00 00')]),  # dep list head/sentinel
    ('db_load', 'LoadNodesParallelFor',
     [(0x00, '48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57')]),
    ('mesh', 'Total time "{}" in {} ms',
     [(0x41, '48 8B 05 ?? ?? ?? ?? 44 8B A0 48 06 00 00')]),  # thread setting [g]+0x648
    ('cs_start', 'Starting checksum calculation',
     [(0x00, '48 89 5C 24 08 48 89 74 24 20 55 57 41 56')]),
    ('cs_feed', 'Feeding into checksum: {}',
     [(0x00, '48 89 5C 24 08 44 88 44 24 18 48 89 54 24 10')]),
]
THUNK_STR = 'IterateTypesThreaded'
THUNK_LEA_BACK = 0x24                     # lea rax,[rip+thunk] sits 0x24 bytes before the xref
THUNK_LEA = '48 8D 05 ?? ?? ?? ?? 48 89 45 F0'
THUNK_BODY = '48 8B 01 48 FF 60 30'       # mov rax,[rcx]; jmp [rax+0x30]


def pat(s):
    return [None if t == '??' else int(t, 16) for t in s.split()]


def match(buf, off, p):
    p = pat(p)
    return off >= 0 and off + len(p) <= len(buf) and all(b is None or buf[off + i] == b for i, b in enumerate(p))


class Img:
    def __init__(self, path):
        pe = pefile.PE(path, fast_load=True)
        self.secs = {}
        self.data = []
        for s in pe.sections:
            n = s.Name.rstrip(b'\0').decode()
            raw = s.get_data()[:s.Misc_VirtualSize].ljust(s.Misc_VirtualSize, b'\0')
            if n == '.text': self.text_rva, self.text = s.VirtualAddress, raw
            elif n in ('.rdata', '.data'): self.data.append((s.VirtualAddress, raw))
            elif n == '.pdata': self.pdata = (s.VirtualAddress, raw)
        pr = self.pdata[1]
        self.rf = sorted(struct.unpack_from('<III', pr, i) for i in range(0, len(pr) - 11, 12) if struct.unpack_from('<I', pr, i)[0])
        self.starts = [r[0] for r in self.rf]
        self.pe = pe

    def find_string(self, s):
        b = s.encode() + b'\0'
        for rva, raw in self.data:
            i = raw.find(b)
            while i >= 0:
                if i == 0 or raw[i - 1] == 0: return rva + i
                i = raw.find(b, i + 1)
        return None

    def xrefs(self, target):
        t = self.text; out = []
        for op in (b'\x48\x8d', b'\x48\x8b', b'\x4c\x8d', b'\x4c\x8b'):
            i = t.find(op)
            while i >= 0:
                if i + 7 <= len(t) and (t[i + 2] & 0xC7) == 5:
                    d = struct.unpack_from('<i', t, i + 3)[0]
                    if self.text_rva + i + 7 + d == target: out.append(self.text_rva + i)
                i = t.find(op, i + 1)
        return sorted(out)

    def unwind_primary(self, idx):
        b, e, u = self.rf[idx]
        for _ in range(32):
            ui = self.pe.get_data(u, 4)
            if not ((ui[0] >> 3) & 4): break
            off = 4 + ((ui[2] + 1) & ~1) * 2
            b, e, u = struct.unpack('<III', self.pe.get_data(u + off, 12))
        return b

    def function_start(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i < 0 or not (self.rf[i][0] <= rva < self.rf[i][1]): return None
        return self.unwind_primary(i)

    def code(self, rva, n):
        o = rva - self.text_rva
        return self.text[o:o + n] if 0 <= o < len(self.text) else b''


def resolve(img, verbose=False):
    ok_all = True; res = {}
    for name, s, checks in ANCHORS:
        srva = img.find_string(s)
        if srva is None:
            print(f'FAIL {name:10s} string not found: {s!r}'); ok_all = False; continue
        fn = None
        for x in img.xrefs(srva)[:16]:
            f = img.function_start(x)
            if f is None: continue
            buf = img.code(f, 0x400)
            if all(match(buf, o, p) for o, p in checks):
                fn = f; break
        if fn is None:
            print(f'FAIL {name:10s} no xref function passes {len(checks)} shape checks (string {srva:08x})'); ok_all = False; continue
        res[name] = fn
        print(f'OK   {name:10s} fn {fn:08x}  string {srva:08x}  checks {len(checks)}'
              + (f'  bytes {img.code(fn, 16).hex(" ")}' if verbose else ''))
    if 'mesh' in res:
        fn = res['mesh']; buf = img.code(fn, 0x1000)
        o = 0x41; disp = struct.unpack_from('<i', buf, o + 3)[0]
        print(f'     mesh thread-setting global ptr at {fn + o + 7 + disp:08x} (+0x648, clamped 2..15)')
        srva = img.find_string(THUNK_STR); thunk = None
        for x in (img.xrefs(srva) if srva else []):
            if img.function_start(x) != fn: continue
            lo = x - THUNK_LEA_BACK - fn
            if match(buf, lo, THUNK_LEA):
                d = struct.unpack_from('<i', buf, lo + 3)[0]; t = fn + lo + 7 + d
                if match(img.code(t, 8), 0, THUNK_BODY): thunk = t
        if thunk: print(f'OK   mesh_item  fn {thunk:08x}  (per-item vcall thunk, entry-only hook)'); res['mesh_item'] = thunk
        else: print('FAIL mesh_item  worker thunk shape mismatch (thread tracking off)'); ok_all = False
    # loading-screen pump step (fast_load.c install_pump): unique call site in 03775600 -> E8 target
    rx = re.compile(rb'\x48\x8b\x0b\xe8....\x48\x83\xc3\x08\x48\x3b\xdf\x75\xef\x48\x8b\x0d....\xe8', re.S)
    sites = [m.start() for m in rx.finditer(img.text)]
    if len(sites) == 1:
        o = sites[0]; step = img.text_rva + o + 8 + struct.unpack_from('<i', img.text, o + 4)[0]
        if match(img.code(step, 32), 0, '48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 30 48 8B F9'):
            print(f'OK   pump_step  fn {step:08x}  (call site {img.text_rva + o:08x}, unique)'); res['pump_step'] = step
        else: print('FAIL pump_step  prologue mismatch'); ok_all = False
    else: print(f'FAIL pump_step  call-site pattern matches {len(sites)} times'); ok_all = False
    # idle-worker try-get-task (fast_load.c install_worker_spin): unique 44-byte prologue, must be a function start
    wp = bytes.fromhex('48895C2410488974241848897C24204156 4883EC40488BDA4C8BF1488B118B493C83F9027507B800010000EB16'.replace(' ', ''))
    hits = []; i = img.text.find(wp)
    while i >= 0: hits.append(img.text_rva + i); i = img.text.find(wp, i + 1)
    if len(hits) == 1 and img.function_start(hits[0]) == hits[0]:
        print(f'OK   worker_get fn {hits[0]:08x}  (unique prologue)'); res['worker_get'] = hits[0]
    else: print(f'FAIL worker_get prologue matches {len(hits)} times'); ok_all = False
    return ok_all, res


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__)
    ok, _ = resolve(Img(sys.argv[1]), '--bytes' in sys.argv)
    print('ALL ANCHORS OK' if ok else 'SOME ANCHORS FAILED')
    sys.exit(0 if ok else 1)
