"""Offline anchor check for plugins/asset_stream (mirrors as_resolve.c; keep in sync).
Usage: python re_stream_check.py <victoria3.exe>
Exit 0 only if every check passes (= the plugin would install its hook). Same scanners as
src/core.c: exact NUL-terminated string in .rdata/.data, RIP-relative lea/mov (REX 48/4C, 8D/8B,
modrm rm=101) xrefs, .pdata function start (following chained unwind info)."""
import re, struct, sys
import pefile

ANCHOR = b'gfx-check-resource-category'
XREF_OFF = 0x23
INIT_PROLOGUE = bytes.fromhex('48895C2410 4889742420 57 4154 4155 4156 4157 4881ECF0020000 4C8BEA 4C8BE1'.replace(' ', ''))
GATE = '41 80 7D 14 00 74 0E 49 8D 4C 24 10 49 8B 55 18 E8'
GATE_CALL = 16
START_HEAD = bytes.fromhex('48895C2408 4889742410 57 4883EC30 C681B101000001 488BF2'.replace(' ', ''))
START_BUDGET_OFF = 0xA9
START_BUDGET = bytes.fromhex('4889B7A0010000')
EXPECT_115 = {'master_init': 0x3c3fdc0, 'gate': 0x3c407af, 'start': 0x3c613e0}


def main(exe):
    pe = pefile.PE(exe, fast_load=True)
    img = pe.get_memory_mapped_image()
    secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
    tx = secs['.text']; tlo = tx.VirtualAddress; text = img[tlo:tlo + tx.Misc_VirtualSize]
    pd = secs['.pdata']; pdat = img[pd.VirtualAddress:pd.VirtualAddress + pd.Misc_VirtualSize]
    rf = sorted(struct.unpack_from('<III', pdat, i) for i in range(0, len(pdat) - 11, 12)
                if struct.unpack_from('<I', pdat, i)[0])
    starts = [r[0] for r in rf]
    import bisect

    def fstart(rva):
        i = bisect.bisect_right(starts, rva) - 1
        if i < 0 or not (rf[i][0] <= rva < rf[i][1]): return None
        b, e, u = rf[i]
        for _ in range(32):
            if not ((img[u] >> 3) & 4): break
            cnt = img[u + 2]
            b, e, u = struct.unpack_from('<III', img, u + 4 + ((cnt + 1) & ~1) * 2)
        return b

    ok = True

    def check(c, msg):
        nonlocal ok
        print(('OK   ' if c else 'FAIL ') + msg)
        ok &= bool(c)
        return c

    srv = []
    for n in ('.rdata', '.data'):
        s = secs[n]; d = img[s.VirtualAddress:s.VirtualAddress + s.Misc_VirtualSize]
        for m in re.finditer(re.escape(ANCHOR + b'\0'), d):
            if m.start() == 0 or d[m.start() - 1] == 0: srv.append(s.VirtualAddress + m.start())
    if not check(len(srv) >= 1, f'string {ANCHOR.decode()!r} at {[hex(x) for x in srv]}'): return 1
    s = srv[0]
    xr = []
    for m in re.finditer(rb'[\x48\x4c][\x8d\x8b]', text):
        i = m.start()
        if i + 7 > len(text) or (text[i + 2] & 0xC7) != 0x05: continue
        if tlo + i + 7 + struct.unpack_from('<i', text, i + 3)[0] == s: xr.append(tlo + i)
    if not check(len(xr) == 1, f'exactly one xref: {[hex(x) for x in xr]}'): return 1
    fn = fstart(xr[0])
    if not check(fn is not None and xr[0] - fn == XREF_OFF, f'xref at fn+{(xr[0] - fn) if fn else 0:#x} (want +{XREF_OFF:#x}), fn {fn or 0:#x}'): return 1
    check(img[fn:fn + len(INIT_PROLOGUE)] == INIT_PROLOGUE, 'master init prologue (mov r13,rdx; mov r12,rcx)')
    pat = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in GATE.split()), re.S)
    gs = [tlo + m.start() for m in pat.finditer(text)]
    if not check(len(gs) == 1, f'gate pattern unique: {[hex(x) for x in gs]}'): return 1
    g = gs[0]
    check(fstart(g) == fn, f'gate {g:#x} inside master init (fn+{g - fn:#x})')
    st = g + GATE_CALL + 5 + struct.unpack_from('<i', img, g + GATE_CALL + 1)[0]
    check(fstart(st) == st, f'Start {st:#x} is a function start')
    check(img[st:st + len(START_HEAD)] == START_HEAD, 'Start head: mov byte [rcx+0x1b1],1 (enabled)')
    check(img[st + START_BUDGET_OFF:st + START_BUDGET_OFF + 7] == START_BUDGET, 'Start+0xa9: mov [rdi+0x1a0],rsi (budget)')
    got = {'master_init': fn, 'gate': g, 'start': st}
    if got != EXPECT_115:
        print('note: RVAs differ from 1.15.1', {k: hex(v) for k, v in got.items()})
    print('ALL OK' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
