"""par_load: static count of the blend-shape file reads that the .asset parse makes (Victoria 3 1.15.1).

Usage: re_parload_blend.py [--game <game dir>] [--content-load <content_load.json>] [--top 15]

Overlay: game/ < game/dlc/* < mods in playset order (a later root wins a same relative path).
Only the winning copy of each .asset file is parsed, as the engine does. In every `pdxmesh = { ... }` block:
  file = "x.mesh"                                    base mesh
  blend_shape = { id = "..." type = "y.mesh" ... }   blend-shape file (relative to the .asset dir
                                                     unless it contains '/')
Engine behaviour this mirrors (docs/re/par_load.md, 0x3ab67c0):
  - a blend shape whose key is already in the blend-shape database is not read again
    (key = blend-shape id here: inferred);
  - the base mesh is re-read only when it differs from the previous call's base mesh (one-entry cache).
Prints references, distinct files, the reads the engine would do, the file-level duplicate reads that remain,
and the bytes involved.
"""
import argparse, collections, json, os, re

TOK = re.compile(r'"[^"]*"|[{}=]|[^\s{}="#]+|#[^\n]*')


def tokens(text):
    for m in TOK.finditer(text):
        t = m.group(0)
        if t.startswith('#'):
            continue
        yield t[1:-1] if t.startswith('"') else t


def parse(text):
    """-> list of (key, value) with value = str or list (nested block)."""
    it = tokens(text)
    def block():
        out = []
        for t in it:
            if t == '}':
                return out
            nxt = next(it, None)
            if nxt == '=':
                v = next(it, None)
                out.append((t, block() if v == '{' else v))
            elif nxt == '{':
                out.append((t, block()))
            else:
                out.append((t, None))
                if nxt == '}':
                    return out
        return out
    return block()


def roots(game, cl):
    rs = [os.path.join(game, 'game')]
    dd = os.path.join(game, 'game', 'dlc')
    if os.path.isdir(dd):
        rs += [os.path.join(dd, d) for d in sorted(os.listdir(dd))]
    if cl and os.path.exists(cl):
        d = json.load(open(cl, encoding='utf-8-sig'))
        rs += [m['path'] for m in d.get('enabledMods', []) if m.get('path')]
    return rs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    ap.add_argument('--content-load', default=os.path.expanduser('~/Documents/Paradox Interactive/Victoria 3/content_load.json'))
    ap.add_argument('--top', type=int, default=15)
    a = ap.parse_args()
    win = {}                                   # rel path (lower) -> abs path of the winning .asset
    allfiles = {}                              # rel path (lower) -> abs (any file, for sizes)
    for r in roots(os.path.abspath(a.game), a.content_load):
        g = os.path.join(r, 'gfx')
        for dp, _, fs in os.walk(g):
            for f in fs:
                ab = os.path.join(dp, f)
                rel = os.path.relpath(ab, r).replace('\\', '/').lower()
                allfiles[rel] = ab
                if f.lower().endswith('.asset'):
                    win[rel] = ab
    refs = []                                  # (asset rel, base rel, bs id, bs rel)
    meshes = 0
    for rel in sorted(win):
        try:
            text = open(win[rel], encoding='utf-8-sig', errors='replace').read()
        except OSError:
            continue
        d = os.path.dirname(rel)
        for k, v in parse(text):
            if k != 'pdxmesh' or not isinstance(v, list):
                continue
            meshes += 1
            base = next((x for kk, x in v if kk == 'file' and isinstance(x, str)), '')
            base = (base if '/' in base else d + '/' + base).lower()
            for kk, x in v:
                if kk == 'blend_shape' and isinstance(x, list):
                    bid = next((y for k3, y in x if k3 == 'id' and isinstance(y, str)), '')
                    bt = next((y for k3, y in x if k3 == 'type' and isinstance(y, str)), '')
                    bt = (bt if '/' in bt else d + '/' + bt).lower()
                    refs.append((rel, base, bid, bt))
    size = lambda p: os.path.getsize(allfiles[p]) if p in allfiles else 0
    ids = collections.Counter(r[2] for r in refs)
    files = collections.Counter(r[3] for r in refs)
    # engine reads: blend files once per new id; base mesh once per change of base between consecutive calls
    seen_id, reads_bs, prev, reads_base, base_files = set(), [], None, [], collections.Counter()
    for _, base, bid, bt in refs:
        if bid in seen_id:
            continue
        seen_id.add(bid)
        if base != prev:
            reads_base.append(base); base_files[base] += 1; prev = base
        reads_bs.append(bt)
    rb = collections.Counter(reads_bs)
    dup_bs = sum(c - 1 for c in rb.values())
    dup_base = sum(c - 1 for c in base_files.values())
    print(f"roots scanned: {len(roots(os.path.abspath(a.game), a.content_load))}, .asset files: {len(win)}, pdxmesh: {meshes}")
    print(f"blend_shape refs: {len(refs)}; distinct ids {len(ids)}; distinct files {len(files)}; missing files "
          f"{sum(1 for f in files if f not in allfiles)}")
    print(f"engine reads (inferred model): blend {len(reads_bs)} ({len(rb)} distinct files, {dup_bs} duplicate reads), "
          f"base {len(reads_base)} ({len(base_files)} distinct, {dup_base} duplicate reads)")
    print(f"bytes: blend reads {sum(size(p) for p in reads_bs) / 1e6:.1f} MB, base reads {sum(size(p) for p in reads_base) / 1e6:.1f} MB, "
          f"duplicate share {(sum(size(p) * (c - 1) for p, c in rb.items()) + sum(size(p) * (c - 1) for p, c in base_files.items())) / 1e6:.1f} MB")
    print(f"pdxmesh with blend shapes: {len(set(r[1] for r in refs))} base meshes (each is read again later by the mesh pass "
          f"CMeshType::Load unless lazy_mesh defers it)")
    for p, c in rb.most_common(a.top):
        if c > 1:
            print(f"  dup blend file x{c}: {p}")
    for p, c in base_files.most_common(a.top):
        if c > 1:
            print(f"  dup base x{c}: {p}")


if __name__ == '__main__':
    main()
