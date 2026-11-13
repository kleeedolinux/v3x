"""Walk caller chains upward (or callees downward) in a devtools edges CSV and name each function.

Usage:
  re_map_chain.py EDGES.csv RVA [RVA...] [--exe victoria3.exe] [--depth 8] [--fan 3] [--down]
  EDGES.csv = v3x/profile_edges_<from>_to_<to>_<n>.csv (caller_rva,callee_rva,samples,caller_module,callee_module)

For every RVA it prints the top --fan callers (or callees with --down) by samples, recursively up to
--depth, each with symbolize.py hints. Only victoria3.exe -> victoria3.exe edges are used.
Run WITHOUT python -I (needs pefile from user site-packages).
"""
import argparse, csv, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symbolize  # noqa: E402


def load_edges(path, exe_name):
    up, down = collections.defaultdict(list), collections.defaultdict(list)
    with open(path, newline='') as f:
        for r in csv.DictReader(l for l in f if not l.startswith('#')):
            if (r.get('caller_module') or exe_name).lower() != exe_name or (r.get('callee_module') or exe_name).lower() != exe_name:
                continue
            a, b, s = int(r['caller_rva'], 16), int(r['callee_rva'], 16), int(r['samples'])
            up[b].append((s, a))
            down[a].append((s, b))
    for d in (up, down):
        for k in d:
            d[k].sort(reverse=True)
    return up, down


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('edges')
    ap.add_argument('rvas', nargs='+')
    ap.add_argument('--exe', default=symbolize.DEFAULT_EXE)
    ap.add_argument('--depth', type=int, default=8)
    ap.add_argument('--fan', type=int, default=3)
    ap.add_argument('--down', action='store_true')
    a = ap.parse_args()
    img = symbolize.Image(a.exe)
    up, down = load_edges(a.edges, os.path.basename(a.exe).lower())
    g = down if a.down else up
    cache = {}

    def name(rva):
        if rva not in cache:
            f = img.function(rva)
            cache[rva] = ' '.join(symbolize.hints(img, f[0], f[1], 2)) if f else '(no .pdata)'
        return cache[rva]

    def walk(rva, depth, seen, indent):
        if depth > a.depth:
            return
        for s, nxt in g.get(rva, [])[:a.fan]:
            loop = nxt in seen
            print(f"{'  ' * indent}{'<-' if not a.down else '->'} {nxt:08x} {s:8d}  {name(nxt)}{'  (loop)' if loop else ''}")
            if not loop:
                walk(nxt, depth + 1, seen | {nxt}, indent + 1)

    for x in a.rvas:
        r = int(x, 16)
        print(f"{r:08x}  {name(r)}")
        walk(r, 1, {r}, 1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
