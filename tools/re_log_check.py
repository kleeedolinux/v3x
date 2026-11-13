"""Offline check of the log_filter plugin anchors in victoria3.exe (mirrors plugins/log_filter/log_filter.c).
Chain:  RTTI name ".?AVlogger@spdlog@@" -> TypeDescriptor (name - 0x10) -> CompleteObjectLocator
        (.rdata, signature 1, pTypeDescriptor/pSelf image-relative) -> vtable (qword == COL VA, +8)
        vtable[2] = spdlog::logger::sink_it_(const details::log_msg &)
Layout checks (spdlog >= 1.12 log_msg): level +0x10, source.filename +0x38, source.line +0x40,
payload.data +0x50, payload.size +0x58 - verified through the %v / %# / %s flag formatters.
Usage: re_log_check.py <victoria3.exe> [--dis N]
Exit code 0 = all anchors resolved and every byte check passed (same checks the plugin does)."""
import argparse, bisect, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone
from pexref import load, find_string, xrefs

# (rtti name, vtable slot, [(hex bytes, window)], what)   -- keep in sync with log_filter.c CHECKS
CHECKS = [
    ('.?AVlogger@spdlog@@', 2, [('FF5008', 0xC0), ('FF5018', 0xC0), ('83F906', 0xC0)],
     'spdlog::logger::sink_it_  (sink->log [rax+8], flush_ [rax+0x18], level::off == 6)'),
    ('.?AV?$v_formatter@Unull_scoped_padder@details@spdlog@@@details@spdlog@@', 1,
     [('4C8B4258', 0x20), ('488B5250', 0x20)], 'v_formatter::format  (payload +0x50/+0x58)'),
    ('.?AV?$source_linenum_formatter@Unull_scoped_padder@details@spdlog@@@details@spdlog@@', 1,
     [('8B4A40', 0x10)], 'source_linenum_formatter::format  (source.line +0x40)'),
    ('.?AV?$short_filename_formatter@Unull_scoped_padder@details@spdlog@@@details@spdlog@@', 1,
     [('488B4A38', 0x20)], 'short_filename_formatter::format  (source.filename +0x38)'),
]
CATCH_STR = 'Rethrowing unknown exception in logger'


def sections(pe):
    return {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}


def pdata_starts(pe):
    d = sections(pe)['.pdata'].get_data()
    out = []
    for i in range(0, len(d) - 11, 12):
        b, e, _ = struct.unpack_from('<III', d, i)
        if b == 0:
            break
        out.append((b, e))
    out.sort()
    return out


def find_vtable(pe, base, name):
    hits = find_string(pe, name)
    if len(hits) != 1:
        return None, f'RTTI name found {len(hits)}x'
    td = hits[0] - 0x10
    rd = sections(pe)['.rdata']
    data, va = rd.get_data(), rd.VirtualAddress
    cols = []
    for off in range(0, len(data) - 24, 4):
        if data[off] != 1:
            continue
        sig, o, cd, ptd, pcd, pself = struct.unpack_from('<IIIIII', data, off)
        if sig == 1 and o == 0 and ptd == td and pself == va + off:
            cols.append(va + off)
    if len(cols) != 1:
        return None, f'COL found {len(cols)}x'
    needle = struct.pack('<Q', base + cols[0])
    vts, i = [], data.find(needle)
    while i != -1:
        if i % 8 == 0:
            vts.append(va + i + 8)
        i = data.find(needle, i + 1)
    if len(vts) != 1:
        return None, f'vtable found {len(vts)}x'
    return (td, cols[0], vts[0]), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe'); ap.add_argument('--dis', type=int, default=0)
    a = ap.parse_args()
    pe, text, tdata = load(a.exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    rd = sections(pe)['.rdata']; rdata = rd.get_data()
    funcs = pdata_starts(pe); fstarts = [f[0] for f in funcs]
    ok = True
    sink_it = None
    for name, slot, pats, what in CHECKS:
        res, err = find_vtable(pe, base, name)
        if not res:
            print(f'FAIL {what}: {err}'); ok = False; continue
        td, col, vt = res
        fn = struct.unpack_from('<Q', rdata, vt - rd.VirtualAddress + 8 * slot)[0] - base
        k = bisect.bisect_left(fstarts, fn)
        is_start = (k < len(fstarts) and fstarts[k] == fn) or slot != 2   # formatters are leaf fns (no .pdata)
        code = tdata[fn - text.VirtualAddress: fn - text.VirtualAddress + 0x100]
        miss = [p for p, w in pats if bytes.fromhex(p) not in code[:w]]
        good = is_start and not miss
        ok &= good
        print(f"{'OK  ' if good else 'FAIL'} {what}")
        print(f'     TD 0x{td:x}  COL 0x{col:x}  vtable 0x{vt:x}  [{slot}] = 0x{fn:x}'
              f"{'' if is_start else '  (not a .pdata function start)'}"
              f"{'  missing bytes ' + ','.join(miss) if miss else ''}")
        if slot == 2:
            sink_it = fn
    cs = find_string(pe, CATCH_STR)
    n = sum(len(xrefs(text, tdata, s)) for s in cs)
    print(f'info "{CATCH_STR}" (SPDLOG_LOGGER_CATCH, in catch funclets): {n} xrefs')
    if a.dis and sink_it is not None:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        off = sink_it - text.VirtualAddress
        for i, ins in enumerate(md.disasm(tdata[off:off + 0x200], sink_it)):
            if i >= a.dis:
                break
            print(f'    {ins.address:08x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}')
    print('ALL OK' if ok else 'CHECK FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
