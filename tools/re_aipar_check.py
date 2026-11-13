"""Offline anchor + static-fact check for plugins/ai_par against victoria3.exe 1.15.1.

Runs the same lookups as ai_par.c resolve() (string xrefs, unique byte shapes, call/jmp targets, the
cross-checks between anchors) and verifies every static fact docs/re/ai_par.md relies on (parallel-for
chunking formula, dynamic atomic chunk loop, mode 2 = grain 1, RNG-owner swap, Step offsets read by the
range runner and the heuristic). Exit code 0 = ALL OK.
Usage: re_aipar_check.py [exe]"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_sim_lib import Img

EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'binaries', 'victoria3.exe')

# --- must stay identical to plugins/ai_par/ai_par.c ---------------------------------------------
S_RRP = "RunRangeExecutionParallel"
S_HEUR = "MaybeRunRangeExecutionParallelWithHeuristic"
S_PGPAR = "UpdatePopGrowthParallel"
P = dict(
    RRP_PRO="4C 89 44 24 18 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 08 FE FF FF 48 81 EC F8 02 00 00",
    RRP_SCHED="48 8B 05 ?? ?? ?? ?? 45 8B B5 94 00 00 00 45 8B A5 90 00 00 00",
    RRP_MODE="41 80 BD 9C 00 00 00 00 74 09 45 8B BD 98 00 00 00",
    RRP_WORKERS="8B 88 48 06 00 00",
    HEUR_PRO="48 8B C4 48 89 58 10 4C 89 48 20 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57",
    HEUR_FLAG="40 38 BB B0 00 00 00",
    GUARD_DTOR="48 83 EC 28 80 39 00 48 89 5C 24 38 48 8B D9 0F 84 ?? ?? ?? ?? 48 8B 41 08",
    GUARD_10MS="48 83 F8 0A",
    HAI_PSITE="4A 8B 0C 36 E8 ?? ?? ?? ?? 48 8B 4D 10 FF C3 48 89 04 0E",
    HAI_SSITE="FF C6 FF CB 85 DB 7E ?? 48 8B CD E8 ?? ?? ?? ?? 48 8B F8 48 8B 08 48 8B 51 30",
    HAI_EVAL_PRO="48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC 70 01 00 00",
    HAI_S2SITE="48 83 EC 38 C5 F8 10 41 10 48 8B 49 08 48 8D 54 24 20 C5 F8 11 44 24 20 E8",
    HAI_S2_PRO="4C 8B DC 4D 89 43 18 49 89 4B 08 56 41 54 48 81 EC E8 00 00 00",
    HAI_BSITE="4C 8B C7 49 8B D6 48 8B CD E8 ?? ?? ?? ?? 84 C0 74 ?? FF C6 FF CB",
    AI_S1SITE="4C 8D 46 28 45 8B CF 48 8D 56 10 E8",
    AI_EXEC_PRO="48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 60",
    AI_ISITE="49 8B 52 10 48 81 C2 90 02 00 00 E8",
    AI_ITEM_PRO="48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 48 89 48 08",
    AI_ASITE="45 0F B6 09 48 8B 49 08 E9",
    AI_APPLY_PRO="4C 8B DC 45 88 4B 20 49 89 53 10 55 41 55 48 81 EC D8 00 00 00",
    TR_ESITE="48 8D 4F 08 E8 ?? ?? ?? ?? FF C3 3B DE 75 E0",
    TR_EVAL_PRO="48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC 10 03 00 00",
    TR_ASITE="48 8D 85 90 00 00 00 48 89 44 24 20 4C 8D 8D E0 00 00 00 E8",
    TR_PFOR_CALL="48 8D 95 70 02 00 00 48 8D 4D 90 C5 F8 77 E8",
    TR_APPLY_PRO="4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 53 55 56 57 41 54 41 55 41 56 41 57",
    PG_PAR_PRO="40 55 53 56 57 41 54 41 57 48 8D 6C 24 D1 48 81 EC B8 00 00 00",
    PG_SEQ="48 8D 4D D7 E8 ?? ?? ?? ?? 48 8D 4D D7 E8 ?? ?? ?? ?? 48 8D 4D D7 E8",
    PG_APPLY_PRO="4C 8B DC 55 56 41 54 41 57 49 8D 6B A1 48 81 EC D8 00 00 00",
)
# -------------------------------------------------------------------------------------------------
# expected 1.15.1 RVAs (documentation; the plugin never uses them)
EXPECT = dict(RRP=0x146c960, HEUR=0x146c010, GUARD=0x1468be0, SCHED=0x5c726e8, HAI_EVAL=0x921910, HAI_PRET=0x9f3465,
              HAI_SRET=0x9b2c32, HAI_S2=0x9b2930, HAI_BUILD=0x8dcea0, AI_EXEC=0x1647910, AI_ITEM=0x1604cc0,
              AI_IRET=0x16cb8f8, AI_APPLY=0x1647ab0, TR_EVAL=0xdd26e0, TR_ERET=0xe02e1a, TR_APPLY=0xd96a90,
              PG_PAR=0xe5bbd0, PG_APPLY=0xe5c260)
fails = 0


def check(ok, msg):
    global fails
    print(("OK   " if ok else "FAIL ") + msg)
    fails += not ok
    return ok


def pat(s):
    return re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in s.split()), re.S)


def find_all_text(im, p):
    return [im.tva + m.start() for m in pat(p).finditer(im.tdata)]


def find_in(im, start, n, p):
    o = start - im.tva
    return [im.tva + m.start() for m in pat(p).finditer(im.tdata, o, o + n)]


def at(im, rva, p):
    return pat(p).match(im.tdata, rva - im.tva) is not None


def rel32(im, insn_rva, disp_off):
    return insn_rva + disp_off + 4 + struct.unpack_from('<i', im.tdata, insn_rva + disp_off - im.tva)[0]


def primary(im, rva):
    """function start with chained unwind entries followed (like api->function_start)."""
    f = im.func(rva)
    if not f:
        return None
    pe = im.pe
    s = f[0]
    for _ in range(32):
        # find the RUNTIME_FUNCTION for s and follow UNW_FLAG_CHAININFO
        pd = im.secs['.pdata']
        data = pd.get_data()
        lo, hi = 0, len(data) // 12
        ent = None
        while lo < hi:
            mid = (lo + hi) // 2
            b, e, u = struct.unpack_from('<III', data, mid * 12)
            if s < b:
                hi = mid
            elif s >= e:
                lo = mid + 1
            else:
                ent = (b, e, u)
                break
        if not ent:
            return s
        ui = pe.get_data(ent[2], 4)
        if not ((ui[0] >> 3) & 4):
            return ent[0]
        cnt = ui[2]
        b, e, u = struct.unpack('<III', pe.get_data(ent[2] + 4 + ((cnt + 1) & ~1) * 2, 12))
        s = b
    return s


def fn_of_string(im, s):
    xs = [x for t in im.find_string(s) for x in im.xref.get(t, [])]
    fs = {primary(im, x) for x in xs}
    return (fs.pop() if len(fs) == 1 else None), len(xs)


def unique(im, name, p):
    hits = find_all_text(im, p)
    check(len(hits) == 1, f"{name}: pattern unique in .text ({len(hits)} matches)")
    return hits[0] if len(hits) == 1 else None


im = Img(EXE)
R = {}
# --- range runner, heuristic, scheduler global
rrp, nx = fn_of_string(im, S_RRP)
check(rrp == EXPECT['RRP'], f"RunRangeExecutionParallel = function of '{S_RRP}' ({nx} xrefs): {rrp and hex(rrp)}")
check(rrp and at(im, rrp, P['RRP_PRO']), "RRP prologue")
sch = find_in(im, rrp, 0x300, P['RRP_SCHED']) if rrp else []
check(len(sch) == 1, f"RRP loads the scheduler global and Step end/begin (+0x94/+0x90) once ({len(sch)})")
if sch:
    R['SCHED'] = sch[0] + 7 + struct.unpack_from('<i', im.tdata, sch[0] + 3 - im.tva)[0]
    check(R['SCHED'] == EXPECT['SCHED'], f"scheduler global = {R['SCHED']:#x}")
check(rrp and len(find_in(im, rrp, 0x300, P['RRP_MODE'])) == 1, "RRP reads batch mode Step+0x98 only if Step+0x9c (optional engaged)")
check(rrp and len(find_in(im, sch[0] if sch else rrp, 0x40, P['RRP_WORKERS'])) == 1, "RRP threads = [sched+0x648] + 1")
heur, nx = fn_of_string(im, S_HEUR)
check(heur == EXPECT['HEUR'], f"heuristic = function of '{S_HEUR}' ({nx} xrefs): {heur and hex(heur)}")
check(heur and at(im, heur, P['HEUR_PRO']), "heuristic prologue")
check(heur and len(find_in(im, heur, 0x100, P['HEUR_FLAG'])) == 1, "heuristic tests Step+0xb0 (cost-array optional engaged)")
calls_rrp = [a for a, t in im.calls_in(rrp)] if rrp else []
check(rrp and any(t == heur for a, t in im.calls_in(rrp)), "RRP calls the heuristic")
check(len(im.callsto.get(rrp, [])) == 1 and primary(im, im.callsto[rrp][0]) == 0x146d010,
      f"RRP has exactly one caller, in RunTask ({[hex(c) for c in im.callsto.get(rrp, [])]})")
g = unique(im, "guard dtor (RenderFrameIfNeeded)", P['GUARD_DTOR'])
check(g == EXPECT['GUARD'], f"guard dtor at {g and hex(g)}")
check(g and len(find_in(im, g, 0x40, P['GUARD_10MS'])) == 1, "guard dtor renders only after > 10 ms (cmp rax,0xa)")
R['GUARD'] = g

# --- HandleAutonomousInvestment
ss = unique(im, "HAI serial re-eval site", P['HAI_SSITE'])
ev = rel32(im, ss + 11, 1) if ss else None
check(ev == EXPECT['HAI_EVAL'] and at(im, ev, P['HAI_EVAL_PRO']), f"HAI per-country eval = {ev and hex(ev)} (target of the serial site, prologue)")
check(ss and ss + 16 == EXPECT['HAI_SRET'], "HAI serial-site return address")
pss = [a for a in find_all_text(im, P['HAI_PSITE']) if rel32(im, a + 4, 1) == ev]
check(len(pss) == 1, f"HAI parallel item site: exactly one of {len(find_all_text(im, P['HAI_PSITE']))} shape matches calls the eval ({len(pss)})")
ps = pss[0] if len(pss) == 1 else None
check(ps and ps + 9 == EXPECT['HAI_PRET'], "HAI parallel-site return address")
st2 = primary(im, ss) if ss else None
check(st2 == EXPECT['HAI_S2'] and at(im, st2, P['HAI_S2_PRO']), f"HAI stage-2 apply = function of the serial site {st2 and hex(st2)}")
s2s = [a for a in find_all_text(im, P['HAI_S2SITE']) if rel32(im, a + 24, 1) == st2]
check(len(s2s) == 1, f"HAI stage-2 is the body of exactly one stage functor Do_call ({len(s2s)})")
bs = unique(im, "HAI build site", P['HAI_BSITE'])
bf = rel32(im, bs + 9, 1) if bs else None
check(bf == EXPECT['HAI_BUILD'] and primary(im, bs) == st2, f"HAI build call {bf and hex(bf)} inside stage 2")

# --- UpdateAI
a1 = unique(im, "UpdateAI stage-1 call site", P['AI_S1SITE'])
ex = primary(im, a1) if a1 else None
check(ex == EXPECT['AI_EXEC'] and at(im, ex, P['AI_EXEC_PRO']), f"UpdateAI Execute = {ex and hex(ex)}")
check(ex and len(find_in(im, ex, 0x120, "83 4D 00 03")) == 1, "UpdateAI Execute sets Step.flags |= 3 (parallel + heuristic stage)")
ai = unique(im, "UpdateAI item site", P['AI_ISITE'])
it = rel32(im, ai + 11, 1) if ai else None
check(it == EXPECT['AI_ITEM'] and at(im, it, P['AI_ITEM_PRO']), f"UpdateAI per-subsystem update = {it and hex(it)}")
check(ai and ai + 16 == EXPECT['AI_IRET'], "UpdateAI item return address")
aas = [a for a in find_all_text(im, P['AI_ASITE']) if at(im, rel32(im, a + 8, 1), P['AI_APPLY_PRO'])]
check(len(aas) == 1, f"UpdateAI stage-3 thunk: exactly one jmp shape reaches the apply prologue ({len(aas)})")
ap = rel32(im, aas[0] + 8, 1) if len(aas) == 1 else None
check(ap == EXPECT['AI_APPLY'], f"UpdateAI stage-3 per-item apply = {ap and hex(ap)}")
check(ap and len(find_in(im, ap, 0x200, "E8 ?? ?? ?? ??")) and 0x1085b90 in [rel32(im, x, 1) for x in find_in(im, ap, 0x200, "E8 ?? ?? ?? ??")],
      "UpdateAI stage-3 item calls ExecuteGameCommand 01085b90")

# --- UpdateStateTrades
ta = unique(im, "trades apply site", P['TR_ASITE'])
tp = rel32(im, ta + 19, 1) if ta else None
check(tp == EXPECT['TR_APPLY'] and at(im, tp, P['TR_APPLY_PRO']), f"trades per-record apply = {tp and hex(tp)}")
tfn = primary(im, ta) if ta else None
check(tfn == 0xdd1650, f"trades apply site lies in UpdateStateTrades {tfn and hex(tfn)}")
pc = find_in(im, tfn, 0x1200, P['TR_PFOR_CALL']) if tfn else []
check(len(pc) == 1, f"UpdateStateTrades calls its parallel-for once ({len(pc)})")
pf = rel32(im, pc[0] + 14, 1) if len(pc) == 1 else None
check(pf == 0xe02c90, f"trades parallel-for = {pf and hex(pf)}")
te_ = find_in(im, pf, 0x200, P['TR_ESITE']) if pf else []
check(len(te_) == 1, f"inline per-record eval site inside the parallel-for ({len(te_)})")
te = te_[0] if len(te_) == 1 else None
tv = rel32(im, te + 4, 1) if te else None
check(tv == EXPECT['TR_EVAL'] and at(im, tv, P['TR_EVAL_PRO']), f"trades per-record eval = {tv and hex(tv)}")
check(te and te + 9 == EXPECT['TR_ERET'], "trades inline-eval return address")

# --- UpdatePopGrowth
pg, nx = fn_of_string(im, S_PGPAR)
check(pg == EXPECT['PG_PAR'] and at(im, pg, P['PG_PAR_PRO']), f"UpdatePopGrowthParallel = {pg and hex(pg)}")
sq = unique(im, "pop-growth stage sequence", P['PG_SEQ'])
check(sq and rel32(im, sq + 13, 1) == pg, "sequence: 2nd call = UpdatePopGrowthParallel")
pa = rel32(im, sq + 22, 1) if sq else None
check(pa == EXPECT['PG_APPLY'] and at(im, pa, P['PG_APPLY_PRO']), f"pop-growth serial apply = {pa and hex(pa)}")

# --- static facts used by the verdicts (docs/re/ai_par.md section 1)
check(at(im, 0x146cb4d, "41 8B D7 45 85 FF 74 2D 83 EA 01 74 11 83 EA 01 74 2B 83 FA 01 75 26 BB FF FF FF FF"),
      "RRP grain: mode0 n/T, mode1 n/T/3, mode2 = 1, mode3 = whole range inline")
check(at(im, 0x146ccdf, "8B C3 41 2B C4 FF C8 41 03 C6 99 F7 FB 8B D8 39 44 24 30 0F 4C 5C 24 30 83 FB 01"),
      "RRP chunks = min(ceil(n/grain), T); 1 chunk -> inline")
check(at(im, 0x14d3a5f, "48 8B 79 08 8B 6F 20 8B C5 8B 77 18 F0 0F C1 07"),
      "chunk task: lock xadd [job+0x130], grain loop (dynamic self-scheduling)")
check(at(im, 0x146c5e9, "C7 45 E7 01 00 00 00 C7 45 EB 02 00 00 00"),
      "heuristic: expensive list runs with grain 1 / mode 2 (engine itself uses grain 1)")
check(at(im, 0x146c08b, "45 8B 75 10 45 2B 75 0C 48 8B 05 ?? ?? ?? ?? 8B 80 48 06 00 00 90 03 C0 44 3B F0 0F 8E"),
      "heuristic only if n > 2 * workers")
check(at(im, 0x146ca11, "44 8B F6 4C 87 35"), "RRP swaps the script-RNG owner out (xchg) for the whole range")
check(at(im, 0x149d328, "C7 44 24 28 01 00 00 00"), "fork submits chunk tasks to pool kind 1 (NORMAL workers only)")
check(at(im, 0x14abde8, "B8 48 06 00 00 41 83 F8 01"), "submit: kind 1 worker count = sched+0x648")
check(at(im, 0x9799e5, "83 0E 03"), "HAI Execute sets Step.flags |= 3 (parallel stage with cost heuristic)")
check(at(im, 0x979920, "49 8B 00 48 8D 52 04 4D 8D 40 08 8B 88 E4 0B 00 00 03 88 6C 0B 00 00"),
      "HAI cost per country = [c+0xbe4] + [c+0xb6c]")
check(at(im, 0x16cb8c6, "B8 9F BA A6 5E 44 03 CB 41 2B C1"), "UpdateAI item seed = (country id + item index, 0x5ea6ba9f - that): local, per item")
check(at(im, 0xdd22c0, "8B 89 48 06 00 00"), "trades parallel-for: T = [sched+0x648] + 1, grain max(n/T/3, 1) hard-coded in code")
print(f"{'ALL OK' if not fails else str(fails) + ' FAILED'}")
sys.exit(1 if fails else 0)
