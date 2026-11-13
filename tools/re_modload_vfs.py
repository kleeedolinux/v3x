"""Mod loading cost model for Victoria 3 (PhysFS search path), offline. See docs/re/modloading.md.

  python tools/re_modload_vfs.py [--exe victoria3.exe] [--docs <Documents/Paradox Interactive/Victoria 3>]
                                 [--profile profile_Empty_to_Game_1.csv ...] [--bench 300]

1. static: resolves the PhysFS DIR archiver table (the struct whose description is
   "Non-archive, direct filesystem I/O") and prints its 9 function slots - the same anchor
   plugins/mod_loader uses. Exit 1 if it does not resolve.
2. roots: rebuilds the search path the engine builds for the current playset
   (content_load.json; base roots clausewitz < jomini < game, then every game/dlc/* folder, then the
   mods in playset order; every mount PREPENDS so the last mod is searched first) and counts, for
   every file of every root, how many roots are probed in vain before the winner.
3. bench (--bench N): wall time of a failed GetFileAttributesExW / CreateFileW on a mod root vs. a
   successful one on game/, i.e. the cost of one miss in PHYSFS getRealDir/openRead.
4. profile: inclusive samples of the PhysFS lookup functions in a devtools profile CSV.
"""
import argparse, ctypes, json, os, re, struct, sys, time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.normpath(os.path.join(HERE, '..', '..'))
DEF_EXE = os.path.join(GAME, 'binaries', 'victoria3.exe')
DEF_DOCS = os.path.join(os.path.expanduser('~'), 'Documents', 'Paradox Interactive', 'Victoria 3')

# PhysFS 3.x PHYSFS_Archiver: version, info{ext,desc,author,url,symlinks}, then 9 fns
SLOTS = ['openArchive', 'enumerate', 'openRead', 'openWrite', 'openAppend', 'remove', 'mkdir', 'stat',
         'closeArchive']
DESC = b'Non-archive, direct filesystem I/O'
AUTHOR = b'Ryan C. Gordon <icculus@icculus.org>'


def resolve_dir_archiver(exe):
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
    text = secs['.text']
    t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
    rdata = secs['.rdata']
    rd = rdata.get_data()
    m = re.search(re.escape(DESC) + b'\0', rd)
    if not m:
        return None, 'description string not found'
    desc_rva = rdata.VirtualAddress + m.start()
    needle = struct.pack('<Q', base + desc_rva)
    hits = [i for i in range(0, len(rd) - 8) if rd[i:i + 8] == needle] if rd.count(needle) else []
    if len(hits) != 1:
        return None, 'expected 1 pointer to the description, found %d' % len(hits)
    s = hits[0] - 0x10          # struct start: version (u32 + pad), extension ptr, description ptr
    ver = struct.unpack_from('<I', rd, s)[0]
    q = [struct.unpack_from('<Q', rd, s + 8 + 8 * k)[0] for k in range(5 + 9)]
    def cstr(va):
        try:
            return pe.get_string_at_rva(va - base, 64)
        except Exception:
            return None
    if ver != 0 or cstr(q[0]) != b'' or cstr(q[2]) != AUTHOR or q[4] != 1:
        return None, 'archiver struct shape mismatch (version %d)' % ver
    fns = [v - base for v in q[5:]]
    if not all(t0 <= f < t1 for f in fns):
        return None, 'archiver slot outside .text'
    return {'struct_rva': rdata.VirtualAddress + s, 'slots': dict(zip(SLOTS, fns))}, None


def list_files(root):
    out = set()
    rl = len(root.rstrip('\\/')) + 1
    for dp, dn, fn in os.walk(root):
        for f in fn:
            out.add(os.path.join(dp, f)[rl:].replace('\\', '/').lower())
    return out


def engine_mods(docs):
    """Mod paths the engine really mounted last run: the "Mod:" block of logs/debug.log
    ("name|path" lines). None if the log has no such block."""
    p = os.path.join(docs, 'logs', 'debug.log')
    if not os.path.exists(p):
        return None
    out, inblk = None, False
    with open(p, encoding='utf-8', errors='replace') as f:
        for ln in f:
            ln = ln.rstrip('\r\n')
            if ln == 'Mod:':
                out, inblk = [], True
                continue
            if inblk:
                if not ln or re.match(r'\[\d\d:\d\d:\d\d\]', ln):
                    inblk = False
                    continue
                if '|' in ln:
                    out.append(ln.rsplit('|', 1)[1])
    return out


def norm(p):
    return os.path.normcase(os.path.normpath(p))


def build_roots(docs, skipped=None):
    """Search path, LOWEST priority first (mount order). Mods of the playset that the engine did not
    mount last run (debug.log "Mod:" block, e.g. metadata read error) are left out and appended to
    `skipped` (if given)."""
    roots = [('base', 'clausewitz', os.path.join(GAME, 'clausewitz')),
             ('base', 'jomini', os.path.join(GAME, 'jomini')),
             ('base', 'game', os.path.join(GAME, 'game'))]
    dlc = os.path.join(GAME, 'game', 'dlc')
    for d in sorted(os.listdir(dlc)) if os.path.isdir(dlc) else []:
        if os.path.isdir(os.path.join(dlc, d)):
            roots.append(('dlc', d, os.path.join(dlc, d)))
    cl = os.path.join(docs, 'content_load.json')
    mods = []
    if os.path.exists(cl):
        with open(cl, encoding='utf-8-sig') as f:
            mods = [m['path'] for m in json.load(f).get('enabledMods', [])]
    eng = engine_mods(docs)
    engset = {norm(x) for x in eng} if eng is not None else None
    for p in mods:
        if engset is not None and norm(p) not in engset:
            if skipped is not None:
                skipped.append(p)
            continue
        roots.append(('mod', os.path.basename(p.rstrip('\\/')), p))
    return roots


def model(roots):
    files = [list_files(r[2]) if os.path.isdir(r[2]) else set() for r in roots]
    top = list(range(len(roots)))[::-1]          # search order
    nidx = sum(1 for r in roots if r[0] in ('dlc', 'mod'))
    allf = set().union(*files)
    misses, saved, by_root = 0, 0, {}
    for f in allf:
        k = 0
        for i in top:
            if f in files[i]:
                break
            k += 1
        win = top[k]
        by_root[win] = by_root.get(win, 0) + 1
        misses += k
        saved += sum(1 for i in top[:k] if roots[i][0] in ('dlc', 'mod'))
    return files, allf, misses, saved, by_root, nidx


def bench(roots, files, n):
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.GetFileAttributesExW.argtypes = [wintypes.LPCWSTR, ctypes.c_int, ctypes.c_void_p]
    buf = ctypes.create_string_buffer(64)
    game = [i for i, r in enumerate(roots) if r[1] == 'game'][0]
    sample = sorted(files[game])[:: max(1, len(files[game]) // n)][:n]
    mods = [r for r in roots if r[0] in ('dlc', 'mod')]
    INVALID = wintypes.HANDLE(-1).value

    def run(paths, op):
        t = time.perf_counter()
        for p in paths:
            if op == 'stat':
                k32.GetFileAttributesExW(p, 0, buf)
            else:
                h = k32.CreateFileW(p, 0x80000000, 1, None, 3, 0, None)
                if h != INVALID and h:
                    k32.CloseHandle(h)
        return (time.perf_counter() - t) / max(1, len(paths)) * 1e6
    miss = [os.path.join(r[2], p).replace('/', '\\') for p in sample for r in mods[:8]]
    hit = [os.path.join(roots[game][2], p).replace('/', '\\') for p in sample]
    t0 = time.perf_counter(); [None for _ in miss]; ov = (time.perf_counter() - t0) / max(1, len(miss)) * 1e6
    run(hit, 'stat'); run(miss, 'stat')   # warm the FS cache
    r = {'n_miss': len(miss), 'n_hit': len(hit),
         'stat_miss_us': run(miss, 'stat'), 'open_miss_us': run(miss, 'open'),
         'stat_hit_us': run(hit, 'stat'), 'open_hit_us': run(hit, 'open'), 'loop_overhead_us': ov}
    r['open_hit2_us'] = run(hit, 'open')   # second open of the same files (AV scan cache warm)
    return r


PROFILE_FNS = {0x3E1C4F0: 'VFSOpen (pdx vfs)', 0x3DFFD50: 'PHYSFS_getRealDir', 0x3E00AC0: 'PHYSFS_openRead',
               0x3E03E00: 'DIR_stat', 0x3E034D0: 'platformStat (GetFileAttributesExW)',
               0x3E03BC0: 'DIR_openRead', 0x3E031E0: 'platformOpen (CreateFileW)', 0x3E00360: 'PHYSFS_enumerate'}


def profile(path):
    rows, total = {}, 0
    with open(path) as f:
        next(f)
        for line in f:
            p = line.strip().split(',')
            if len(p) < 4:
                continue
            total += int(p[1])
            if p[3] == 'victoria3.exe':
                rows[int(p[0], 16)] = (int(p[1]), int(p[2]))
    return total, {PROFILE_FNS[r]: rows.get(r, (0, 0)) for r in PROFILE_FNS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=DEF_EXE)
    ap.add_argument('--docs', default=DEF_DOCS)
    ap.add_argument('--profile', nargs='*', default=[])
    ap.add_argument('--bench', type=int, default=0)
    a = ap.parse_args()
    rc = 0
    st, err = resolve_dir_archiver(a.exe)
    if st:
        print('DIR archiver struct rva %x: %s' % (st['struct_rva'],
              ' '.join('%s=%x' % kv for kv in st['slots'].items())))
    else:
        print('DIR archiver NOT resolved: %s' % err); rc = 1
    skipped = []
    roots = build_roots(a.docs, skipped)
    for p in skipped:
        print('playset mod NOT mounted by the engine last run (debug.log): %s' % p)
    files, allf, misses, saved, by_root, nidx = model(roots)
    print('\nsearch path (%d roots, top = searched first; %d dlc/mod roots are indexable):' % (len(roots), nidx))
    for i in range(len(roots) - 1, -1, -1):
        r = roots[i]
        print('  %2d %-4s %-38s files %6d  wins %6d' % (len(roots) - 1 - i, r[0], r[1][:38], len(files[i]),
                                                       by_root.get(i, 0)))
    print('\nunique paths %d; one lookup of each = %d probes, %d of them misses (%.1f per lookup), '
          '%d misses on dlc/mod roots (skippable by an index)' % (len(allf), len(allf) + misses, misses,
                                                                 misses / max(1, len(allf)), saved))
    if a.bench:
        b = bench(roots, files, a.bench)
        print('\nbench (warm cache, ctypes; loop overhead %.2f us): stat miss %.1f us, open miss %.1f us, '
              'stat hit %.1f us, open+close hit %.1f us (again: %.1f us)  (%d miss / %d hit samples)' % (
                  b['loop_overhead_us'], b['stat_miss_us'], b['open_miss_us'], b['stat_hit_us'],
                  b['open_hit_us'], b['open_hit2_us'], b['n_miss'], b['n_hit']))
        per = b['stat_miss_us'] + b['open_miss_us']
        print('  VFSOpen does getRealDir (stat per root) THEN openRead (CreateFileW per root): one miss pair '
              '= %.1f us; skippable pairs if every path is opened once: %d -> %.2f s single-thread' % (
                  per, saved, saved * per / 1e6))
    for p in a.profile:
        total, rows = profile(p)
        print('\nprofile %s (total %d samples):' % (p, total))
        for n, (ex, inc) in rows.items():
            print('  %-38s excl %7d incl %7d (%.1f%%)' % (n, ex, inc, 100.0 * inc / max(1, total)))
    return rc


if __name__ == '__main__':
    sys.exit(main())
