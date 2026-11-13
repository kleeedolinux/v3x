"""Offline anchor check for plugins/tick_par against victoria3.exe: runs the same lookups as
install() in tick_par.c (string xrefs, unique byte shapes, call targets) plus the static facts the
plugin relies on (type layout, serial runner). Exit code 0 = ALL OK.
Usage: re_tickpar_check.py [exe]"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_sim_lib import Img

EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'binaries', 'victoria3.exe')

P = dict(
    RUNNER_PRO="48 8B C4 48 89 50 10 55 53 56 57",
    BUILD_CALL="4C 8B C3 48 8D 55 F0 49 8B CD E8",
    RUN_CALL="49 8B D7 49 8B CD E8",
    INST_TYPE="49 8B 57 08 48 8B 4D 18 E8",
    BUILD_PRO="48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 55 41 56 48 83 EC 30",
    BUILD_SORT="49 8B F0 48 8B FA 4C 8B E9 E8",
    BUILD_RATE="8B 43 28 83 F8 01",
    TOPO_VISIT="4C 89 44 24 18 48 89 4C 24 08 53 56 57 41 57 48 83 EC 38 48 8B 71 30 4D 8B F8 48 63 79 3C",
)
fails = 0


def check(ok, msg):
    global fails
    print(("OK   " if ok else "FAIL ") + msg)
    fails += not ok
    return ok


def pat(s):
    return re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in s.split()), re.S)


def find_all(im, start, n, p):
    o = start - im.tva
    return [im.tva + m.start() for m in pat(p).finditer(im.tdata, o, o + n)]


def at(im, rva, p):
    o = rva - im.tva
    return pat(p).match(im.tdata, o) is not None


def rel(im, e8):
    return e8 + 5 + struct.unpack_from('<i', im.tdata, e8 + 1 - im.tva)[0]


def fn_of_string(im, s):
    ts = im.find_string(s)
    xs = [x for t in ts for x in im.xref.get(t, [])]
    return (im.func(xs[0])[0] if len(xs) == 1 and im.func(xs[0]) else None), len(xs)


im = Img(EXE)
runner, nx = fn_of_string(im, "Tick Tasks for {}")
check(runner == 0x14736c0, f"runner = function of the only xref to 'Tick Tasks for {{}}': {runner and hex(runner)} ({nx} xrefs)")
check(runner and at(im, runner, P['RUNNER_PRO']), "runner prologue")
ctx = [x for t in im.find_string("Context %d") for x in im.xref.get(t, [])]
runtask = im.func(ctx[0])[0] if ctx else None
check(runtask == 0x146d010, f"RunTask = function of first xref to 'Context %d': {runtask and hex(runtask)}")
b = find_all(im, runner, 0xd00, P['BUILD_CALL']); r = find_all(im, runner, 0xd00, P['RUN_CALL']); it = find_all(im, runner, 0xd00, P['INST_TYPE'])
check(len(b) == 1 and len(r) == 1 and len(it) == 1, f"runner shapes once each: builder call {len(b)}, RunTask call {len(r)}, instance->type {len(it)}")
if len(b) == 1 and len(r) == 1:
    bcall, rcall = b[0] + 10, r[0] + 6
    check(rcall == 0x1473c78, f"RunTask call site {rcall:#x}")
    check(rel(im, rcall) == runtask, "call site targets RunTask")
    builder = rel(im, bcall)
    check(builder == 0x146bce0 and at(im, builder, P['BUILD_PRO']), f"builder {builder:#x} + prologue")
    s = find_all(im, builder, 0x80, P['BUILD_SORT']); rt = find_all(im, builder, 0x100, P['BUILD_RATE'])
    check(len(s) == 1 and len(rt) == 1, f"builder shapes: sorted call {len(s)}, rate read type+0x28 {len(rt)}")
    if s:
        sorted_ = rel(im, s[0] + 9)
        own, n2 = fn_of_string(im, "Two tick tasks with the same name, not allowed!")
        check(sorted_ == 0x2fd5e70 and own == sorted_, f"sorted registry {sorted_:#x} owns the duplicate-name assert ({n2} xrefs)")
        calls = [t for a, t in im.calls_in(sorted_)]
        tv = find_all(im, im.tva, len(im.tdata), P['TOPO_VISIT'])
        check(len(tv) == 1 and tv[0] in calls, f"dependency DFS {tv and hex(tv[0])} (reads type+0x30/+0x3c) unique and called by the sorter")
    # the runner loop calls RunTask from exactly one site (strictly serial loop)
    rs = [a for a, t in im.calls_in(runner) if t == runtask]
    check(len(rs) == 1, f"runner calls RunTask from {len(rs)} site(s) (serial loop)")
    callers = im.callsto.get(runtask, [])
    check(sorted(callers) == [0x1472ec3, 0x1473c78],
          f"RunTask direct callers: {[hex(c) for c in callers]}")
# type ctor layout: name +8, rate +0x28 = 0, prio +0x2c = 1, deps array +0x30, flag +0x48 = 1, order +0x50
ctor = 0x2fd59d0
txt = ' '.join(f"{i.mnemonic} {i.op_str}" for i in im.dis(ctor, 40))
for frag in ("dword ptr [rdi + 0x28], r15d", "dword ptr [rdi + 0x2c], 1", "lea rcx, [rdi + 0x30]", "byte ptr [rdi + 0x48], 1",
             "qword ptr [rdi + 0x50], 0xffffffffffffffff"):
    check(frag in txt, f"type ctor {ctor:#x}: {frag}")
check(len(im.callsto.get(ctor, [])) == 272, f"type ctor callers = 272 registered tasks ({len(im.callsto.get(ctor, []))})")
print("ALL OK" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
