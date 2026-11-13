"""Independent offline cross-check for plugins/loc_fallback (read-only).

Computes, for a language, the English localization keys of the enabled mods (content_load.json) that no
file of that language defines (clausewitz/jomini/game/DLC/content packs/mods; same relative path ->
highest root wins, like the VFS), and prints per-mod counts. Compare with test/loc_fallback_test.c's
REAL lines (the C plugin) - the numbers must match.

Usage: python tools/re_loc_fallback.py [--lang l_braz_por] [--game-root ..] [--docs <Documents/Paradox Interactive/Victoria 3>]
                                       [--packs ../binaries/v3x/content_packs] [--dump N] [--vanilla]
Default language: pdx_settings.json System.language. Also prints the engine's own language verdict
from logs/debug.log (pdx_json_settings "not part of enum" = the game ignored the setting).
"""
import argparse, json, os, re, time

LANGS = ["l_english", "l_braz_por", "l_french", "l_german", "l_polish", "l_russian", "l_spanish",
         "l_simp_chinese", "l_japanese", "l_korean", "l_turkish"]
ENTRY = re.compile(r'^([^\s:"#]+):(\d*)[ \t]*"(.*)"[ \t]*(#.*)?$')
HEADER = re.compile(r'^(l_[^\s:"#]+):[ \t]*(#.*)?$')


def parse(text):
    """yields (lang, key, ver, value) like lf_parse (value ends at the last quote before blanks/# comment)"""
    lang = ""
    if text.startswith("﻿"):
        text = text[1:]
    for raw in text.split("\n"):
        line = raw.strip(" \t\r ").lstrip("﻿")
        if not line or line.startswith("#"):
            continue
        h = HEADER.match(line)
        if h:
            lang = h.group(1)
            continue
        m = re.match(r'^([^\s:"#]+):(\d*)[ \t]*"', line)
        if not m:
            continue
        rest = line[m.end():]
        end = None
        for i in range(len(rest) - 1, -1, -1):
            if rest[i] == '"':
                tail = rest[i + 1:].lstrip(" \t\r")
                if not tail or tail.startswith("#"):
                    end = i
                    break
        if end is None:
            continue
        yield lang, m.group(1), m.group(2), rest[:end]


def name_class(lname):
    best, pos = -1, -1
    for i, l in enumerate(LANGS):
        p = lname.rfind(l)
        if p > pos:
            best, pos = i, p
    return best


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--lang")
    ap.add_argument("--game-root", default=os.path.normpath(os.path.join(here, "..", "..")))
    ap.add_argument("--docs", default=os.path.join(os.path.expanduser("~"), "Documents", "Paradox Interactive", "Victoria 3"))
    ap.add_argument("--packs", default=os.path.normpath(os.path.join(here, "..", "..", "binaries", "v3x", "content_packs")))
    ap.add_argument("--dump", type=int, default=0, help="print the first N filled keys")
    ap.add_argument("--vanilla", action="store_true", help="also_fill_vanilla=1")
    a = ap.parse_args()
    t0 = time.perf_counter()
    settings = os.path.join(a.docs, "pdx_settings.json")
    lang = a.lang
    if not lang:
        try:
            lang = json.load(open(settings, encoding="utf-8-sig")).get("System", {}).get("language", "l_english")
        except Exception:
            lang = "l_english"
    dbg = os.path.join(a.docs, "logs", "debug.log")
    if os.path.exists(dbg):
        bad = [l.strip() for l in open(dbg, encoding="utf-8", errors="replace") if "not part of enum" in l and "language" in l]
        print("engine language verdict (debug.log):", bad[0] if bad else "setting accepted")
    roots = []  # (path, kind, mod index)
    for b in ("clausewitz", "jomini", "game"):
        roots.append((os.path.join(a.game_root, b), "base", -1))
    if os.path.isdir(a.packs):
        for n in sorted(os.listdir(a.packs), key=str.lower):
            if os.path.isdir(os.path.join(a.packs, n)) and n[0] not in "._" and n.lower() != "v3x_loc_fallback":
                roots.append((os.path.join(a.packs, n), "pack", -1))
    dlc = os.path.join(a.game_root, "game", "dlc")
    if os.path.isdir(dlc):
        for n in sorted(os.listdir(dlc), key=str.lower):
            if os.path.isdir(os.path.join(dlc, n)):
                roots.append((os.path.join(dlc, n), "dlc", -1))
    mods = [m["path"] for m in json.load(open(os.path.join(a.docs, "content_load.json"), encoding="utf-8-sig"))["enabledMods"]]
    for i, m in enumerate(mods):
        roots.append((m, "mod", i))
    win = {}
    for ri, (rp, kind, _) in enumerate(roots):
        base = os.path.join(rp, "localization")
        for d, _, fs in os.walk(base):
            for f in fs:
                rel = os.path.relpath(os.path.join(d, f), rp).replace("\\", "/").lower()
                if rel.endswith(".yml"):
                    win[rel] = (ri, os.path.join(d, f))
    target = LANGS.index(lang)
    present, cand = set(), {}
    for rel, (ri, full) in sorted(win.items(), key=lambda x: (x[1][0], x[0])):
        kind = roots[ri][1]
        cls = name_class(rel.rsplit("/", 1)[-1])
        fill = kind in ("mod", "pack") or a.vanilla
        if not (cls < 0 or cls == target or (cls == 0 and fill)):
            continue
        rep = "/replace/" in rel
        for l, k, v, val in parse(open(full, encoding="utf-8", errors="surrogateescape").read()):
            if l == lang:
                present.add(k)
            elif l == "l_english" and fill:
                o = cand.get(k)
                if o is None or rep > o[2] or (rep == o[2] and ri > o[1]):
                    cand[k] = (val, ri, rep, v)
    missing = [(k, c) for k, c in cand.items() if k not in present]
    per = {}
    for k, (val, ri, _, _) in missing:
        per[ri] = per.get(ri, 0) + 1
    print(f"language {lang}: {len(missing)} key(s) to fill, {len(present)} present, {len(cand)} English candidates, "
          f"{len(win)} files, {(time.perf_counter() - t0) * 1000:.0f} ms")
    for ri, n in sorted(per.items()):
        print(f"  {n:5d}  {roots[ri][0]}")
    for k, (val, ri, _, v) in missing[:a.dump]:
        print(f"   {k}:{v} \"{val}\"")


if __name__ == "__main__":
    main()
