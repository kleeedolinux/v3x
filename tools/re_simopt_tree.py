"""Hot call tree below (or callers above) a function, from the devtools sampling profile.

Inputs: devtools profile_<..>.csv (rva,samples,inclusive,module) and profile_edges_<..>.csv
(caller_rva,callee_rva,samples,...). Edge samples = samples whose unwound stack (<= unwind_depth
frames, default 8) contained caller directly above callee, i.e. ~inclusive time of callee via caller.
Names: tools/symbolize.py string hints (best effort).

Usage: re_simopt_tree.py PROFILE EDGES [--down|--up] [--depth 4] [--min 0.02] RVA [RVA ...]
  --min: drop children below this fraction of the root's inclusive samples.
"""
import argparse, csv, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symbolize

EXE = symbolize.DEFAULT_EXE


def load(prof, edges):
    fn = {}
    for r in csv.DictReader(open(prof)):
        if r['module'] == 'victoria3.exe':
            fn[int(r['rva'], 16)] = (int(r['samples']), int(r['inclusive']))
    down, up = {}, {}
    for r in csv.DictReader(open(edges)):
        if r['caller_module'] != 'victoria3.exe' or r['callee_module'] != 'victoria3.exe':
            continue
        a, b, s = int(r['caller_rva'], 16), int(r['callee_rva'], 16), int(r['samples'])
        down.setdefault(a, []).append((s, b))
        up.setdefault(b, []).append((s, a))
    return fn, down, up


_img = None
def name(rva):
    global _img
    if _img is None:
        _img = symbolize.Image(EXE)
    f = _img.function(rva)
    if not f:
        return '?'
    h = symbolize.hints(_img, f[0], f[1], 2)
    return '; '.join(h)[:110] + f'  [size {f[1]-f[0]}]'


def walk(g, fn, root, depth, minfrac):
    incl = fn.get(root, (0, 0))[1] or 1
    print(f"== {root:08x} self={fn.get(root,(0,0))[0]} incl={incl}  {name(root)}")
    seen = {root}

    def rec(n, d, pre):
        if d >= depth:
            return
        for s, c in sorted(g.get(n, []), reverse=True):
            if s < minfrac * incl:
                break
            mark = ' (seen)' if c in seen else ''
            print(f"{pre}{s:9d} {100.0*s/incl:5.1f}%  {c:08x} self={fn.get(c,(0,0))[0]:<7d} {name(c)}{mark}")
            if c not in seen:
                seen.add(c)
                rec(c, d + 1, pre + '  ')
    rec(root, 0, '  ')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile'); ap.add_argument('edges')
    ap.add_argument('--up', action='store_true'); ap.add_argument('--down', action='store_true')
    ap.add_argument('--depth', type=int, default=4); ap.add_argument('--min', type=float, default=0.02)
    ap.add_argument('rva', nargs='+')
    a = ap.parse_args()
    fn, down, up = load(a.profile, a.edges)
    for h in a.rva:
        walk(up if a.up else down, fn, int(h, 16), a.depth, a.min)


if __name__ == '__main__':
    main()
