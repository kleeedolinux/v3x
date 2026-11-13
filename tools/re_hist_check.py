"""Offline check of the keep_history plugin's anchor logic against victoria3.exe on disk.
Applies exactly the same steps as plugins/keep_history/keep_history.c and prints the RVAs
it would patch / hook. Exit code 0 = all shape checks pass (the plugin would load), 1 = fail.
Usage: re_hist_check.py <victoria3.exe>"""
import re, struct, sys
import pefile, capstone

RESAMPLE_STR = b'Trying to fill samples in trend but no space was reserved!\0'
# Country "demote to AI" path: 6x `lea rcx,[r14+disp32]; call Trend::Clear`, then
# `lea rcx,[r14+disp32]; call X`, then `lea rcx,[r14+disp32]; mov edx,0x1c; mov r8d,...`.
CLEAR6 = (rb'(?:\x49\x8d\x8e..\x00\x00\xe8....){6}'
          rb'\x49\x8d\x8e..\x00\x00\xe8....'
          rb'\x49\x8d\x8e..\x00\x00\xba\x1c\x00\x00\x00\x41\xb8')
END_MARK = b'\xba\x78\x00\x00\x00\x49\x8b\xce\xe8'   # mov edx,120; mov rcx,r14; call SetCountryTrendRate
STATE_WINDOW = 0x200


def main():
    pe = pefile.PE(sys.argv[1], fast_load=True)
    secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
    text = secs['.text']; td = text.get_data(); tva = text.VirtualAddress
    pd = secs['.pdata'].get_data()
    funcs = sorted(struct.unpack_from('<II', pd, i) for i in range(0, len(pd) - 11, 12)
                   if struct.unpack_from('<I', pd, i)[0])

    def fstart(rva):  # .pdata lookup (no chain following; fine for these targets)
        lo, hi = 0, len(funcs)
        while lo < hi:
            m = (lo + hi) // 2
            if funcs[m][0] <= rva: lo = m + 1
            else: hi = m
        b, e = funcs[lo - 1]
        return b if b <= rva < e else None

    def rel_call(rva):  # target of E8 rel32 at rva
        o = rva - tva
        if td[o] != 0xE8: return None
        return rva + 5 + struct.unpack_from('<i', td, o + 1)[0]

    def find_str(raw):
        for name in ('.rdata', '.data'):
            d = secs[name].get_data(); i = d.find(raw)
            while i > 0 and d[i - 1] != 0:
                i = d.find(raw, i + 1)
            if i != -1: return secs[name].VirtualAddress + i
        return None

    def first_xref(t):  # lea/mov reg,[rip+disp32] with REX.W, like api->find_xref(t, 0)
        for m in re.finditer(rb'[\x48\x4c][\x8d\x8b]', td):
            i = m.start()
            if i + 7 <= len(td) and (td[i + 2] & 0xC7) == 0x05 and tva + i + 7 + struct.unpack_from('<i', td, i + 3)[0] == t:
                return tva + i
        return None

    ok = True
    def check(cond, msg):
        nonlocal ok
        print(('OK   ' if cond else 'FAIL ') + msg)
        ok = ok and bool(cond)
        return cond

    # 1. Trend::Resample (string anchor -> first lea/mov xref -> function start)
    s_rva = find_str(RESAMPLE_STR)
    if not check(s_rva, f"resample string at {s_rva or 0:08x}"): return 1
    x = first_xref(s_rva)
    if not check(x, f"resample string xref at {x or 0:08x}"): return 1
    resample = fstart(x)
    if not check(resample, f"Trend::Resample = {resample or 0:08x}"): return 1

    # 2. unique clear block
    hits = [tva + m.start() for m in re.finditer(CLEAR6, td, re.S)]
    if not check(len(hits) == 1, f"clear block matches: {[hex(h) for h in hits]}"): return 1
    blk = hits[0]
    sites = [blk + 12 * k + 7 for k in range(6)]
    clear = rel_call(sites[0])
    check(all(rel_call(s) == clear for s in sites), f"6 country clear calls -> {clear:08x}: {[hex(s) for s in sites]}")
    check(clear != resample and fstart(clear) == clear, f"Trend::Clear {clear:08x} is a function start, != resample")
    demote = fstart(blk)
    check(demote, f"demote function = {demote or 0:08x}")

    # 3. state trend clears between block and END_MARK
    o0 = blk - tva + 12 * 7
    e = td.find(END_MARK, o0, o0 + STATE_WINDOW)
    if not check(e != -1, f"end marker (mov edx,120; call) at {tva + e if e != -1 else 0:08x}"): return 1
    state_sites = [tva + i for i in range(o0, e) if td[i] == 0xE8 and rel_call(tva + i) == clear]
    check(len(state_sites) == 5, f"state clear calls: {[hex(s) for s in state_sites]}")
    setrate = rel_call(tva + e + len(END_MARK) - 1)
    so = setrate - tva
    calls_resample = [i for i in range(so, so + 0x40) if td[i] == 0xE8 and rel_call(tva + i) == resample]
    check(calls_resample, f"SetCountryTrendRate {setrate:08x} calls Trend::Resample early ({[hex(tva + i) for i in calls_resample]})")

    # 4. record_all: hook SetCountryTrendRate (rate = min(rate, rate_days)) and ShouldUpdateTrends
    print(f"OK   record_all hook 1: SetCountryTrendRate = {setrate:08x}")
    us = find_str(b'UpdateCountryTrends\0')
    ux = first_xref(us) if us else None
    if check(ux, f"UpdateCountryTrends string {us or 0:08x} xref {ux or 0:08x}"):
        o = ux - tva
        cs = [tva + i for i in range(o - 0x300, o) if td[i] == 0xE8 and td[i + 5:i + 9] == b'\x84\xc0\x0f\x84']
        if check(len(cs) == 1, f"call; test al,al; je sites before it: {[hex(c) for c in cs]}"):
            should = rel_call(cs[0]); so = should - tva
            shp = any(td[i:i + 4] == b'\x83\xfa\x01\x74' and td[i + 5:i + 8] == b'\x83\xfa\x02' for i in range(so, so + 0x80))
            check(shp, f"record_all hook 2: ShouldUpdateTrends = {should:08x} (forced true for return addr {cs[0] + 5:08x})")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    print("patch sites (call -> 5-byte nop):")
    for s in sites + state_sites:
        insn = list(md.disasm(td[s - tva - 7:s - tva + 5], s - 7))
        print("  " + " | ".join(f"{i.address:08x} {i.mnemonic} {i.op_str}" for i in insn))
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
