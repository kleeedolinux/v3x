"""Reconstruct Victoria 3 load-phase timelines from debug.log (+ rotated debug.N.log).
Usage: re_load_timeline.py [LOGDIR]   (default: ~/Documents/Paradox Interactive/Victoria 3/logs)

Sessions are split at 'Log system initialized.' or a >10 min time gap. Rotated files are stitched oldest-first
(debug.5.log ... debug.1.log, debug.log). For every session prints each milestone with its
offset from session start (seconds). Milestones come from explicit engine log lines
(verified) or from the first/last line of a log *source file* (inferred phase boundary:
the phase may start before its first warning and end after its last one).
Use it to compare runs before/after a V3X fast_load change: same mods, same machine."""
import os, re, sys, collections

LINE = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\]\[([^\]:]+)(?::(\d+))?\]: ?(.*)')

# (label, regex on "file:line msg") -> first match only
EXPLICIT = [
    ('startup duration line', r'main\.cpp.*Total startup duration'),
    ('FMOD engine created', r'Creating FMOD sound engine'),
    ('mods matched (dlc.cpp)', r'^dlc\.cpp'),
    ('checksum calculation START', r'Starting checksum calculation'),
    ('gfx/models .asset enumerated', r"Done enumerating 'gfx/models'"),
    ('ThreadedInitTextureLookup start', r'ThreadedInitTextureLookup: '),
    ('ThreadedInitTextureLookup done', r'ThreadedInitTextureLookup found'),
    ('Jomini social created', r'Created Jomini Social'),
    ('mesh type load summary (pdx_3dtypes)', r'pdx_3dtypes\.cpp.*Total time'),
    ('Transition Empty->Game', r'Transition Empty->Game took'),
    ('portrait palettes init', r'Initializing portrait color palettes'),
    ('first game-script effect log (game running)', r'00_code_on_actions'),
]
# source files whose first/last line bound a phase (inferred)
SPANS = [
    ('localization parse', ('pdx_localize.cpp',)),
    ('GUI build (gui files)', ('pdx_gui_localize.cpp', 'pdx_data_statementparser.cpp')),
    ('script database parse', ('lexer.cpp', 'jomini_trigger.cpp', 'jomini_effect.cpp', 'gamedatabase.h',
                               'jomini_scriptvalue.h', 'jomini_eventmanager.cpp')),
    ('frontend data bindings', ('pdx_data_localize.cpp',)),
]


def files(d):
    rot = sorted((f for f in os.listdir(d) if re.fullmatch(r'debug\.\d+\.log', f)),
                 key=lambda f: -int(f.split('.')[1]))
    return [os.path.join(d, f) for f in rot + (['debug.log'] if os.path.exists(os.path.join(d, 'debug.log')) else [])]


def sessions(paths):
    cur = None; out = []; day = 0; last = None
    for p in paths:
        for raw in open(p, encoding='utf-8', errors='replace'):
            m = LINE.match(raw)
            if not m: continue
            t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3])
            if last is not None and t < last - 43200: day += 86400
            last = t; t += day
            src, ln, msg = m[4], m[5], m[6]
            new = cur is None or t - cur['lines'][-1][0] > 600 or t < cur['lines'][-1][0]
            if 'Log system initialized' in msg and cur is not None and t - cur['start'] > 5:
                new = True
            if new:
                cur = {'start': t, 'lines': [], 'hhmmss': f"{m[1]}:{m[2]}:{m[3]}"}; out.append(cur)
            cur['lines'].append((t, src, ln, msg))
    return out


def report(s):
    t0 = s['start']; L = s['lines']
    print(f"=== session {s['hhmmss']}  ({len(L)} lines, last +{L[-1][0]-t0}s)")
    rows = []
    for label, rx in EXPLICIT:
        r = re.compile(rx)
        hit = next(((t, msg) for t, src, ln, msg in L if r.search(f"{src}:{ln} {msg}")), None)
        if hit: rows.append((hit[0] - t0, label, hit[1][:70]))
    for label, srcs in SPANS:
        ts = [t for t, src, ln, msg in L if src in srcs]
        if ts: rows.append((ts[0] - t0, f"{label} (inferred span)", f"+{ts[0]-t0}s .. +{ts[-1]-t0}s, {len(ts)} lines"))
    mesh = [(msg) for t, src, ln, msg in L if src == 'pdx_3dtypes.cpp']
    if mesh:
        ms = [int(x) for x in (re.search(r'in (\d+) ms', m)[1] for m in mesh if re.search(r'in (\d+) ms', m))]
        rows.append((None, 'mesh types timed', f"{len(ms)} meshes, sum {sum(ms)/1000:.1f}s, max {max(ms)} ms"))
    for off, label, info in sorted(rows, key=lambda r: (r[0] is None, r[0] or 0)):
        print(f"  {'+%4ds' % off if off is not None else '      '}  {label:45s} {info}")


if __name__ == '__main__':
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(r'~/Documents/Paradox Interactive/Victoria 3/logs')
    for s in sessions(files(d)):
        report(s)
