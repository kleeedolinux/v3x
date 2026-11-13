"""Count Victoria 3 error.log lines per source location and per message template.
Usage: re_log_stats.py [--top N] [--ini log_filter.ini] LOG [LOG...]
With --ini, also simulates the log_filter rules (drop/keep + dedupe) and prints the reduction.
Template = message with quoted strings, numbers and identifiers after keywords collapsed."""
import argparse, collections, fnmatch, re, sys

LINE = re.compile(r'^\[(\d\d:\d\d:\d\d)\]\[([^\]:]+):(\d+)\]: ?(.*)$')

def template(msg):
    t = re.sub(r"'[^']*'", "'*'", msg)
    t = re.sub(r'"[^"]*"', '"*"', t)
    t = re.sub(r'\b(Event|event|Modifier type definition|key|Key|type|trigger|effect|scope|for|in|on)\s+[\w.:/\-]+', r'\1 *', t)
    t = re.sub(r'\b\d+(\.\d+)?\b', 'N', t)
    t = re.sub(r'near: .*', 'near: *', t)
    t = re.sub(r'(file|File):? [^\s,]+', r'\1 *', t)
    return t[:160]

def parse(paths):
    out = []
    for p in paths:
        with open(p, encoding='utf-8', errors='replace') as f:
            for raw in f:
                raw = raw.rstrip('\n')
                m = LINE.match(raw)
                if m:
                    out.append((m.group(2) + ':' + m.group(3), m.group(4), len(raw) + 1))
                elif out:   # continuation line of a multi-line message
                    loc, msg, n = out[-1]
                    out[-1] = (loc, msg + '\n' + raw, n + len(raw) + 1)
    return out

NOISY = set('0123456789_./' + chr(92) + ':')

def lf_template(msg):
    """Mirror of lf_template() in plugins/log_filter/lf_rules.c."""
    WS = ' \t\r\n'
    out, i, n = [], 0, len(msg)
    while i < n:
        ch = msg[i]
        if ch in WS:
            while i < n and msg[i] in WS: i += 1
            out.append(' ')
        elif ch in '\'"':
            j = msg.find(ch, i + 1)
            i = j + 1 if j != -1 else n
            out.append('Q')
        else:
            j = i
            while j < n and msg[j] not in WS and msg[j] not in '\'"': j += 1
            w = msg[i:j]
            out.append('*' if NOISY & set(w) else w)
            i = j
    return ''.join(out)

PRESETS = {'default': {'dedupe_first': 3, 'dedupe_template': 0},
           'aggressive': {'dedupe_first': 1, 'dedupe_template': 1}}

def load_ini(path, extra=()):
    """Minimal mirror of plugins/log_filter/lf_rules.c parsing (for offline simulation only).
    `extra` lines are applied after the file (e.g. 'preset = aggressive')."""
    rules, opt = [], {'dedupe_first': 3, 'dedupe_template': 0}
    for line in list(open(path, encoding='utf-8')) + list(extra):
        line = line.strip()
        if not line or line[0] in '#;[':
            continue
        k, _, v = line.partition('=')
        k, v = k.strip(), v.strip()
        if k[:4] in ('drop', 'keep'):
            loc, msg = '*', '*'
            if k[4:] == '_loc': loc = v
            elif k[4:] == '_msg': msg = v
            else:
                loc, _, msg = (x.strip() for x in v.partition('::'))
            rules.append((k[:4] == 'keep', loc, msg))
        elif k in ('dedupe_first', 'dedupe_template'):
            opt[k] = int(v)
        elif k == 'preset':
            opt.update(PRESETS[v])
    return rules, opt

def decide(rules, loc, msg):
    """Returns 'keep', 'drop' or None. keep rules beat drop rules ('*' matches newlines too)."""
    drop = None
    for keep, lp, mp in rules:
        if fnmatch.fnmatchcase(loc, lp) and fnmatch.fnmatchcase(msg, mp):
            if keep:
                return 'keep'
            drop = 'drop'
    return drop

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+'); ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--ini')
    ap.add_argument('--set', action='append', default=[], help="extra ini line, e.g. 'preset = aggressive'")
    a = ap.parse_args()
    lines = parse(a.logs)
    total_b = sum(n for _, _, n in lines)
    print(f"{len(lines)} messages, {total_b} bytes in {len(a.logs)} file(s)\n")
    by_loc = collections.Counter(l for l, _, _ in lines)
    by_tpl = collections.Counter((l, template(m.split('\n')[0])) for l, m, _ in lines)
    print(f"== top {a.top} source locations ==")
    for loc, n in by_loc.most_common(a.top):
        print(f"{n:6d} {100*n/len(lines):5.1f}%  {loc}")
    print(f"\n== top {a.top} templates ==")
    for (loc, t), n in by_tpl.most_common(a.top):
        print(f"{n:6d}  {loc:45s} {t}")
    if a.ini:
        rules, opt = load_ini(a.ini, a.set)
        seen = collections.Counter()
        kept = kept_b = dropped = deduped = 0
        for loc, msg, n in lines:
            d = decide(rules, loc, msg)
            if d == 'drop':
                dropped += 1; continue
            if d == 'keep':
                kept += 1; kept_b += n; continue
            key = (loc, lf_template(msg) if opt['dedupe_template'] else msg)
            seen[key] += 1
            if opt['dedupe_first'] and seen[key] > opt['dedupe_first']:
                deduped += 1; continue
            kept += 1; kept_b += n
        print(f"\n== simulation with {a.ini} {a.set} -> {opt} ==")
        print(f"kept {kept}/{len(lines)} messages ({100*kept/len(lines):.1f}%), "
              f"{kept_b}/{total_b} bytes; dropped by rule {dropped}, deduped {deduped}")
