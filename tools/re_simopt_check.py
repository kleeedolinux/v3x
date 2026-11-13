"""Offline anchor check for plugins/sim_opt against a victoria3.exe on disk (no game needed).

Mirrors the plugin's load-time checks:
  1. "Context %d" has exactly one xref; its .pdata function (RunTask) contains, in this order within
     0x200 bytes, the step zero-init, the Execute dispatch with Step at rsp+0x50, and the empty-step exit.
  2. MSVC RTTI: >= 50 CTickTask_* primary vtables; slots 1..5 match the common pattern and slot 6
     points into .text (= classes the plugin would patch).
  3. Step layout sanity: HandleAutonomousInvestment's Execute writes flags |= 3, Step+0x90/+0x94 and
     builds the range functor at Step+0x50 (the offsets diag reads).
  4. Each task in --throttle (Name:N list) is patchable and not on the plugin's deny list.
  5. diag_trades: of the functions referencing "UpdateStateTrades", exactly one contains the
     parallel-for call site (once, within 0x1200 bytes); its target has the expected prologue and
     reads begin/end at +0xc/+0x10, and has exactly one direct caller.
  6. diag_journal: "ExecuteOnPulse" has one xref; its function has JR_EOP_PROLOGUE; exactly one
     per-country Do_call (JR_DOCALL) targets a function with JR_S2_PROLOGUE; both take <= 4 args
     (no reads above the home slots); the JE type validation (string "Journal Entry Type {} need to
     belong to a group!") formats type+0x18 (the name std::string the report reads).
Exit code 0 = all OK.

Usage: re_simopt_check.py [victoria3.exe] [--exe victoria3.exe] [--throttle "HandleAutonomousInvestment:3,..."]
"""
import argparse, re, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_sim_lib import Img
import symbolize

SHAPE_ZERO = "4C 89 65 98 4C 89 65 D8"
SHAPE_EXEC = "49 8B 06 48 8D 54 24 50 49 8B CE FF 50 30"
SHAPE_EMPTY = "48 8B 4D D8 4C 8B 45 98 4D 85 C0 75 ?? 48 85 C9 0F 84"
JR_EOP_PROLOGUE = ("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 70 FF FF FF "
                   "48 81 EC 90 01 00 00 48 8B D9 80 B9 08 02 00 00 00 0F 84")
JR_DOCALL = ("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B 18 48 8B F1 41 8B 78 04 3B DF 74 ?? "
             "0F 1F 00 48 8B 46 08 44 8B 46 18 48 63 D3 48 8B 08 48 8B 14 D1 48 8B 4E 10 8B 52 18 E8")
JR_S2_PROLOGUE = "40 53 41 55 41 57 48 83 EC 50 4C 8B 99 88 00 00 00 4C 8B D1 48 63 99 94 00 00 00 45 8B F8"
TR_CALL_PAT = "48 8D 95 70 02 00 00 48 8D 4D 90 C5 F8 77 E8"
TR_PFOR_PROLOGUE = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC"
TR_PFOR_RANGE = "8B 59 0C 8B 71 10"
DENY = {"RecalculateModifierNodes", "RemoveDeadObjects", "RemoveNonexistantCountries", "PrepareNavyAIData",
        "FlushNavyAIData", "ClearLogisticsData", "ClearOutlinerData", "MoveArmies", "MoveFleets", "MoveFormations",
        "UpdateActiveBattles", "UpdateActiveNavalBattles", "ProcessAttritionCasualties", "UpdateRichPresence",
        "MonthlyPulseJournalEntries", "YearlyPulseJournalEntries", "UpdateJournalEntries"}
DENY_PREFIX = ("Remove", "Clear", "Flush", "Validate", "Prepare", "Recalculate", "Invalidate")
NAME_LEA = bytes([0x4C, 0x8D, 0x66, 0x18])   # lea r12, [rsi+0x18]


def pat(s):
    return re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in s.split()), re.S)


def rtti_tasks(im):
    rd = im.secs['.rdata']; d = rd.get_data(); va = rd.VirtualAddress
    cols = {}
    for o in range(0, len(d) - 24, 4):
        if d[o:o + 4] == b'\x01\0\0\0':
            sig, off, cd, td, cls, self_ = struct.unpack_from('<6I', d, o)
            if self_ == va + o and off == 0:
                cols[va + o] = td
    out = {}
    base = im.base
    for o in range(0, len(d) - 8, 8):
        p = struct.unpack_from('<Q', d, o)[0] - base
        if p in cols:
            td = cols[p]
            name = im.pe.get_data(td + 16, 96).split(b'\0')[0].decode('latin1')
            m = re.match(r'\.\?A[VU](?:\?\$)?(CTickTask_\w+?)@', name)
            if m:
                out.setdefault(m.group(1)[10:], va + o + 8)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe_pos', nargs='?', metavar='EXE', help='victoria3.exe (same as --exe)')
    ap.add_argument('--exe', default=None)
    ap.add_argument('--throttle', default='')
    a = ap.parse_args()
    a.exe = a.exe or a.exe_pos or symbolize.DEFAULT_EXE
    print(f"exe: {a.exe}")
    im = Img(a.exe)
    fails = 0

    def check(ok, msg):
        nonlocal fails
        print(('OK   ' if ok else 'FAIL ') + msg)
        fails += not ok

    # 1. RunTask
    s = im.find_string('Context %d')
    xs = [x for t in s for x in im.xref.get(t, [])]
    check(len(xs) == 1, f"'Context %d' xrefs: {len(xs)}")
    if xs:
        f = im.func(xs[0])
        body = im.tdata[f[0] - im.tva:f[0] - im.tva + 0x200]
        offs = []
        for nm, sh in (('zero', SHAPE_ZERO), ('exec', SHAPE_EXEC), ('empty', SHAPE_EMPTY)):
            m = pat(sh).search(body)
            offs.append(m.start() if m else -1)
            check(m is not None, f"RunTask {f[0]:08x}: shape {nm} at +{m.start():#x}" if m else f"RunTask shape {nm} missing")
        check(-1 not in offs and offs == sorted(offs), f"shapes in order {offs}")

    # 2. task vtables
    tasks = rtti_tasks(im)
    check(len(tasks) >= 50, f"CTickTask_ vtables: {len(tasks)}")
    tlo, thi = im.tva, im.tva + len(im.tdata)
    slots = {n: [struct.unpack('<Q', im.pe.get_data(vt + 8 * k, 8))[0] - im.base for k in range(7)] for n, vt in tasks.items()}
    common = [max(set(v[k] for v in slots.values()), key=lambda c: sum(v[k] == c for v in slots.values())) for k in range(7)]
    patchable = {n for n, v in slots.items() if tlo <= v[6] < thi and all(v[k] == common[k] for k in range(1, 6))}
    check(len(patchable) >= 50, f"patchable classes: {len(patchable)} of {len(tasks)}; not patchable: {sorted(set(tasks) - patchable)}")

    # 3. Step layout
    hai = slots.get('HandleAutonomousInvestment')
    if hai:
        f = im.func(hai[6]); code = im.tdata[f[0] - im.tva:f[1] - im.tva]
        ok = b'\x83\x0e\x03' in code and b'\xc7\x86\x90\x00\x00\x00' in code and b'\x89\xbe\x94\x00\x00\x00' in code \
            and b'\x48\x8d\x56\x50' in code
        check(ok, f"HAI Execute {f[0]:08x}: flags|=3, Step+0x90/+0x94, functor at Step+0x50")
    else:
        check(False, "HandleAutonomousInvestment vtable not found")

    # 4. throttle list
    for tok in filter(None, (t.strip() for t in a.throttle.split(','))):
        n, _, e = tok.partition(':')
        bad = n in DENY or n.startswith(DENY_PREFIX)
        check(n in patchable and not bad and e.isdigit() and 2 <= int(e) <= 64,
              f"throttle {tok}: {'patchable' if n in patchable else 'NOT patchable'}{', DENIED' if bad else ''}")

    # 5. diag_trades anchors
    fns = sorted({im.func(x)[0] for t in im.find_string('UpdateStateTrades') for x in im.xref.get(t, []) if im.func(x)})
    cands = []
    for f0 in fns:
        body = im.tdata[f0 - im.tva:f0 - im.tva + 0x1200]
        hits = [m.start() for m in pat(TR_CALL_PAT).finditer(body)]
        if hits:
            cands.append((f0, hits))
    check(len(cands) == 1 and len(cands[0][1]) == 1, f"UpdateStateTrades candidates {[(hex(c[0]), len(c[1])) for c in cands]} of {[hex(f) for f in fns]}")
    if cands:
        f0, h = cands[0][0], cands[0][1][0]
        a0 = f0 + h
        pfa = a0 + 19 + struct.unpack_from('<i', im.tdata, a0 - im.tva + 15)[0]
        pb = im.tdata[pfa - im.tva:pfa - im.tva + 0x40]
        ncall = len(im.callsto.get(pfa, []))
        check(pat(TR_PFOR_PROLOGUE).match(pb) is not None and pat(TR_PFOR_RANGE).search(pb) is not None and ncall == 1,
              f"UpdateStateTrades {f0:08x} -> parallel-for {pfa:08x}: prologue, begin/end reads, {ncall} direct caller(s)")

    # 6. diag_journal anchors
    import capstone
    import re_simopt_argcheck as ac
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    xs = [x for t in im.find_string('ExecuteOnPulse') for x in im.xref.get(t, [])]
    eop = im.func(xs[0])[0] if len(xs) == 1 and im.func(xs[0]) else None
    ok = eop is not None and pat(JR_EOP_PROLOGUE).match(im.tdata[eop - im.tva:eop - im.tva + 64]) is not None
    check(ok, f"ExecuteOnPulse xrefs {len(xs)}, function {eop and hex(eop)}, prologue")
    tg = set()
    n = len(JR_DOCALL.split())
    for m in pat(JR_DOCALL).finditer(im.tdata):
        h = im.tva + m.start()
        t = h + n + 4 + struct.unpack_from('<i', im.tdata, m.start() + n)[0]
        if tlo <= t < thi and pat(JR_S2_PROLOGUE).match(im.tdata[t - im.tva:t - im.tva + 32]):
            tg.add(t)
    check(len(tg) == 1, f"per-country stage candidates {[hex(t) for t in tg]}")
    for t, nm in ((eop, 'ExecuteOnPulse'), (next(iter(tg)) if tg else None, 'per-country stage')):
        if t:
            hits = ac.callee_side(im, md, t)
            check(hits == [], f"{nm} {t:08x}: no stack-argument reads (<= 4 args)")
    gx = [x for t in im.find_string('Journal Entry Type {} need to belong to a group!') for x in im.xref.get(t, [])]
    vf = im.func(gx[0])[0] if gx and im.func(gx[0]) else 0
    nb = im.tdata[vf - im.tva:vf - im.tva + 0x70] if vf else b''
    check(NAME_LEA in nb, f"JE type validation {vf:08x} formats [type+0x18] (name std::string)")

    print('ALL OK' if not fails else f'{fails} FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
