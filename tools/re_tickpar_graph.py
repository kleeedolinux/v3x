"""tick_par: static tick-task dependency graph of victoria3.exe 1.15.1 + overlap potential.

The engine stores each task's dependencies as a CPdxArray<CString> of task NAMES in its
CJominiTickTaskType (+0x30 data, +0x3c count; filled by the static initializer / config helper).
This tool recovers those names statically:
  (a) rip-relative loads from the start of a NUL-terminated .rdata literal (as re_sim_ticktasks.py),
  (b) SSO strings assembled from immediates: `movabs r, imm64; mov [mem], r` followed by
      `mov dword/word/byte [mem+k], imm` (short names such as "UpdateAI" are never in .rdata).
Every candidate must be the exact name of a registered task (other strings are dropped).
It is still static: the plugin's measure mode dumps the authoritative runtime graph
(binaries/v3x/tick_par_graph.csv) read from the live type objects.

Then it emulates the engine's order (0x2fd5e70: stable sort by priority then name, DFS that emits
dependencies first) and, with per-task times from tick_profile.csv, estimates what fork/join could save:
  * per due-set (TICK, TICK+DAY, +WEEK, +MONTH) serial sum vs. DAG critical path (infinite lanes),
  * 2-lane greedy list schedule where only `--offload` candidates may leave the main lane.
Usage: re_tickpar_graph.py <exe> [--profile tick_profile.csv] [--csv out.csv] [--offload A,B,...] [--exclude-parallel]
"""
import argparse, csv, os, re, struct, sys
import capstone
from capstone import x86 as X
from re_sim_lib import Img

EXE = None
CTOR = 0x2fd59d0         # CJominiTickTaskType ctor (rcx = type object, rdx = name), 272 callers
RATE = ['TICK', 'DAY', 'WEEK', 'MONTH', 'QUARTER', 'YEAR']
PRIO = ['EARLY', 'DEFAULT', 'LATE']


def cstr_at(im, rva):
    rd = im.secs['.rdata']; d = rd.get_data(); o = rva - rd.VirtualAddress
    if not (0 < o < len(d)) or d[o - 1] != 0:
        return None
    j = d.find(b'\0', o, o + 200)
    s = d[o:j]
    return s.decode() if len(s) >= 3 and re.fullmatch(rb'[A-Za-z0-9_]+', s) else None


def memkey(op):
    m = op.mem
    return (m.base, m.index, m.scale, m.disp)


def strings_in(im, md, s, e):
    """candidate strings in [s,e): rdata literal refs + SSO immediates."""
    out = []
    regimm = {}          # reg -> 8-byte immediate (movabs)
    bufs = {}            # (base,index,scale) -> {disp: byte}
    for i in md.disasm(im.tdata[s - im.tva:e - im.tva], s):
        for op in i.operands:
            if op.type == X.X86_OP_MEM and op.mem.base == X.X86_REG_RIP:
                c = cstr_at(im, i.address + i.size + op.mem.disp)
                if c: out.append(c)
        if i.mnemonic == 'movabs' and len(i.operands) == 2 and i.operands[1].type == X.X86_OP_IMM:
            regimm[i.operands[0].reg] = i.operands[1].imm & 0xffffffffffffffff
            continue
        if i.mnemonic == 'mov' and len(i.operands) == 2 and i.operands[0].type == X.X86_OP_MEM \
                and i.operands[0].mem.base != X.X86_REG_RIP:
            dst, src = i.operands
            k = memkey(dst); base = k[:3]; disp = k[3]
            val = None; n = dst.size
            if src.type == X.X86_OP_IMM:
                val = src.imm & ((1 << (8 * n)) - 1)
            elif src.type == X.X86_OP_REG and src.reg in regimm and n == 8:
                val = regimm[src.reg]
            if val is not None:
                b = bufs.setdefault(base, {})
                if n == 8:
                    b.clear()                     # a new qword at a buffer start begins a new string
                for j in range(n):
                    b[disp + j] = (val >> (8 * j)) & 0xff
                # assemble from the lowest offset of this buffer
                lo = min(b); bs = bytearray()
                while lo in b and b[lo] != 0:
                    bs.append(b[lo]); lo += 1
                if len(bs) >= 3 and re.fullmatch(rb'[A-Za-z0-9_]+', bytes(bs)):
                    out.append(bs.decode())
        if i.mnemonic == 'call':
            regimm.clear()
    return out


def base_rows(im):
    """rate/prio/init per task from re_sim_ticktasks.py (the verified registry extractor)."""
    import io, contextlib, re_sim_ticktasks as rt
    buf = io.StringIO(); argv = sys.argv
    sys.argv = ['re_sim_ticktasks.py', im.pe.__dict__.get('_exe', EXE), '%x' % CTOR]
    try:
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            rt.main()
    finally:
        sys.argv = argv
    rows = {}
    for ln in buf.getvalue().splitlines():
        m = re.match(r'(\S+)\s+(\S+)\s+(\S+)\s+init=([0-9a-f]+) .*deps=(.*)$', ln)
        if m:
            rows[m.group(1)] = (m.group(2), m.group(3), int(m.group(4), 16), [d for d in m.group(5).split(',') if d])
    return rows


def extract(im):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
    base = base_rows(im)
    names = set(base)
    g = {}
    for name, (r, p, init, deps0) in base.items():
        f = im.func(init)
        cands = list(deps0)
        if f:
            cands += strings_in(im, md, f[0], f[1])
            # the config helper (first few direct calls) also pushes dependency names
            for a, t in im.calls_in(init)[:4]:
                hf = im.func(t)
                if hf and hf[1] - hf[0] < 0x2000 and t not in (CTOR,):
                    cands += strings_in(im, md, hf[0], hf[1])
        deps = []
        for c in cands:
            if c in names and c != name and c not in deps:
                deps.append(c)
        g[name] = {'rate': RATE.index(r) if r in RATE else 9, 'prio': PRIO.index(p) if p in PRIO else 1,
                   'deps': deps, 'deps_lit': [d for d in deps0 if d in names], 'init': init}
    return g


def engine_order(g):
    """0x2fd5e70: stable insertion sort by (prio, name), then pop front + DFS (deps first)."""
    rem = sorted(g, key=lambda n: (g[n]['prio'], n))
    out = []

    def visit(n):
        for d in g[n]['deps']:
            if d in rem:
                rem.remove(d)
                visit(d)
        out.append(n)
    while rem:
        visit(rem.pop(0))
    return out


def closure(g):
    anc = {}

    def a(n, stack=()):
        if n in anc:
            return anc[n]
        s = set()
        for d in g[n]['deps']:
            if d in g and d not in stack:
                s.add(d); s |= a(d, stack + (n,))
        anc[n] = s
        return s
    for n in g:
        a(n)
    return anc


def load_profile(path):
    t = {}
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            n = row['task'].split('CTickTask_')[-1]
            us = float(row['avg_us'])
            t[n] = max(t.get(n, 0.0), us)   # ?$ template rows collapse; keep the larger
    return t


def schedule(order, g, anc, dur, cand, corun=None):
    """same policy as plugins/tick_par/tick_par_sched.h tp_simulate: one extra lane, main lane keeps the
    engine order; join when the in-flight task is done, or before a task that depends on it / may not
    co-run, and at the end. Returns (makespan, {task: overlapped time})."""
    corun = cand if corun is None else corun
    main_t = 0.0; lane = None; saved = {}
    n = len(order)

    def join():
        nonlocal main_t, lane
        a, st, end = lane
        ov = min(main_t, end) - st
        if ov > 0: saved[a] = saved.get(a, 0.0) + ov
        main_t = max(main_t, end); lane = None
    for k, t in enumerate(order):
        if lane and (lane[2] <= main_t or lane[0] in anc[t] or t not in corun):
            join()
        nx = order[k + 1] if k + 1 < n else None
        if t in cand and t in corun and lane is None and nx and t not in anc[nx] and nx in corun:
            lane = (t, main_t, main_t + dur.get(t, 0.0))
        else:
            main_t += dur.get(t, 0.0)
    if lane:
        join()
    return main_t, saved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe'); ap.add_argument('--profile'); ap.add_argument('--csv')
    ap.add_argument('--offload', default='')
    ap.add_argument('--exclude-parallel', action='store_true',
                    help='tasks marked range-parallel in docs/re/simulation.md (plus UpdateAI, whose stage 2 is parallel) '
                         'may neither leave the main lane nor co-run: they already use all workers')
    a = ap.parse_args()
    global EXE
    EXE = a.exe
    im = Img(a.exe)
    g = extract(im)
    order = engine_order(g)
    anc = closure(g)
    nd = sum(len(v['deps']) for v in g.values())
    print(f"# {len(g)} tasks, {nd} declared dependency edges")
    if a.csv:
        with open(a.csv, 'w', newline='') as f:
            w = csv.writer(f); w.writerow(['order', 'task', 'rate', 'prio', 'deps'])
            for i, n in enumerate(order):
                v = g[n]
                w.writerow([i, n, RATE[v['rate']] if v['rate'] < 6 else v['rate'],
                            PRIO[v['prio']] if v['prio'] < 3 else v['prio'], ' '.join(v['deps'])])
    if not a.profile:
        return
    dur = load_profile(a.profile)
    off = set(x for x in a.offload.split(',') if x)
    excl = set()
    if a.exclude_parallel:
        doc = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 're', 'simulation.md')
        for ln in open(doc, encoding='utf-8'):
            c = [x.strip() for x in ln.split('|')]
            if len(c) > 5 and c[4] == 'yes':
                excl.add(c[1])
        excl.add('UpdateAI')
        print(f"# excluded (already parallel inside): {len(excl)} tasks")
    print("dueset            n   serial_ms  critpath_ms  2lane(all)_ms  2lane(--offload)_ms")
    for top in range(4):
        due = [n for n in order if g[n]['rate'] <= top]
        ser = sum(dur.get(n, 0.0) for n in due) / 1000
        fin = {}
        for n in due:
            fin[n] = max([fin[x] for x in anc[n] if x in fin] + [0.0]) + dur.get(n, 0.0)
        cp = max(fin.values()) / 1000
        allc = set(due) - excl
        two, sv = schedule(due, g, anc, dur, allc)
        two /= 1000
        # what-if: only the --offload tasks leave the main lane, every task may co-run with them
        twoo = schedule(due, g, anc, dur, off, set(due))[0] / 1000
        tops = sorted(sv.items(), key=lambda kv: -kv[1])[:6]
        top_s = ', '.join(f'{k} {v / 1000:.1f}' for k, v in tops)
        print(f"{'+'.join(RATE[:top + 1]):16s} {len(due):3d} {ser:10.1f} {cp:12.1f} {two:14.1f} {twoo:20.1f}")
        print(f"   top overlapped (ms): {top_s}")
    # the critical path's members for the TICK set
    due = [n for n in order if g[n]['rate'] == 0]
    print("TICK tasks (engine order) avg_ms deps:")
    for n in due:
        print(f"  {n:45s} {dur.get(n, 0) / 1000:7.2f}  {','.join(g[n]['deps'])}")


if __name__ == '__main__':
    main()
