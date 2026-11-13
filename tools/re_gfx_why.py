"""Why is texture loading slow? Evidence summary for docs/re/lazy_gfx.md (worker lazygfx).

Usage:
  python -I tools/re_gfx_why.py edges EDGES.csv [--ct 0x3C5AF20]
      CreateTexture subtree from a devtools edges CSV (caller_rva,callee_rva,samples,caller_module,callee_module):
      direct callees of CreateTexture with samples and share, and the Win32 leaf calls under the PhysFS probes
      (KernelBase / kernel32 / d3d11 exports resolved from this machine's System32 DLLs; needs pefile).
  python -I tools/re_gfx_why.py log v3x.log
      map_cache 'spike' and 'load' lines: per spike reads/MB/ms and ms per read; per-file loads by type.
  python -I tools/re_gfx_why.py csv lazy_gfx_textures.csv [--top 15]
      lazy_gfx per-texture records: stage totals per phase x thread, PNG vs DDS, top requester RVAs.
"""
import argparse, collections, csv, os, re, sys

STAGES = ['opts', 'stat', 'open', 'read', 'decode', 'upload', 'obj', 'stamp']
# 1.15.1 CreateTexture callees (docs/re/lazy_gfx.md)
NAMES = {0x3E1C4F0: 'VFSOpen (PhysFS getRealDir + openRead)', 0x3C42EF0: 'texture object (incl. stamp getRealDir)',
         0x3E1C090: 'file read', 0x3C6AC30: 'DX11 CreatePixelBuffer (CreateTexture2D)', 0x3C5E0D0: 'decode (DDS/FreeImage)',
         0x3E1C230: 'file close', 0x847FE0: 'Reserve (file buffer)', 0x3E1C130: 'file size', 0x3C5ABB0: 'path view',
         0x3C5DF50: 'load_settings lookup'}


def exports(dll):
    try:
        import pefile
    except ImportError:
        return []
    path = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', dll)
    if not os.path.exists(path):
        return []
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
    return sorted((e.address, (e.name or b'?').decode()) for e in getattr(pe, 'DIRECTORY_ENTRY_EXPORT').symbols)


_exp = {}


def sym(mod, rva):
    if mod.lower() == 'victoria3.exe':
        return NAMES.get(rva, '')
    if mod not in _exp:
        _exp[mod] = exports(mod)
    best = None
    for a, n in _exp[mod]:
        if a <= rva:
            best = (a, n)
        else:
            break
    return f'{best[1]}+0x{rva - best[0]:x}' if best else ''


def cmd_edges(a):
    down = collections.defaultdict(list)
    with open(a.edges, newline='') as f:
        for r in csv.DictReader(l for l in f if not l.startswith('#')):
            down[(r['caller_module'], int(r['caller_rva'], 16))].append((int(r['samples']), r['callee_module'], int(r['callee_rva'], 16)))
    ct = ('victoria3.exe', int(a.ct, 16))
    kids = sorted(down.get(ct, []), reverse=True)
    tot = sum(s for s, _, _ in kids)
    print(f'CreateTexture 0x{ct[1]:x}: {tot} samples in direct callees (edge samples, all threads)')
    for s, m, r in kids[:12]:
        print(f'  {s:8d} {100.0 * s / max(tot, 1):5.1f}%  {m}!0x{r:x} {sym(m, r)}')
    # Win32 leaves anywhere below VFSOpen / object create (depth-limited walk, max edge per node)
    leaves = collections.Counter()
    seen = set()

    def walk(node, depth):
        if depth > 7 or node in seen:
            return
        seen.add(node)
        for s, m, r in down.get(node, []):
            if m.lower() != 'victoria3.exe':
                leaves[(m, r)] = max(leaves[(m, r)], s)
            else:
                walk((m, r), depth + 1)
    for _, m, r in kids:
        if m.lower() == 'victoria3.exe':
            walk((m, r), 0)
    print('OS/driver calls reached from CreateTexture callees (max edge samples; shared helpers count all their callers):')
    for (m, r), s in leaves.most_common(10):
        print(f'  {s:8d}  {m}!0x{r:x} {sym(m, r)}')


def cmd_log(a):
    spike = re.compile(r'\[(\d\d:\d\d:\d\d\.\d+)\]\[map_cache\] spike (\d+) ms \| texture disk reads in it \(all threads\): (\d+) \((\d+) re-reads\), ([\d.]+) MB, (\d+) ms')
    load = re.compile(r"\[map_cache\] load ([\d.]+) ms ([\d.]+) MB #\d+(?: \(re-read\))? (cache-miss|direct) tid=(\d+) '([^']*)'")
    by_ext = collections.defaultdict(lambda: [0, 0.0, 0.0])
    tids = collections.Counter()
    print(f'{"time":12} {"spike ms":>8} {"reads":>6} {"MB":>7} {"tex ms":>7} {"ms/read":>8} {"tex share":>9}')
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = spike.search(line)
        if m:
            t, gap, n, _, mb, ms = m.groups()
            n, gap, ms = int(n), int(gap), int(ms)
            if n:
                print(f'{t:12} {gap:8d} {n:6d} {float(mb):7.1f} {ms:7d} {ms / n:8.1f} {100.0 * ms / max(gap, 1):8.0f}%')
            continue
        m = load.search(line)
        if m:
            ms, mb, kind, tid, path = m.groups()
            ext = os.path.splitext(path.split('?')[0])[1].lower() or '(none)'
            e = by_ext[ext]
            e[0] += 1; e[1] += float(ms); e[2] += float(mb)
            tids[tid] += 1
    if by_ext:
        print('logged single loads (>= load_log_ms or >= load_log_mb only):')
        for ext, (n, ms, mb) in sorted(by_ext.items(), key=lambda x: -x[1][1]):
            print(f'  {ext:6} n={n:4d} {ms:8.0f} ms {mb:8.1f} MB  {ms / max(mb, 0.01):6.1f} ms/MB')
        print('  threads:', ', '.join(f'{t}x{n}' for t, n in tids.most_common(6)))


def cmd_csv(a):
    agg = collections.defaultdict(lambda: [0, 0, 0.0] + [0.0] * len(STAGES))
    req = collections.Counter()
    with open(a.csv, newline='') as f:
        for r in csv.DictReader(f):
            k = (r['phase'], 'main' if r['main'] == '1' else 'worker', r['ext'])
            v = agg[k]
            v[0] += 1; v[1] += int(r['bytes']); v[2] += float(r['total_ms'])
            for i, s in enumerate(STAGES):
                v[3 + i] += float(r[s + '_ms'])
            if r['main'] == '1':
                req[r['req1'] if r['via_cache'] == '1' else r['req0']] += 1
    print(f'{"phase":14} {"thread":6} {"ext":5} {"n":>6} {"MB":>8} {"total":>8} ' + ' '.join(f'{s:>7}' for s in STAGES))
    for k in sorted(agg):
        v = agg[k]
        print(f'{k[0]:14.14} {k[1]:6} {k[2]:5} {v[0]:6d} {v[1] / 1048576:8.1f} {v[2]:8.0f} ' + ' '.join(f'{x:7.0f}' for x in v[3:]))
    print('main-thread requesters (CreateTexture caller, or the caller of GetOrLoadTexture on cache misses):')
    for r, n in req.most_common(a.top):
        print(f'  {n:6d}  {r}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    e = sp.add_parser('edges'); e.add_argument('edges'); e.add_argument('--ct', default='0x3C5AF20')
    l = sp.add_parser('log'); l.add_argument('log')
    c = sp.add_parser('csv'); c.add_argument('csv'); c.add_argument('--top', type=int, default=15)
    a = ap.parse_args()
    {'edges': cmd_edges, 'log': cmd_log, 'csv': cmd_csv}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
