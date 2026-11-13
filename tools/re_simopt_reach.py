"""Static reachability from tick-task bodies, joined with the sampling profile, to find the hot
inner functions of a task even though the profiler only unwinds ~8 frames.

Edges (byte scan inside each .pdata function): E8/E9 rel32 to a function start (call, tail jmp),
and RIP-relative lea/mov targets that are a function start (lambdas / callbacks) or an .rdata
vtable (consecutive qwords pointing at function starts, up to 32 slots). BFS to --depth.

Per task we report hot functions (profile self samples) reachable from it and how many of the
analysed tasks also reach them ('shared'): low shared = attributable to this task.

Usage: re_simopt_reach.py PROFILE [--depth 6] [--top 25] NAME=RVA [NAME=RVA ...]
"""
import argparse, bisect, csv, os, struct, sys, pickle
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_sim_lib import Img, PAT
import symbolize

EXE = symbolize.DEFAULT_EXE


class Graph:
    def __init__(self, im):
        self.im = im
        self.fs = im._fset()
        rd = im.secs['.rdata']
        self.rd, self.rva0, self.rdd = rd, rd.VirtualAddress, rd.get_data()
        self.cache = {}
        self.generic = 150

    def vtable_targets(self, t):
        out = []
        o = t - self.rva0
        for k in range(32):
            if o + 8 * k + 8 > len(self.rdd):
                break
            v = struct.unpack_from('<Q', self.rdd, o + 8 * k)[0] - self.im.base
            if v in self.fs:
                out.append(v)
            else:
                break
        return out

    def succ(self, f):
        if f in self.cache:
            return self.cache[f]
        im = self.im
        r = im.func(f)
        if not r:   # leaf thunk without .pdata: scan to the int3 padding
            j = f - im.tva
            while j < len(im.tdata) - 1 and not (im.tdata[j] == 0xCC and im.tdata[j + 1] == 0xCC) and j - (f - im.tva) < 512:
                j += 1
            r = (f, im.tva + j + 5)
        res = set()
        if r:
            s, e = r
            d, va = im.tdata, im.tva
            for i in range(s - va, e - va - 4):
                b = d[i]
                if b == 0xE8 or b == 0xE9:
                    t = va + i + 5 + struct.unpack_from('<i', d, i + 1)[0]
                    if t in self.fs and t != s:
                        res.add(t)
                elif b == 0x0F and 0x80 <= d[i + 1] <= 0x8F and i + 6 <= e - va:
                    t = va + i + 6 + struct.unpack_from('<i', d, i + 2)[0]
                    if t in self.fs and t != s:
                        res.add(t)
            for m in PAT.finditer(d, s - va, e - va):
                i = m.start()
                t = va + i + 7 + struct.unpack_from('<i', d, i + 3)[0]
                if t in self.fs:
                    res.add(t)
                elif self.rva0 <= t < self.rva0 + len(self.rdd):
                    res.update(self.vtable_targets(t))
        self.cache[f] = res
        return res

    def reach(self, root, depth):
        dist = {root: 0}
        fr = [root]
        for dd in range(1, depth + 1):
            nx = []
            for f in fr:
                if f != root and len(self.im.callsto.get(f, ())) > self.generic:
                    continue   # utility with many callers: count it, do not expand
                for t in self.succ(f):
                    if t not in dist:
                        dist[t] = dd
                        nx.append(t)
            fr = nx
        return dist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile'); ap.add_argument('--depth', type=int, default=6)
    ap.add_argument('--top', type=int, default=25); ap.add_argument('--maxshared', type=int, default=99)
    ap.add_argument('--generic', type=int, default=150, help='do not expand fns with more direct callers')
    ap.add_argument('tasks', nargs='+')
    a = ap.parse_args()
    prof = {}
    for r in csv.DictReader(open(a.profile)):
        if r['module'] == 'victoria3.exe':
            prof[int(r['rva'], 16)] = (int(r['samples']), int(r['inclusive']))
    im = Img(EXE)
    g = Graph(im)
    g.generic = a.generic
    simg = symbolize.Image(EXE)
    reach = {}
    for t in a.tasks:
        n, h = t.split('=')
        reach[n] = g.reach(int(h, 16), a.depth)
    cnt = {}
    for n, d in reach.items():
        for f in d:
            cnt[f] = cnt.get(f, 0) + 1
    for n, d in reach.items():
        hot = sorted(((prof[f][0], f) for f in d if f in prof and cnt[f] <= a.maxshared), reverse=True)
        tot = sum(s for s, _ in hot)
        print(f"== {n}: {len(d)} fns reachable (depth {a.depth}), hot self samples {tot}")
        for s, f in hot[:a.top]:
            rr = simg.function(f)
            hn = '; '.join(symbolize.hints(simg, rr[0], rr[1], 2))[:100] if rr else '?'
            print(f"  {s:8d} {f:08x} d={d[f]} shared={cnt[f]} incl={prof[f][1]:<8d} {hn}")


if __name__ == '__main__':
    main()
