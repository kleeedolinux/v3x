"""Playset conflict report for Victoria 3 mods (offline, read-only). See docs/re/modloading.md.

  python tools/re_modload_conflicts.py [--docs <Documents/Paradox Interactive/Victoria 3>] [--lang english]
                                       [--out report.txt] [--max 40]

Sections:
  playset     mods in content_load.json order (last = highest priority), metadata, version, replace_paths
  overrides   relative paths provided by more than one root (mod-vs-mod first) and which root wins
              (the engine prepends every mount, so the LAST mod in the playset is searched first)
  dup_keys    top-level keys defined in two different effective files of the same common/ folder
              (effective = winning copy of each path, after replace_path). The engine creates the FIRST
              one in file-name order and logs "Duplicated key X will not be created" for the rest
              (gamedatabase.h:394). INJECT:/REPLACE:/TRY_* prefixed keys are listed as patches, not conflicts.
  loc_keys    localization keys of --lang defined in more than one effective .yml (files under
              localization/<lang>/replace/ win; otherwise reported as ambiguous)
  encoding    BOM / encoding problems: UTF-16 files, invalid UTF-8, localization .yml without a UTF-8
              BOM or with a bad l_<lang>: header / file name, unbalanced braces in script files
  errors      previous run's logs/error.log grouped by the mod that provides the referenced file
  db_log      logs/database_conflicts.log (raw, if not empty)

Heuristics (marked so in the output): file-name load order (case-insensitive ASCII sort per folder),
replace_path = exact folder (sub-folders still load), folders where duplicate top-level keys are
normal (defines, on_actions, history, ...) are skipped.
"""
import argparse, collections, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_modload_vfs import DEF_DOCS, GAME, build_roots, list_files  # noqa: E402

SCRIPT_EXT = ('.txt',)
# top-level folders the game reads; mod files elsewhere (.metadata, .git, steamcmd, thumbnails) are ignored
GAME_DIRS = {'common', 'events', 'gui', 'gfx', 'localization', 'map_data', 'fonts', 'music', 'sound',
             'interface', 'data_binding', 'input_profile', 'dlc_metadata', 'notifications', 'content_source',
             'tools', 'dlc'}
TEXT_EXT = ('.txt', '.yml', '.gui', '.gfx', '.asset', '.settings', '.shader', '.fxh', '.json', '.csv')
# folders whose top-level keys legitimately repeat or merge (defines blocks, on_action appends, history)
DUP_SKIP = ('common/defines', 'common/on_actions', 'common/history', 'common/scripted_lists',
            'common/script_values', 'common/coat_of_arms/template_lists')
PATCH_PREFIX = re.compile(r'^(INJECT|REPLACE|TRY_INJECT|TRY_REPLACE|REPLACE_OR_CREATE|INJECT_OR_CREATE):')


def read_meta(path):
    m = {'name': os.path.basename(path), 'version': '', 'supported_game_version': '', 'replace_paths': [],
         'error': None}
    p = os.path.join(path, '.metadata', 'metadata.json')
    try:
        with open(p, encoding='utf-8-sig') as f:
            j = json.load(f)
        m.update({k: j.get(k, m[k]) for k in ('name', 'version', 'supported_game_version')})
        gcd = j.get('game_custom_data')
        if gcd is None:
            m['error'] = 'no game_custom_data member (engine logs "Mod metadata read error")'
        else:
            m['replace_paths'] = [x.replace('\\', '/').strip('/').lower() for x in gcd.get('replace_paths', [])]
    except FileNotFoundError:
        m['error'] = 'no .metadata/metadata.json'
    except Exception as e:  # noqa: BLE001
        m['error'] = 'metadata.json: %s' % e
    return m


def lex_top_keys(text):
    """Top-level `key =` / `key ?=` names of a Paradox script text (comments, strings, braces aware).
    Returns (keys, brace_balance)."""
    keys, depth, i, n = [], 0, 0, len(text)
    prev = None
    tok_re = re.compile(r'"(?:[^"\\\n]|\\.)*"|[{}]|[<>!?]?=|[^\s{}=<>!?#"]+')
    while i < n:
        c = text[i]
        if c == '#':
            j = text.find('\n', i)
            i = n if j < 0 else j
            continue
        if c.isspace():
            i += 1
            continue
        m = tok_re.match(text, i)
        if not m:
            i += 1
            continue
        t = m.group()
        i = m.end()
        if t == '{':
            depth += 1
        elif t == '}':
            depth -= 1
        elif t in ('=', '?=') and depth == 0 and prev and prev not in ('{', '}', '=', '?='):
            keys.append(prev)
        prev = t
    return keys, depth


def decode(raw):
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return None, 'UTF-16 BOM (engine expects UTF-8)'
    try:
        return raw.decode('utf-8-sig'), None
    except UnicodeDecodeError as e:
        return raw.decode('utf-8', 'replace'), 'invalid UTF-8 at byte %d (probably ANSI/GBK)' % e.start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--docs', default=DEF_DOCS)
    ap.add_argument('--lang', default='english')
    ap.add_argument('--out')
    ap.add_argument('--errlog', default='error.log', help='file in logs/ to attribute (error.1.log = run before)')
    ap.add_argument('--max', type=int, default=40, help='max lines per list')
    a = ap.parse_args()
    out = open(a.out, 'w', encoding='utf-8') if a.out else sys.stdout
    W = lambda s='': print(s, file=out)  # noqa: E731

    skipped = []
    roots = build_roots(a.docs, skipped)      # lowest priority first
    files = [{f for f in list_files(r[2]) if f.split('/', 1)[0] in GAME_DIRS} if os.path.isdir(r[2]) else set()
             for r in roots]
    meta = {i: read_meta(r[2]) for i, r in enumerate(roots) if r[0] == 'mod'}
    label = lambda i: ('%s "%s"' % (roots[i][1], meta[i]['name'])) if i in meta else roots[i][1]  # noqa: E731

    W('# V3X mod conflict report (tools/re_modload_conflicts.py) - %d roots, %d mods' % (len(roots), len(meta)))
    W('\n## playset (load order; LAST = highest priority = wins file conflicts)')
    for i in meta:
        m = meta[i]
        W('  %2d %-12s %-45s v%-8s game %-8s%s%s' % (
            i, roots[i][1], m['name'][:45], m['version'][:8], m['supported_game_version'] or '-',
            ('  replace_paths=' + ','.join(m['replace_paths'])) if m['replace_paths'] else '',
            ('  !! ' + m['error']) if m['error'] else ''))
    for p in skipped:
        m = read_meta(p)
        W('  !! NOT MOUNTED by the engine last run (absent from the debug.log "Mod:" list): %s "%s"%s' % (
            os.path.basename(p), m['name'], ('  - ' + m['error']) if m['error'] else ''))
    names = collections.Counter(meta[i]['name'] for i in meta)
    for n, c in names.items():
        if c > 1:
            W('  !! "%s" is enabled %d times (different workshop ids) - the later one overrides the earlier' % (n, c))

    # effective winner per path, with replace_path hiding lower roots' files in that exact folder
    owner, providers, hidden = {}, collections.defaultdict(list), collections.Counter()
    for i in range(len(roots) - 1, -1, -1):
        rp = set(meta[i]['replace_paths']) if i in meta else set()
        for f in files[i]:
            providers[f].append(i)
        for f in files[i]:
            if f not in owner:
                owner[f] = i
        if rp:
            for j in range(i - 1, -1, -1):
                for f in list(files[j]):
                    if os.path.dirname(f) in rp and owner.get(f) is None:
                        owner[f] = -1          # hidden by replace_path
                        hidden[i] += 1
    W('\n## overrides (same path in several roots; first listed = winner)')
    mm = sorted((f for f, p in providers.items() if sum(1 for x in p if x in meta) >= 2), key=lambda f: f)
    mv = sorted(f for f, p in providers.items() if len(p) >= 2 and p[0] in meta and
                any(roots[x][0] != 'mod' for x in p) and f not in mm)
    W('  mod-vs-mod: %d paths (one mod silently replaces another mod\'s whole file)' % len(mm))
    for f in mm[:a.max]:
        W('    %s: %s' % (f, ' > '.join(label(x) for x in providers[f])))
    if len(mm) > a.max:
        W('    ... %d more' % (len(mm) - a.max))
    per = collections.Counter(providers[f][0] for f in mv)
    W('  mod-over-vanilla/dlc: %d paths (whole-file replaces; these break first after a game patch)' % len(mv))
    for i, c in per.most_common():
        W('    %-60s %5d files' % (label(i)[:60], c))
    for i, c in hidden.items():
        W('  replace_path of %s hides %d lower files' % (label(i), c))

    # duplicate database keys among effective script files
    W('\n## dup_keys (top-level keys in two effective files of one common/ folder; heuristic)')
    eff = {f: i for f, i in owner.items() if i >= 0}
    byfolder = collections.defaultdict(list)
    enc_issues, keyfile = [], {}
    for f, i in eff.items():
        if f.startswith('common/') and f.endswith(SCRIPT_EXT):
            byfolder[os.path.dirname(f)].append(f)
    ndup, npatch, lines = 0, 0, []
    for folder in sorted(byfolder):
        if folder.startswith(DUP_SKIP):
            continue
        seen = {}
        for f in sorted(byfolder[folder], key=lambda x: os.path.basename(x)):
            i = eff[f]
            try:
                raw = open(os.path.join(roots[i][2], f), 'rb').read()
            except OSError:
                continue
            text, err = decode(raw)
            if err and i in meta:
                enc_issues.append((i, f, err))
            if text is None:
                continue
            keys, bal = lex_top_keys(text)
            if bal != 0 and i in meta:
                enc_issues.append((i, f, 'unbalanced braces (%+d) - the rest of the file may be dropped' % bal))
            for k in keys:
                if k.startswith('@') or k == 'namespace':
                    continue
                if PATCH_PREFIX.match(k):
                    npatch += 1
                    continue
                if k in seen and seen[k][0] != f:
                    ndup += 1
                    if eff[f] in meta or seen[k][1] in meta:
                        lines.append('    %s: "%s" in %s [%s] (created) and %s [%s] (dropped)' % (
                            folder, k, seen[k][0], label(seen[k][1]), f, label(i)))
                else:
                    seen.setdefault(k, (f, i))
    W('  %d duplicates (%d involve a mod), %d INJECT/REPLACE patches' % (ndup, len(lines), npatch))
    for ln in lines[:a.max]:
        W(ln)
    if len(lines) > a.max:
        W('    ... %d more' % (len(lines) - a.max))

    # localization
    W('\n## loc_keys (%s; files under replace/ win)' % a.lang)
    locdir = 'localization/%s/' % a.lang
    lockeys = collections.defaultdict(list)
    for f, i in sorted(eff.items()):
        if not f.endswith('.yml') or not f.startswith('localization/'):
            continue
        try:
            raw = open(os.path.join(roots[i][2], f), 'rb').read()
        except OSError:
            continue
        if i in meta:
            if raw[:3] != b'\xef\xbb\xbf':
                enc_issues.append((i, f, 'localization .yml without UTF-8 BOM (engine ignores/garbles it)'))
            base = os.path.basename(f)
            mlang = re.search(r'_l_([a-z_]+)\.yml$', base)
            if not mlang:
                enc_issues.append((i, f, 'localization file name does not end in _l_<language>.yml'))
        text, err = decode(raw)
        if err and i in meta:
            enc_issues.append((i, f, err))
        if not f.startswith(locdir) or text is None:
            continue
        hdr = None
        for ln in text.splitlines():
            s = ln.strip()
            if not s or s.startswith('#'):
                continue
            if hdr is None:
                hdr = s
                if i in meta and not s.startswith('l_%s:' % a.lang):
                    enc_issues.append((i, f, 'first line is not "l_%s:"' % a.lang))
                continue
            mk = re.match(r'([A-Za-z0-9_.\-\']+):\d*\s', s + ' ')
            if mk:
                lockeys[mk.group(1)].append((f, i))
    dl = []
    for k, v in lockeys.items():
        fs = {f for f, _ in v}
        if len(fs) < 2 or not any(i in meta for _, i in v):
            continue
        rep = [x for x in v if '/replace/' in x[0]]
        dl.append('    %s: %s%s' % (k, ', '.join('%s [%s]' % (f, label(i)) for f, i in v),
                                     (' -> replace/ wins' if rep else ' -> ambiguous')))
    W('  %d keys defined in several files with a mod involved' % len(dl))
    for ln in sorted(dl)[:a.max]:
        W(ln)
    if len(dl) > a.max:
        W('    ... %d more' % (len(dl) - a.max))

    # other mod text files: encoding only
    for f, i in eff.items():
        if i in meta and f.endswith(TEXT_EXT) and not f.startswith(('common/', 'localization/')):
            try:
                raw = open(os.path.join(roots[i][2], f), 'rb').read()
            except OSError:
                continue
            _, err = decode(raw)
            if err:
                enc_issues.append((i, f, err))
    W('\n## encoding (%d issues)' % len(enc_issues))
    for i, f, e in sorted(enc_issues)[:a.max * 2]:
        W('    %s: %s: %s' % (label(i)[:50], f, e))

    # error.log attribution
    W('\n## errors (previous run, logs/%s, grouped by the root that provides the file)' % a.errlog)
    el = os.path.join(a.docs, 'logs', a.errlog)
    per_mod, samples, other = collections.Counter(), collections.defaultdict(list), 0
    pat = re.compile(r'((?:common|events|gui|gfx|localization|map_data|interface|fonts|sound|music)/[^\s:\'"(),]+)')
    if os.path.exists(el):
        with open(el, encoding='utf-8', errors='replace') as fh:
            for ln in fh:
                m = pat.search(ln)
                if not m:
                    if ln.startswith('['):
                        other += 1
                    continue
                f = m.group(1).lower().rstrip('.')
                i = owner.get(f)
                key = label(i) if i is not None and i >= 0 else 'unknown file'
                per_mod[key] += 1
                if len(samples[key]) < 3:
                    samples[key].append(ln.strip()[:160])
        for k, c in per_mod.most_common(a.max):
            W('  %5d  %s' % (c, k))
            for s in samples[k]:
                W('           %s' % s)
        W('  (%d error lines without a file path)' % other)
    else:
        W('  no error.log')

    W('\n## db_log (logs/database_conflicts.log)')
    dc = os.path.join(a.docs, 'logs', 'database_conflicts.log')
    if os.path.exists(dc) and os.path.getsize(dc):
        with open(dc, encoding='utf-8', errors='replace') as fh:
            ls = fh.read().splitlines()
        W('  %d lines' % len(ls))
        for ln in ls[:a.max]:
            W('    ' + ln)
    else:
        W('  empty or missing (the engine wrote no database conflicts last run)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
