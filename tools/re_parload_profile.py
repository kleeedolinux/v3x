"""par_load: rank the serial steps under the game-init call tree from devtools profile CSVs.

Usage:
  re_parload_profile.py <edges.csv> [--nodes nodes.csv] [--root 0x6aff80] [--depth 4] [--min 200]
                        [--exe ../binaries/victoria3.exe]

edges.csv = devtools `profile_edges_*.csv` (caller_rva,callee_rva,samples,caller_module,callee_module).
An edge's samples = number of stack samples that contain caller->callee (inclusive). With --exe each
node is annotated with the first anchor-like strings referenced in its body (tools/re_sim_lib.py index;
set RE_SIM_CACHE to keep the pickle outside the repo).
The tree is printed depth-first; recursive edges (callee already on the path) are cut.
"""
import argparse, collections, csv, os, sys


def load_edges(path):
    kids = collections.defaultdict(list)
    for r in csv.DictReader(open(path, newline='')):
        if r['caller_module'] != 'victoria3.exe':
            continue
        a, b = int(r['caller_rva'], 16), int(r['callee_rva'], 16)
        mod = r['callee_module']
        kids[a].append((int(r['samples']), b, mod))
    for v in kids.values():
        v.sort(reverse=True)
    return kids


def load_nodes(path):
    out = {}
    if not path:
        return out
    for r in csv.DictReader(open(path, newline='')):
        if r['module'] == 'victoria3.exe':
            out[int(r['rva'], 16)] = (int(r['samples']), int(r['inclusive']))
    return out


def label(im, rva, cache={}):
    if im is None:
        return ''
    if rva in cache:
        return cache[rva]
    s = ''
    try:
        ss = [t for _, _, t in im.strs_in(rva)]
        pick = [t for t in ss if '\\' in t and '.cpp' in t] + [t for t in ss if '\\' not in t]
        s = ' | '.join(x[:60] for x in pick[:2])
    except Exception:
        pass
    cache[rva] = s
    return s


def walk(kids, nodes, im, rva, depth, mn, path, out, ind=0):
    for smp, c, mod in kids.get(rva, []):
        if smp < mn:
            break
        if c in path:
            out.append(f"{'  ' * ind}  0x{c:x} (recursive) {smp}")
            continue
        selfs = nodes.get(c, (0, 0))[0] if mod == 'victoria3.exe' else 0
        lab = label(im, c) if mod == 'victoria3.exe' else mod
        out.append(f"{'  ' * ind}{smp:7d}  0x{c:x} self={selfs}  {lab}")
        if depth > 1 and mod == 'victoria3.exe':
            walk(kids, nodes, im, c, depth - 1, mn, path | {c}, out, ind + 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('edges')
    ap.add_argument('--nodes')
    ap.add_argument('--root', default='0x6aff80')
    ap.add_argument('--depth', type=int, default=4)
    ap.add_argument('--min', type=int, default=200)
    ap.add_argument('--exe')
    a = ap.parse_args()
    im = None
    if a.exe:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from re_sim_lib import Img
        im = Img(a.exe)
    kids = load_edges(a.edges)
    nodes = load_nodes(a.nodes)
    root = int(a.root, 16)
    out = [f"root 0x{root:x}  {label(im, root)}"]
    walk(kids, nodes, im, root, a.depth, a.min, {root}, out)
    print('\n'.join(out))


if __name__ == '__main__':
    main()
