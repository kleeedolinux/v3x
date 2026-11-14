#!/usr/bin/env python3
"""Generate the V3X diagrams in docs/assets/ from data.

    python tools/svg/gen_assets.py              # writes docs/assets/*.svg
    python tools/svg/gen_assets.py --out DIR    # writes them somewhere else
    python tools/svg/gen_assets.py platform     # only the named diagrams

Layout is computed, not hand-placed: every box is sized from its measured text (a conservative
per-character width table for the fallback fonts), text is wrapped to a maximum width, grids use
equal gaps, and arrows run between box edges. Every <text> carries data-box="<id>" naming the rect
it must stay inside (data-id on that rect, data-parent on nested rects), so tools/svg/check_assets.ts
can verify in a real browser that no text leaves its box and no two texts overlap.

GitHub renders these files as <img>, so no web font ever loads: only system font stacks are used.
No dependencies. The logo and the icon-*.svg files are hand-drawn and not generated here; the banner
embeds the logo's artwork.
"""
from __future__ import annotations

import html
import math
import re
import sys
from dataclasses import dataclass, replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "docs" / "assets"

# ---- brand ------------------------------------------------------------------------------------
NAVY, NAVY_D, NAVY_L = "#1f3550", "#14202e", "#2c4a6e"
TEAL, TEAL_L = "#2f6f73", "#e3eeec"
BRASS, BRASS_L, BRASS_D = "#b98b3e", "#e2c07a", "#7a5a22"
RED, RED_L = "#9b2d20", "#f6e3dc"
CREAM, PAPER, INK = "#f4ecd8", "#fbf6ea", "#14202e"

FONTS = {
    "sans": "'Segoe UI', Helvetica, Arial, sans-serif",
    "mono": "Consolas, Menlo, 'DejaVu Sans Mono', monospace",
    "serif": "Georgia, 'Times New Roman', Times, serif",
}

# ---- text metrics -----------------------------------------------------------------------------
# Helvetica/Arial advance widths (1/1000 em). Segoe UI, Arial, Helvetica and Liberation Sans are
# all within a few percent of these; SAFETY covers the difference.
_W = {c: 556 for c in "0123456789"}
_W.update(zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ",
              [667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778,
               722, 667, 611, 722, 667, 944, 667, 667, 611]))
_W.update(zip("abcdefghijklmnopqrstuvwxyz",
              [556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556,
               333, 500, 278, 556, 500, 722, 500, 500, 500]))
_W.update({" ": 278, "!": 278, '"': 355, "#": 556, "$": 556, "%": 889, "&": 667, "'": 191,
           "(": 333, ")": 333, "*": 389, "+": 584, ",": 278, "-": 333, ".": 278, "/": 278,
           ":": 278, ";": 278, "<": 584, "=": 584, ">": 584, "?": 556, "@": 1015, "[": 278,
           "\\": 278, "]": 278, "^": 469, "_": 556, "`": 333, "{": 334, "|": 260, "}": 334,
           "~": 584, " ": 278, "·": 278, "→": 1000, "–": 556, "—": 1000,
           "“": 333, "”": 333, "’": 222, "…": 1000, "×": 584,
           "✕": 800, "≈": 584})
_WB = dict(_W)  # Helvetica Bold differs mostly in lowercase
_WB.update(zip("abcdefghijklmnopqrstuvwxyz",
               [556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611, 611, 611,
                389, 556, 333, 611, 556, 778, 556, 556, 500]))
_WB.update(zip("ABJKLR", [722, 722, 556, 722, 611, 722]))
SAFETY = {"sans": 1.05, "serif": 1.14}
MONO_EM = 0.61  # Consolas 0.55, Menlo / DejaVu Sans Mono 0.602
ASC, DESC = 1.09, 0.27  # em box used by browsers for text bboxes (Segoe UI is the tallest stack)


@dataclass(frozen=True)
class St:
    size: float = 13
    font: str = "sans"
    bold: bool = False
    fill: str = INK
    italic: bool = False
    ls: float = 0  # letter-spacing

    def but(self, **kw) -> "St":
        return replace(self, **kw)


def text_width(s: str, st: St) -> float:
    if st.font == "mono":
        w = MONO_EM * len(s)
    else:
        table = _WB if st.bold else _W
        w = sum(table.get(c, 650) for c in s) / 1000 * SAFETY[st.font]
    return w * st.size + st.ls * len(s)


Run = tuple  # (text, St)


def parse(s: str, st: St) -> list:
    """Tiny markup: **bold**, `mono`."""
    runs = []
    for part in re.split(r"(\*\*[^*]+\*\*|`[^`]+`)", s):
        if not part:
            continue
        if part.startswith("**"):
            runs.append((part[2:-2], st.but(bold=True)))
        elif part.startswith("`"):
            runs.append((part[1:-1], st.but(font="mono", size=round(st.size * 0.96, 2))))
        else:
            runs.append((part, st))
    return runs


def nb(s: str) -> str:
    """Keep a phrase on one line (no-break spaces)."""
    return s.replace(" ", " ")


def runs_width(runs) -> float:
    return sum(text_width(t, s) for t, s in runs)


def wrap(runs, maxw):
    words = [[]]
    for text, st in runs:
        for i, p in enumerate(text.split(" ")):
            if i > 0:
                words.append([])
            if p:
                words[-1].append((p, st))
    words = [w for w in words if w]
    lines, cur, curw = [], [], 0.0
    for w in words:
        ww = runs_width(w)
        sp = text_width(" ", w[0][1])
        if cur and maxw is not None and curw + sp + ww > maxw + 0.01:
            lines.append(cur)
            cur, curw = [], 0.0
        if cur:
            curw += sp
        cur.append(w)
        curw += ww
    if cur:
        lines.append(cur)
    out = []
    for ln in lines:
        rs = []
        for i, w in enumerate(ln):
            for j, (t, s) in enumerate(w):
                if i > 0 and j == 0:
                    t = " " + t
                if rs and rs[-1][1] == s:
                    rs[-1] = (rs[-1][0] + t, s)
                else:
                    rs.append((t, s))
        out.append(rs)
    return out


class Para:
    """A block of text: explicit '\\n' breaks, then greedy wrapping to maxw."""

    def __init__(self, content, st: St = St(), maxw=None, lh=1.32):
        self.lines = []
        if isinstance(content, str):
            for chunk in content.split("\n"):
                self.lines += wrap(parse(chunk, st), maxw)
        else:
            self.lines = wrap(content, maxw)
        self.size = max(s.size for ln in self.lines for _, s in ln)
        self.lh = lh * self.size
        self.widths = [runs_width(ln) for ln in self.lines]
        self.w = max(self.widths)
        self.h = (ASC + DESC) * self.size + (len(self.lines) - 1) * self.lh


def P(content, size=13, font="sans", bold=False, fill=INK, maxw=None, lh=1.32, italic=False, ls=0):
    return Para(content, St(size, font, bold, fill, italic, ls), maxw, lh)


def f(v: float) -> str:
    return f"{v:.1f}".rstrip("0").rstrip(".")


def esc(s: str) -> str:
    return html.escape(s, quote=False)


# ---- scene ------------------------------------------------------------------------------------
class Box:
    def __init__(self, x, y, w, h, bid, parent, attrs, shadow):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.id, self.parent, self.attrs, self.shadow = bid, parent, attrs, shadow

    cx = property(lambda s: s.x + s.w / 2)
    cy = property(lambda s: s.y + s.h / 2)
    r = property(lambda s: s.x + s.w)
    b = property(lambda s: s.y + s.h)

    def svg(self) -> str:
        out = ""
        if self.shadow:
            out += (f'<rect x="{f(self.x)}" y="{f(self.y + 3)}" width="{f(self.w)}" height="{f(self.h)}" '
                    f'rx="{f(self.attrs["rx"])}" fill="{NAVY_D}" opacity=".14"/>')
        a = " ".join(f'{k}="{f(v) if isinstance(v, (int, float)) else v}"' for k, v in self.attrs.items()
                     if v is not None)
        par = f' data-parent="{self.parent.id}"' if self.parent else ""
        out += (f'<rect data-id="{self.id}"{par} x="{f(self.x)}" y="{f(self.y)}" width="{f(self.w)}" '
                f'height="{f(self.h)}" {a}/>')
        return out


class TextEl:
    def __init__(self, para: Para, x, top, box, anchor):
        self.p, self.x, self.top, self.box, self.anchor = para, x, top, box, anchor

    def svg(self) -> str:
        p = self.p
        db = f' data-box="{self.box.id}"' if self.box else ""
        out = f'<text{db} text-anchor="{self.anchor}" font-size="{f(p.size)}">'
        for i, ln in enumerate(p.lines):
            y = self.top + ASC * p.size + i * p.lh
            for j, (t, s) in enumerate(ln):
                pos = f' x="{f(self.x)}" y="{f(y)}"' if j == 0 else ""
                a = f' font-family="{FONTS[s.font]}" font-size="{f(s.size)}" fill="{s.fill}"'
                if s.bold:
                    a += ' font-weight="bold"'
                if s.italic:
                    a += ' font-style="italic"'
                if s.ls:
                    a += f' letter-spacing="{f(s.ls)}"'
                out += f"<tspan{pos}{a}>{esc(t)}</tspan>"
        return out + "</text>"


class Raw:
    def __init__(self, s):
        self.s = s

    def svg(self):
        return self.s


class Arrow:
    def __init__(self, pts, color, dash, width):
        self.pts, self.color, self.dash, self.width = pts, color, dash, width

    def svg(self):
        d = "M" + " L".join(f"{f(x)},{f(y)}" for x, y in self.pts)
        da = f' stroke-dasharray="{self.dash}"' if self.dash else ""
        mid = self.color.lstrip("#")
        return (f'<path data-arrow="1" d="{d}" fill="none" stroke="{self.color}" stroke-width="{f(self.width)}"'
                f'{da} marker-end="url(#ah{mid})"/>')


class Svg:
    def __init__(self, w, title, desc):
        self.w, self.title, self.desc = w, title, desc
        self.els, self.defs, self.n, self.bg = [], {}, 0, []

    def box(self, x, y, w, h=0.0, fill=PAPER, stroke=NAVY, sw=1.5, rx=8, parent=None, dash=None,
            shadow=False, opacity=None):
        self.n += 1
        b = Box(x, y, w, h, f"b{self.n}", parent,
                {"rx": rx, "fill": fill, "stroke": stroke, "stroke-width": sw if stroke else None,
                 "stroke-dasharray": dash, "fill-opacity": opacity}, shadow)
        self.els.append(b)
        return b

    def text(self, para: Para, x, top, box=None, anchor="start") -> TextEl:
        t = TextEl(para, x, top, box, anchor)
        self.els.append(t)
        return t

    def center(self, para: Para, box: Box, dy=0.0) -> TextEl:
        """Centre a block in a box (the em box, which is what the browser's bbox measures)."""
        return self.text(para, box.cx, box.cy - para.h / 2 + dy, box, "middle")

    def raw(self, s: str):
        self.els.append(Raw(s))

    def marker(self, color):
        mid = color.lstrip("#")
        self.defs[f"ah{mid}"] = (f'<marker id="ah{mid}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
                                 f'markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 Z" '
                                 f'fill="{color}"/></marker>')

    def arrow(self, pts, color=NAVY, dash=None, width=2.0):
        self.marker(color)
        self.els.append(Arrow(pts, color, dash, width))

    def link(self, a: Box, b: Box, color=NAVY, dash=None, gap=4.0, at=None):
        """Straight arrow from box a to box b between their facing edges."""
        if b.x >= a.r - 0.5:  # b right of a
            y = at if at is not None else (max(a.y, b.y) + min(a.b, b.b)) / 2
            self.arrow([(a.r + gap, y), (b.x - gap, y)], color, dash)
        elif a.x >= b.r - 0.5:
            y = at if at is not None else (max(a.y, b.y) + min(a.b, b.b)) / 2
            self.arrow([(a.x - gap, y), (b.r + gap, y)], color, dash)
        elif b.y >= a.b - 0.5:  # b below a
            x = at if at is not None else (max(a.x, b.x) + min(a.r, b.r)) / 2
            self.arrow([(x, a.b + gap), (x, b.y - gap)], color, dash)
        else:
            x = at if at is not None else (max(a.x, b.x) + min(a.r, b.r)) / 2
            self.arrow([(x, a.y - gap), (x, b.b + gap)], color, dash)

    def render(self, h) -> str:
        w = self.w
        defs = "".join(self.defs.values())
        body = "".join(self.bg) + "".join(e.svg() for e in self.els)
        return ('<?xml version="1.0" encoding="UTF-8"?>\n'
                f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {f(w)} {f(h)}" width="{f(w)}" '
                f'height="{f(h)}" role="img" aria-labelledby="t d">\n'
                f'<title id="t">{esc(self.title)}</title><desc id="d">{esc(self.desc)}</desc>\n'
                f'<defs>{defs}</defs>\n{body}\n</svg>\n')


def cols(x0, x1, n, gap):
    w = (x1 - x0 - gap * (n - 1)) / n
    return [(x0 + i * (w + gap), w) for i in range(n)]


def flow(widths, x0, maxw, gap, h, row_gap):
    """Lay chips left to right, wrapping to new rows. Returns positions and total height."""
    pos, x, y = [], x0, 0.0
    for w in widths:
        if x > x0 and x + w > x0 + maxw:
            x, y = x0, y + h + row_gap
        pos.append((x, y))
        x += w + gap
    return pos, y + h


GRAD = ('<linearGradient id="brass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e2c07a"/>'
        '<stop offset=".55" stop-color="#b98b3e"/><stop offset="1" stop-color="#7a5a22"/></linearGradient>')
GRID = ('<pattern id="grid" width="24" height="24" patternUnits="userSpaceOnUse"><path d="M24,0 L0,0 0,24" '
        'fill="none" stroke="#1f3550" stroke-width=".5" opacity=".08"/></pattern>')

X0 = 40  # content margin inside the paper frame


class Paper:
    """The common card: cream paper, navy border, brass hairline, serif title, subtitle."""

    def __init__(self, w, title, desc, heading, sub=None):
        self.s = s = Svg(w, title, desc)
        s.defs["grid"] = GRID
        self.x0, self.x1 = X0, w - X0
        t = P(heading, 24, "serif", True, NAVY)
        s.text(t, X0, 28)
        y = 28 + t.h
        if sub:
            q = P(sub, 13.5, fill=BRASS_D, maxw=self.x1 - X0)
            s.text(q, X0, y + 1)
            y += 1 + q.h
        self.top = y + 22

    def done(self, bottom) -> str:
        s, w = self.s, self.s.w
        h = math.ceil(bottom + 30)
        s.bg = [f'<rect x="4" y="4" width="{w - 8}" height="{h - 8}" rx="14" fill="{CREAM}" stroke="{NAVY}" '
                f'stroke-width="3"/>',
                f'<rect x="4" y="4" width="{w - 8}" height="{h - 8}" rx="14" fill="url(#grid)"/>',
                f'<rect x="14" y="14" width="{w - 28}" height="{h - 28}" rx="8" fill="none" stroke="{BRASS}" '
                f'stroke-width="1.2"/>']
        return s.render(h)


def dry(fn, *a):
    """Run a layout function on a throwaway scene to learn its height."""
    return fn(Svg(1, "", ""), *a)


def bullet(s: Svg, x, top, size, color=BRASS):
    cy = top + (ASC - 0.36) * size
    s.raw(f'<rect x="{f(x)}" y="{f(cy - 3)}" width="6" height="6" rx="1" fill="{color}" '
          f'transform="rotate(45 {f(x + 3)} {f(cy)})"/>')


def num_badge(s: Svg, cx, cy, r, n, fill=NAVY, color=CREAM, parent=None, ring=BRASS):
    b = s.box(cx - r, cy - r, 2 * r, 2 * r, fill=fill, stroke=ring, sw=2, rx=r, parent=parent)
    s.center(P(str(n), round(r * 0.95), "serif", True, color), b, dy=0.03 * r)
    return b


# ---- diagrams ---------------------------------------------------------------------------------
BUNDLED = ["crash_logger", "mem_tuner", "lazy_mesh", "keep_history", "enhance", "mod_loader",
           "frame_pacer", "fast_load"]
N_PLUGINS = 33


def platform() -> str:
    pg = Paper(1200, "The V3X platform",
               "Plugins, bundled with V3X or built by the community, are built with the SDK on top of the "
               "V3X API v6 (find code, change it, run code, events). The loader version.dll (0.6.0) loads "
               "them into the Victoria 3 engine, which stays unchanged on disk.",
               "The V3X platform", "One loader, one stable C API, an SDK, and plugins from anyone")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    pad = 18
    light = dict(fill=PAPER, stroke=NAVY, title=NAVY, cap=BRASS_D, ink=INK, rule=BRASS)
    navy = dict(fill=NAVY, stroke=BRASS_D, title=BRASS_L, cap=BRASS_L, ink=CREAM, rule=BRASS)
    dark = dict(fill=NAVY_D, stroke=BRASS_D, title=BRASS_L, cap=BRASS_L, ink=CREAM, rule=BRASS)

    def plugins(s, x, y, w, band):
        top = y
        for head, color, chips, dashed in [
            (f"bundled with V3X ({N_PLUGINS} plugins)", NAVY, BUNDLED[:7] + [f"+ {N_PLUGINS - 7} more"], False),
            ("built by you and the community", TEAL, ["your_plugin.dll", "a UI tweak", "a sim fix",
                                                      "a new console command"], True),
        ]:
            hp = P(head, 13, bold=True, fill=color)
            s.text(hp, x, y, band)
            y += hp.h + 6
            ps = [P(c, 12.5, "mono", True, color) for c in chips]
            pos, hh = flow([p.w + 22 for p in ps], x, w, 8, 28, 8)
            for p, (cx, cy) in zip(ps, pos):
                b = s.box(cx, y + cy, p.w + 22, 28, fill=TEAL_L if dashed else CREAM, stroke=color, sw=1.3,
                          rx=14, parent=band, dash="4 3" if dashed else None)
                s.center(p, b)
            y += hh + 12
        return y - 12 - top

    def sdk(s, x, y, w, band):
        items = ["`v3x_anchor.h`: fail-closed anchors", "`v3x_hook.h`: safe-hook helpers",
                 "`v3x_mem.h`: fault-proof reads", "`v3x_testkit.h`: offline tests against the real exe",
                 "`build_plugin.py`: build, test, install", "`plugin_template`: start here"]
        cs = cols(x, x + w, 3, 24)
        top = y
        for r in range(2):
            ps = [P(items[r * 3 + c], 13, maxw=cs[c][1] - 16) for c in range(3)]
            for (cx, _), p in zip(cs, ps):
                bullet(s, cx, y, 13)
                s.text(p, cx + 16, y, band)
            y += max(p.h for p in ps) + 6
        return y - 6 - top

    def api(s, x, y, w, band):
        rows = [("find code", "find_string · find_xref · function_start · pattern_scan"),
                ("change it", "hook_safe · hook · unhook · patch"),
                ("run code", "on_frame · run_on_main · register_command · exec_command"),
                ("events", "on_event · emit_event: load_transition, save_begin / save_end, process_exit"),
                ("contain", "manifest caps (v6): a plugin declares what it may do; the loader refuses the rest")]
        kw = max(P(k, 13, bold=True).w for k, _ in rows) + 20
        top = y
        for k, v in rows:
            pk = P(k, 13, bold=True, fill=BRASS_L)
            pv = P(v, 12.5, "mono" if k != "contain" else "sans", fill=CREAM, maxw=w - kw)
            s.text(pk, x, y, band)
            s.text(pv, x + kw, y + (pk.h - pv.h) / 2 if len(pv.lines) == 1 else y, band)
            y += max(pk.h, pv.h) + 3
        return y - 3 - top

    def loader(s, x, y, w, band):
        p = P("Windows loads `binaries/version.dll` with the game because the game asks for version.dll. "
              "It loads every enabled, trusted plugin before the game's own startup code runs.", 13, maxw=w)
        s.text(p, x, y, band)
        return p.h

    def engine(s, x, y, w, band):
        a = P("loading · simulation ticks · saves · GUI · memory · rendering", 13.5, fill=CREAM, maxw=w)
        b = P("plugins change it in memory only, after fail-closed checks", 13, fill=BRASS_L, italic=True, maxw=w)
        s.text(a, x, y, band)
        s.text(b, x, y + a.h + 4, band)
        return a.h + 4 + b.h

    bands = [("Plugins", f"{N_PLUGINS} bundled + yours", light, plugins),
             ("SDK", "`sdk/` + `templates/`", light, sdk),
             ("V3X API v6", "`include/v3x.h`\nplain C, versioned, append-only", navy, api),
             ("Loader", "`binaries/version.dll`\nloader 0.6.0", light, loader),
             ("Victoria 3 engine", "`victoria3.exe`, unchanged on disk", dark, engine)]
    lw = max(max(P(n, 19, "serif", True).w, max(P(c, 12.5).widths)) for n, c, *_ in bands) + 2 * pad
    lw = max(lw, 230)
    cx0, cx1 = x0 + lw + pad, x1 - pad
    links = ["built with", "call", "served by", "loads into"]
    y, prev = pg.top, None
    for i, (name, cap, c, fn) in enumerate(bands):
        pt = P(name, 19, "serif", True, c["title"])
        pc = P(cap, 12.5, fill=c["cap"], maxw=lw - 2 * pad)
        lab_h = pt.h + 4 + pc.h
        con_h = dry(fn, cx0, 0, cx1 - cx0, None)
        inner = max(lab_h, con_h)
        band = s.box(x0, y, x1 - x0, inner + 2 * pad, fill=c["fill"], stroke=c["stroke"], sw=2, rx=10,
                     shadow=True)
        ly = y + pad + (inner - lab_h) / 2
        s.text(pt, x0 + pad, ly, band)
        s.text(pc, x0 + pad, ly + pt.h + 4, band)
        s.raw(f'<line x1="{f(x0 + lw)}" y1="{f(y + 12)}" x2="{f(x0 + lw)}" y2="{f(band.b - 12)}" '
              f'stroke="{c["rule"]}" stroke-width="1" opacity=".6"/>')
        fn(s, cx0, y + pad + (inner - con_h) / 2, cx1 - cx0, band)
        if prev is not None:
            ax = x0 + lw / 2
            s.link(prev, band, at=ax)
            lp = P(links[i - 1], 12, fill=BRASS_D, italic=True)
            s.text(lp, ax + 12, (prev.b + band.y) / 2 - lp.h / 2)
        prev = band
        y = band.b + 34
    return pg.done(prev.b)


def architecture() -> str:
    pg = Paper(1200, "V3X architecture",
               "victoria3.exe loads binaries/version.dll (the V3X proxy, loader 0.6.0), which forwards to "
               "System32 version.dll and boots the V3X core. The core offers loader, code finding, hooks, "
               "frame tick, console and events (API v6) to the plugins, which hook engine functions after "
               "fail-closed anchor checks.",
               "How V3X loads",
               "No game file is changed: Windows loads V3X because the game asks for version.dll")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    # row 1: three files
    files = [("victoria3.exe", "the unmodified game\nimports VERSION.dll", "light"),
             ("binaries/version.dll", "the V3X proxy, found first\n(version.dll is not a KnownDLL)", "navy"),
             ("System32\\version.dll", "the real Windows DLL\ngets all 17 exports forwarded", "light")]
    labels = ["loads", "forwards"]
    gap = max(P(l, 12, italic=True).w for l in labels) + 60
    cs = cols(x0, x1, 3, gap)
    pads = 16
    paras = []
    for (name, sub, kind), (cx, cw) in zip(files, cs):
        dark_ = kind == "navy"
        a = P(name, 17, "mono", True, BRASS_L if dark_ else NAVY, maxw=cw - 2 * pads)
        b = P(sub, 13, fill=CREAM if dark_ else INK, maxw=cw - 2 * pads)
        paras.append((a, b))
    h = max(a.h + 6 + b.h for a, b in paras) + 2 * pads
    y = pg.top
    top_boxes = []
    for ((name, sub, kind), (cx, cw), (a, b)) in zip(files, cs, paras):
        dark_ = kind == "navy"
        bx = s.box(cx, y, cw, h, fill=NAVY if dark_ else PAPER, stroke=BRASS_D if dark_ else NAVY, sw=2,
                   rx=10, shadow=True)
        t = bx.y + (h - (a.h + 6 + b.h)) / 2
        s.text(a, bx.cx, t, bx, "middle")
        s.text(b, bx.cx, t + a.h + 6, bx, "middle")
        top_boxes.append(bx)
    for i, l in enumerate(labels):
        s.link(top_boxes[i], top_boxes[i + 1])
        lp = P(l, 12, fill=BRASS_D, italic=True)
        s.text(lp, (top_boxes[i].r + top_boxes[i + 1].x) / 2, top_boxes[i].cy - lp.h - 4, None, "middle")
    # core
    y = top_boxes[1].b + 46
    core = s.box(x0, y, x1 - x0, 0, fill=PAPER, stroke=NAVY, sw=2, rx=12, shadow=True)
    s.link(top_boxes[1], core, at=top_boxes[1].cx)
    note = P("DllMain hooks only the exe entry point (nothing else under the loader lock); the core starts there",
             12, fill=BRASS_D, italic=True)
    s.text(note, top_boxes[1].cx + 12, (top_boxes[1].b + y) / 2 - note.h / 2)
    pad = 18
    ht = P("V3X core", 21, "serif", True, NAVY)
    s.text(ht, x0 + pad, y + pad - 4, core)
    hc = P("`src/core.c` + `src/safe_hook.c` · the plugin API: `include/v3x.h`, API v6, plain C, versioned and "
           "append-only · loader 0.6.0", 13, maxw=x1 - x0 - 3 * pad - ht.w)
    s.text(hc, x0 + 2 * pad + ht.w, y + pad - 4 + (ht.h - hc.h) / 2 if len(hc.lines) == 1 else y + pad - 4, core)
    cards = [("Loader", ["plugins/*.dll", "trusted plugin list", "[plugins] skip list", "manifests, min_api",
                         "caps (v6)", "game_build_id"]),
             ("Find code", ["find_string", "find_xref", "function_start", "pattern_scan"]),
             ("Hooks", ["hook_safe (v4)", "hook / unhook", "patch"]),
             ("Frame tick", ["on_frame", "run_on_main", "*(PeekMessageW hook, main thread)"]),
             ("Console", ["register_command", "print", "exec_command", "exec_capture", "enum_commands"]),
             ("Events", ["on_event", "emit_event", "load_transition", "save_begin / save_end", "process_exit"])]
    cy = y + pad - 4 + max(ht.h, hc.h) + 14
    cc = cols(x0 + pad, x1 - pad, 6, 12)
    built = []
    for (title, items), (cx, cw) in zip(cards, cc):
        pt = P(title, 15, bold=True, fill=NAVY)
        its = [P(i[1:], 12, fill=INK, italic=True, maxw=cw - 24) if i.startswith("*")
               else P(i, 12.5, "mono", fill=NAVY, maxw=cw - 24) for i in items]
        built.append((pt, its, cx, cw))
    ch = max(pt.h + 10 + sum(p.h + 3 for p in its) for pt, its, _, _ in built) + 24
    for pt, its, cx, cw in built:
        cb = s.box(cx, cy, cw, ch, fill=CREAM, stroke=NAVY_L, sw=1.2, rx=8, parent=core)
        s.raw(f'<rect x="{f(cx)}" y="{f(cy)}" width="{f(cw)}" height="4" rx="2" fill="{BRASS}"/>')
        t = cy + 12
        s.text(pt, cx + 12, t, cb)
        t += pt.h + 10
        for p in its:
            s.text(p, cx + 12, t, cb)
            t += p.h + 3
    core.h = cy + ch + pad - y
    # plugins
    y = core.b + 46
    plug = s.box(x0, y, x1 - x0, 0, fill=PAPER, stroke=NAVY, sw=2, rx=12, shadow=True)
    s.link(core, plug, at=(x0 + x1) / 2)
    lp = P("`v3x_plugin_load(&api)` for every enabled plugin, before the game's CRT starts", 12, fill=BRASS_D,
           italic=True)
    s.text(lp, (x0 + x1) / 2 + 12, (core.b + y) / 2 - lp.h / 2)
    ph = P("Plugins", 19, "serif", True, NAVY)
    s.text(ph, x0 + pad, y + pad - 4, plug)
    chips = [(n, NAVY, False) for n in BUNDLED[:6]] + [(f"+ {N_PLUGINS - 6} more", NAVY, False),
                                                       ("your_plugin.dll", TEAL, True)]
    ps = [P(n, 13, "mono", True, c) for n, c, _ in chips]
    cx0 = x0 + 2 * pad + ph.w
    pos, hh = flow([p.w + 24 for p in ps], cx0, x1 - pad - cx0, 8, 30, 8)
    for p, (n, c, d), (px, py) in zip(ps, chips, pos):
        b = s.box(px, y + pad - 4 + py, p.w + 24, 30, fill=TEAL_L if d else CREAM, stroke=c, sw=1.3, rx=15,
                  parent=plug, dash="4 3" if d else None)
        s.center(p, b)
    cap = P(f"{N_PLUGINS} bundled plugins and community plugins live in `binaries/v3x/plugins/`; each one can be "
            "switched off in `binaries/v3x/v3x.ini`", 12.5, fill=BRASS_D, maxw=x1 - pad - cx0)
    ct = y + pad - 4 + max(hh, ph.h) + 10
    s.text(cap, cx0, ct, plug)
    plug.h = ct + cap.h + pad - y
    # engine
    y = plug.b + 46
    eng = s.box(x0 + 120, y, x1 - x0 - 240, 0, fill=NAVY_D, stroke=BRASS_D, sw=2, rx=12, shadow=True)
    s.link(plug, eng, at=(x0 + x1) / 2)
    lp = P("hook / patch", 12, fill=BRASS_D, italic=True)
    s.text(lp, (x0 + x1) / 2 + 12, (plug.b + y) / 2 - lp.h / 2)
    e1 = P("engine functions inside victoria3.exe", 18, "serif", True, BRASS_L)
    e2 = P("hooked or patched only after a fail-closed anchor check; no match = that feature installs nothing",
           13, fill=CREAM, maxw=eng.w - 2 * pad)
    s.text(e1, eng.cx, y + 14, eng, "middle")
    s.text(e2, eng.cx, y + 14 + e1.h + 4, eng, "middle")
    eng.h = 14 + e1.h + 4 + e2.h + 14
    return pg.done(eng.b)


def compare() -> str:
    pg = Paper(1200, "A script mod compared with a V3X plugin",
               "A script mod is text and data the game runs inside its own rules; it changes content, not the "
               "engine. A V3X plugin is compiled code inside the game's program that can hook and fix engine "
               "functions, read live data, add commands and react to loads and saves. They work together.",
               "Script mods and V3X plugins do different jobs",
               "They work together: V3X does not replace mods, and your playset stays the same.")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    left = [("What it is", "text and data files: events, buildings, laws, GUI, scripts, maps, art"),
            ("Where it runs", "the game reads it and runs it inside its own rules"),
            ("Install", "Steam Workshop or the Paradox launcher"),
            ("Can change", "content: what is in the game"),
            ("Cannot change", "how the engine works: loading speed, memory, crashes, engine bugs, threads")]
    right = [("What it is", "compiled C code (a DLL), loaded by V3X's small loader next to the game"),
             ("Where it runs", "inside the game's program: it can hook and fix engine functions"),
             ("Can do", "read live game data, add console commands, react to loads and saves"),
             ("Proof", f"plugin: `lazy_mesh` · 3D-model loading {nb('37.5 s → **6.7 s**')}\n"
                       f"plugin: `enhance` · debug-mode autosaves {nb('156 MB → **26 MB**')}"),
             ("Rules", "single player; every plugin can be turned off; uninstall = delete version.dll + v3x/")]
    mid = 60
    (lx, cw), (rx, _) = cols(x0, x1, 2, mid)
    pad, lab = 22, 112
    vw = cw - 2 * pad - lab
    rows = []
    for (lk, lv), (rk, rv) in zip(left, right):
        a = (P(lk, 13, bold=True, fill=BRASS_D), P(lv, 13.5, fill=INK, maxw=vw))
        b = (P(rk, 13, bold=True, fill=BRASS_L), P(rv, 13.5, fill=CREAM, maxw=vw))
        rows.append((a, b, max(a[1].h, b[1].h, a[0].h)))
    ha = P("A script mod", 24, "serif", True, NAVY)
    hb = P("A V3X plugin", 24, "serif", True, BRASS_L)
    head_h = max(ha.h, hb.h) + 2 * 16
    rg = 16
    body_h = sum(r[2] for r in rows) + rg * (len(rows) - 1)
    H = head_h + 18 + body_h + 24
    y = pg.top
    ca = s.box(lx, y, cw, H, fill=PAPER, stroke=NAVY, sw=2, rx=12, shadow=True)
    cb = s.box(rx, y, cw, H, fill=NAVY, stroke=BRASS_D, sw=2, rx=12, shadow=True)
    s.raw(f'<line x1="{f(lx + pad)}" y1="{f(y + head_h)}" x2="{f(ca.r - pad)}" y2="{f(y + head_h)}" '
          f'stroke="{BRASS}" stroke-width="1.2"/>')
    s.raw(f'<line x1="{f(rx + pad)}" y1="{f(y + head_h)}" x2="{f(cb.r - pad)}" y2="{f(y + head_h)}" '
          f'stroke="{BRASS}" stroke-width="1.2"/>')
    s.text(ha, ca.cx, y + (head_h - ha.h) / 2, ca, "middle")
    s.text(hb, cb.cx, y + (head_h - hb.h) / 2, cb, "middle")
    t = y + head_h + 18
    for i, (a, b, rh) in enumerate(rows):
        for (k, v), box in ((a, ca), (b, cb)):
            s.text(k, box.x + pad, t + (P("x", 13.5).h - k.h) / 2, box)
            s.text(v, box.x + pad + lab, t, box)
        t += rh
        if i < len(rows) - 1:
            for box, col in ((ca, "#d9cba6"), (cb, NAVY_L)):
                s.raw(f'<line x1="{f(box.x + pad)}" y1="{f(t + rg / 2)}" x2="{f(box.r - pad)}" '
                      f'y2="{f(t + rg / 2)}" stroke="{col}" stroke-width="1"/>')
            t += rg
    r = 24
    plus = s.box((ca.r + cb.x) / 2 - r, y + H / 2 - r, 2 * r, 2 * r, fill="url(#brass)", stroke=BRASS_D, sw=2,
                 rx=r)
    s.defs["brass"] = GRAD
    s.center(P("+", 28, bold=True, fill=NAVY_D), plus, dy=-1)
    return pg.done(y + H)


def anchors() -> str:
    pg = Paper(1200, "How a V3X plugin finds engine code",
               "A string or RTTI anchor, its cross-reference in code, the enclosing function from the unwind "
               "table with a prologue check, then a safe hook. Any mismatch fails closed.",
               "How a plugin finds engine code",
               "Addresses move with every game patch. Strings, RTTI names and code shapes rarely do.")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    steps = [("1 · Anchor", '"Processing Tick: "', "a string in .rdata (or an RTTI class name) that the engine uses",
              "find_string()"),
             ("2 · Cross-reference", "lea reg, [rip+disp32]", "the instruction in .text that loads that string",
              "find_xref()"),
             ("3 · Function start", "48 89 5C 24 ?? ...",
              "the enclosing function from the .pdata unwind table; prologue bytes checked", "function_start()"),
             ("4 · Safe hook", "pre → original → post",
              "the thunk forwards 16 integer slots + xmm0-3 and keeps the return value", "hook_safe()")]
    cc = cols(x0, x1, 4, 46)
    pad = 16
    built = []
    for i, ((t, code, d, fn), (cx, cw)) in enumerate(zip(steps, cc)):
        last = i == 3
        pt = P(t, 17, "serif", True, BRASS_L if last else NAVY, maxw=cw - 2 * pad)
        pk = P(code, 13, "mono", True, BRASS_L, maxw=cw - 2 * pad - 20)
        pd = P(d, 13, fill=CREAM if last else INK, maxw=cw - 2 * pad)
        pf = P(fn, 14, "mono", True, BRASS_L if last else TEAL)
        built.append((pt, pk, pd, pf, cx, cw, last))
    chip_h = max(b[1].h for b in built) + 16
    H = pad + max(b[0].h for b in built) + 12 + chip_h + 12 + max(b[2].h for b in built) + 16 + \
        max(b[3].h for b in built) + pad
    y = pg.top
    cards = []
    for pt, pk, pd, pf, cx, cw, last in built:
        c = s.box(cx, y, cw, H, fill=NAVY if last else PAPER, stroke=BRASS_D if last else NAVY, sw=2, rx=10,
                  shadow=True)
        t = y + pad
        s.text(pt, cx + pad, t, c)
        t += max(b[0].h for b in built) + 12
        chip = s.box(cx + pad, t, cw - 2 * pad, chip_h, fill=NAVY_D, stroke=BRASS_D if last else None, sw=1,
                     rx=6, parent=c)
        s.center(pk, chip)
        t += chip_h + 12
        s.text(pd, cx + pad, t, c)
        s.raw(f'<line x1="{f(cx + pad)}" y1="{f(c.b - pad - pf.h - 8)}" x2="{f(c.r - pad)}" '
              f'y2="{f(c.b - pad - pf.h - 8)}" stroke="{BRASS}" stroke-width="1" opacity=".6"/>')
        s.text(pf, cx + pad, c.b - pad - pf.h, c)
        cards.append(c)
    for a, b in zip(cards, cards[1:]):
        s.link(a, b, at=a.y + pad + max(x[0].h for x in built) + 12 + chip_h / 2)
    # outcomes
    y = cards[0].b + 48
    fail = s.box(cards[0].x, y, cards[2].r - cards[0].x, 0, fill=RED_L, stroke=RED, sw=2, rx=10)
    ok = s.box(cards[3].x, y, cards[3].w, 0, fill=TEAL_L, stroke=TEAL, sw=2, rx=10)
    f1 = P("Not found, not unique, or the bytes differ?", 16, bold=True, fill=RED, maxw=fail.w - 2 * pad)
    f2 = P("Fail closed: install nothing, log `anchor ... FAILED` in v3x.log; that plugin or feature stays "
           "vanilla.", 13.5, maxw=fail.w - 2 * pad)
    o1 = P("All checks pass?", 16, bold=True, fill=TEAL, maxw=ok.w - 2 * pad)
    o2 = P("Hook it. The callbacks see every argument and the return value.", 13.5, maxw=ok.w - 2 * pad)
    hh = max(f1.h + 6 + f2.h, o1.h + 6 + o2.h)
    fail.h = ok.h = hh + 2 * pad
    for box, a, b in ((fail, f1, f2), (ok, o1, o2)):
        t = y + pad + (hh - (a.h + 6 + b.h)) / 2
        s.text(a, box.x + pad, t, box)
        s.text(b, box.x + pad, t + a.h + 6, box)
    for c in cards[:3]:
        s.link(c, fail, color=RED, dash="5 4", at=c.cx)
    s.link(cards[3], ok, color=TEAL, at=cards[3].cx)
    return pg.done(fail.b)


def discovery() -> str:
    pg = Paper(1200, "How an optimization is found",
               "Measure, find, fix, verify, result. Example (plugin: mod_loader): 3.44 million failed file lookups "
               "cost 103.8 s of thread time in one session; a per-folder file index predicts 94.4 percent of them "
               "in game with 0 mismatches.",
               "How an optimization is found",
               "Every fix in the showcase went this way, and every fix is a plugin built on V3X. The tools ship "
               "with V3X.")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    steps = [("Measure", "devtools profiler, crash_logger, plugins in measure mode"),
             ("Find", "trace the hot spot: strings, xrefs, RTTI; notes in docs/re/"),
             ("Fix", "a plugin with fail-closed anchors and hook_safe"),
             ("Verify", "offline test against the real exe; measure mode checks the answers"),
             ("Result", "play with it on, compare the logs, keep it or drop it")]
    ex = ["**3.44 M** failed file lookups, **103.8 s** of thread time",
          "each file is searched for in every mod and DLC folder first",
          "a per-folder file index, built once in the background",
          "**0 mismatches** against the engine in game",
          "the index predicts **94.4 %** of the misses in game"]
    cc = cols(x0, x1, 5, 40)
    pad = 16
    r = 15
    built = []
    for i, ((t, d), (cx, cw)) in enumerate(zip(steps, cc)):
        last = i == 4
        pt = P(t, 18, "serif", True, BRASS_L if last else NAVY)
        pd = P(d, 13, fill=CREAM if last else INK, maxw=cw - 2 * pad)
        built.append((pt, pd))
    head = max(2 * r, max(p.h for p, _ in built))
    H = pad + head + 12 + max(d.h for _, d in built) + pad
    y = pg.top
    cards = []
    for i, ((pt, pd), (cx, cw)) in enumerate(zip(built, cc)):
        last = i == 4
        c = s.box(cx, y, cw, H, fill=NAVY if last else PAPER, stroke=BRASS_D if last else NAVY, sw=2, rx=10,
                  shadow=True)
        num_badge(s, cx + pad + r, y + pad + head / 2, r, i + 1, fill=BRASS_L if last else NAVY,
                  color=NAVY_D if last else CREAM, parent=c)
        s.text(pt, cx + pad + 2 * r + 10, y + pad + (head - pt.h) / 2, c)
        s.text(pd, cx + pad, y + pad + head + 12, c)
        cards.append(c)
    for a, b in zip(cards, cards[1:]):
        s.link(a, b, at=a.y + pad + head / 2)
    y = cards[0].b + 28
    panel = s.box(x0, y, x1 - x0, 0, fill=PAPER, stroke=BRASS, sw=1.5, rx=12, dash=None)
    ph = P("Example: failed file lookups (plugin: `mod_loader`, one session with 23 mods)", 15, bold=True,
           fill=NAVY, maxw=x1 - x0 - 2 * pad)
    s.text(ph, x0 + pad, y + 14, panel)
    ty = y + 14 + ph.h + 12
    cells = [P(e, 12.5, maxw=cw - 2 * 12) for e, (_, cw) in zip(ex, cc)]
    chh = max(p.h for p in cells) + 2 * 12
    # cells line up with the step cards above, inset to sit inside the panel
    for i, (p, (cx, cw)) in enumerate(zip(cells, cc)):
        bx = cx + (8 if i == 0 else 0)
        bw = cw - (8 if i in (0, 4) else 0)
        cell = s.box(bx, ty, bw, chh, fill=CREAM if i < 4 else TEAL_L, stroke="#d9cba6" if i < 4 else TEAL,
                     sw=1, rx=8, parent=panel)
        s.text(p, bx + 12, ty + 12, cell)
    panel.h = ty + chh + 14 - y
    return pg.done(panel.b)


def how_it_works() -> str:
    pg = Paper(1200, "How V3X works",
               "You start Victoria 3, V3X loads with it as one extra file (version.dll), plugins fix and speed up "
               "the engine, you play. No game file is changed; delete version.dll to go back to vanilla.",
               "How V3X works, in one picture")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    steps = [("You start Victoria 3", "as usual, from Steam or the Paradox launcher"),
             ("V3X loads with it", "one extra file next to the game: version.dll"),
             ("Plugins fix the engine", "faster loading, fewer crashes, engine bugs fixed"),
             ("You play", "single player; every plugin can be turned off")]
    cc = cols(x0, x1, 4, 40)
    pad, r = 16, 15
    built = [(P(t, 16, bold=True, fill=NAVY, maxw=cw - 2 * pad - 2 * r - 10), P(d, 14, maxw=cw - 2 * pad))
             for (t, d), (_, cw) in zip(steps, cc)]
    head = max(2 * r, max(a.h for a, _ in built))
    H = pad + head + 12 + max(b.h for _, b in built) + pad
    y = pg.top
    cards = []
    for i, ((a, b), (cx, cw)) in enumerate(zip(built, cc)):
        c = s.box(cx, y, cw, H, fill=PAPER, stroke=NAVY, sw=2, rx=10, shadow=True)
        num_badge(s, cx + pad + r, y + pad + head / 2, r, i + 1, parent=c)
        s.text(a, cx + pad + 2 * r + 10, y + pad + (head - a.h) / 2, c)
        s.text(b, cx + pad, y + pad + head + 12, c)
        cards.append(c)
    for a, b in zip(cards, cards[1:]):
        s.link(a, b, at=a.y + pad + head / 2)
    y = cards[0].b + 22
    ban = s.box(x0, y, x1 - x0, 0, fill=NAVY, stroke=BRASS_D, sw=2, rx=10)
    p = P("No game file is changed. Delete `binaries/version.dll` and the game is exactly vanilla again.", 15,
          bold=True, fill=BRASS_L, maxw=x1 - x0 - 40)
    ban.h = p.h + 28
    s.center(p, ban)
    return pg.done(ban.b)


def lifecycle() -> str:
    pg = Paper(1200, "V3X plugin lifecycle",
               "Process start, plugins load before the CRT, anchors resolve or fail closed, hooks are installed, "
               "the game runs with on_frame and hook callbacks, events between plugins, and process_exit.",
               "A plugin's life", "From process start to exit, and what runs where")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    steps = [("Process start", "Windows maps binaries/version.dll. DllMain only hooks the exe entry point.", NAVY),
             ("Plugins load", "At the entry point, before the CRT: `v3x_plugin_load(&api)` on the main thread.",
              NAVY),
             ("Anchors resolve", "Strings, RTTI and byte checks find code. No unique match = install nothing.", RED),
             ("Hooks go in", "hook_safe, hook, patch: engine code is mapped but has not run yet.", NAVY),
             ("Game runs", "on_frame every frame; hook callbacks on engine threads; console commands.", NAVY),
             ("Events", "load_transition, save_begin / save_end between plugins, on the emitter's thread.", NAVY),
             ("process_exit", "All threads alive: flush reports. Afterwards the API refuses all calls.", NAVY)]
    cc = cols(x0, x1, 7, 12)
    pad, r = 12, 18
    y = pg.top
    ty = y + r + 2
    s.raw(f'<line x1="{f(cc[0][0] + cc[0][1] / 2)}" y1="{f(ty)}" x2="{f(cc[-1][0] + cc[-1][1] / 2)}" y2="{f(ty)}" '
          f'stroke="{BRASS}" stroke-width="3" stroke-linecap="round"/>')
    built = [(P(t, 15, bold=True, fill=c, maxw=cw - 2 * pad), P(d, 12.5, maxw=cw - 2 * pad, lh=1.36))
             for (t, d, c), (_, cw) in zip(steps, cc)]
    cy = ty + r + 16
    H = pad + max(a.h for a, _ in built) + 8 + max(b.h for _, b in built) + pad + 2
    for i, ((a, b), (cx, cw)) in enumerate(zip(built, cc)):
        num_badge(s, cx + cw / 2, ty, r, i + 1, fill=RED if i == 2 else NAVY)
        c = s.box(cx, cy, cw, H, fill=PAPER, stroke=RED if i == 2 else NAVY, sw=1.6, rx=8, shadow=True)
        s.raw(f'<line x1="{f(cx + cw / 2)}" y1="{f(ty + r + 2)}" x2="{f(cx + cw / 2)}" y2="{f(cy)}" '
              f'stroke="{BRASS}" stroke-width="1.5"/>')
        s.text(a, cx + cw / 2, cy + pad, c, "middle")
        s.text(b, cx + cw / 2, cy + pad + a.h + 8, c, "middle")
    # thread table
    y = cy + H + 28
    rows = [("v3x_plugin_load, on_frame, run_on_main", "main thread"),
            ("hook_safe pre / post callbacks", "whatever engine thread called the function"),
            ("console commands", "the console input thread (not the main thread)"),
            ("event subscribers", "the emitting thread")]
    tab = s.box(x0, y, x1 - x0, 0, fill=PAPER, stroke=NAVY, sw=1.6, rx=10)
    pad = 18
    th = P("Which thread runs your code", 16, "serif", True, NAVY)
    s.text(th, x0 + pad, y + 14, tab)
    t = y + 14 + th.h + 10
    kw = max(P(k, 12.5, "mono", True).w for k, _ in rows) + 28
    for k, v in rows:
        kp = P(k, 12.5, "mono", True, NAVY)
        vp = P("→ " + v, 13, maxw=x1 - pad - (x0 + pad + kw))
        bullet(s, x0 + pad, t + (kp.h - vp.h) / 2 if vp.h < kp.h else t, 12.5)
        s.text(kp, x0 + pad + 16, t, tab)
        s.text(vp, x0 + pad + kw, t + (kp.h - vp.h) / 2, tab)
        t += max(kp.h, vp.h) + 4
    tab.h = t - 4 + 16 - y
    return pg.done(tab.b)


def results() -> str:
    pg = Paper(1200, "What the bundled plugins changed",
               "plugin lazy_mesh: mesh loading at startup 37.5 s before, 6.7 s with it. plugin enhance: autosave "
               "in debug mode 156 MB before, 26 MB with it. plugin mem_tuner: New Game crashed before; with it "
               "and a bigger pagefile the game loads and a 4-hour session was played.",
               "What the plugins changed",
               "Each fix is a plugin built on V3X. Measured in game on Victoria 3 1.15.1.")
    s, x0, x1 = pg.s, pg.x0, pg.x1
    rows = [("3D-model loading (mesh pass)", "plugin: `lazy_mesh` · opt-in (mode=lazy)",
             [("before", 37.5, "37.5 s"), ("with the plugin", 6.7, "6.7 s")],
             "Some work moves later: 531 on-demand loads, 28.4 s in total over that 4-hour session."),
            ("Autosave size in debug_mode", "plugin: `enhance` · on by default",
             [("before", 156, "156 MB"), ("with the plugin", 26, "26 MB")],
             "Smaller files only: the ~3.2 s save pause stays (the time goes into serializing)."),
            ("Starting a New Game", "plugin: `mem_tuner` + a bigger Windows pagefile",
             None,
             "Cause in the dumps: Windows commit memory ran out (5-20 MB left) with 0.6-3.9 GB RAM still free.")]
    pad = 20
    lw = max(P(r[0], 18, "serif", True).w for r in rows) + 2 * pad + 10
    y = pg.top
    last = None
    for title, cap, bars, note in rows:
        card = s.box(x0, y, x1 - x0, 0, fill=PAPER, stroke=NAVY, sw=1.6, rx=10, shadow=True)
        pt = P(title, 18, "serif", True, NAVY, maxw=lw - 2 * pad)
        pc = P(cap, 12.5, fill=BRASS_D, maxw=lw - 2 * pad)
        ax0, ax1 = x0 + lw, x1 - pad
        t = y + pad
        if bars:
            bl = max(P(b[0], 12.5, bold=True).w for b in bars) + 14
            tx0 = ax0 + bl
            vmax = max(b[1] for b in bars)
            vlab = max(P(b[2], 15, bold=True).w for b in bars) + 16
            full = ax1 - tx0 - vlab
            bh = 24
            for i, (lab, v, vs) in enumerate(bars):
                by = t + i * (bh + 10)
                lp = P(lab, 12.5, bold=True, fill=INK)
                s.text(lp, tx0 - 12, by + (bh - lp.h) / 2, card, "end")
                track = s.box(tx0, by, ax1 - tx0, bh, fill="none", stroke=None, rx=4, parent=card)
                bw = full * v / vmax
                s.raw(f'<rect x="{f(tx0)}" y="{f(by)}" width="{f(bw)}" height="{f(bh)}" rx="4" '
                      f'fill="{"#ada392" if i == 0 else TEAL}"/>')
                vp = P(vs, 15, bold=True, fill=INK)
                s.text(vp, tx0 + bw + 10, by + (bh - vp.h) / 2, track)
            t += len(bars) * (bh + 10) - 10
        else:
            (bx0, bw0), (bx1, bw1) = cols(ax0, ax1, 2, 16)
            pills = [("before: crashed (5 crash dumps)", RED, RED_L, bx0, bw0),
                     ("after: loads, 4-hour session played", TEAL, TEAL_L, bx1, bw1)]
            for txt, col, bg, px, pw in pills:
                pb = s.box(px, t, pw, 34, fill=bg, stroke=col, sw=1.6, rx=17, parent=card)
                s.center(P(txt, 14, bold=True, fill=col), pb)
            t += 34
        pn = P(note, 12.5, fill=BRASS_D, italic=True, maxw=ax1 - ax0)
        s.text(pn, ax0, t + 10, card)
        t += 10 + pn.h
        lab_h = pt.h + 4 + pc.h
        ch = max(t - y - pad, lab_h)
        ly = y + pad + (ch - lab_h) / 2
        s.text(pt, x0 + pad, ly, card)
        s.text(pc, x0 + pad, ly + pt.h + 4, card)
        card.h = ch + 2 * pad
        last = card
        y = card.b + 16
    return pg.done(last.b)


def mockup_console() -> str:
    W = 960
    s = Svg(W, "Illustration of the V3X console window (not a screenshot)",
            "Illustration of the V3X console window (not a screenshot): loader lines in cyan, plugin lines in "
            "green, a failed anchor in red.")
    cyan, green, red, grey = "#3ad7e0", "#4ee06a", "#ff5c5c", "#d6d6d6"
    win = s.box(4, 4, W - 8, 0, fill="#0c0c0c", stroke="#3a3a3a", sw=2, rx=10)
    bar_h = 40
    bar = s.box(4, 4, W - 8, bar_h, fill="#e8e8e8", stroke=None, rx=10, parent=win)
    s.raw(f'<rect x="4" y="{4 + bar_h - 10}" width="{W - 8}" height="10" fill="#e8e8e8"/>')
    tp = P("V3X console - Victoria 3 native loader", 14, fill="#222")
    s.text(tp, 20, 4 + (bar_h - tp.h) / 2, bar)
    tag = P("ILLUSTRATION · not a screenshot", 12.5, bold=True, fill=CREAM)
    tg = s.box(W - 20 - tag.w - 24, 4 + (bar_h - 26) / 2, tag.w + 24, 26, fill=RED, stroke=None, rx=13,
               parent=bar)
    s.center(tag, tg)
    lines = [(cyan, "[14:02:11.031][v3x] V3X 0.6.0 api v6, exe=...\\victoria3.exe base=00007FF6... "
                    "game=release/1.15.1"),
             (cyan, "[14:02:11.032][v3x] entry hook at 00007FF6...: ok"),
             (cyan, "[14:02:11.090][v3x] entry point reached on main thread 4242"),
             (cyan, "[14:02:11.091][v3x] frame hook (PeekMessageW): ok"),
             (green, "[14:02:11.104][mem_tuner] options: table at exe+0x..., 5 option(s) set"),
             (green, "[14:02:11.118][keep_history] patched 11 Trend::Clear calls: played countries keep their "
                     "graphs after a switch"),
             (red, "[14:02:11.120][my_plugin] anchor FAILED: nothing installed (game updated?)"),
             (cyan, f"[14:02:11.400][v3x] {N_PLUGINS - 2}/{N_PLUGINS - 2} plugins loaded, 2 disabled in v3x.ini"),
             (grey, "V3X console is interactive: type 'help' and press Enter."),
             (grey, "> plugins")]
    x, y = 22, 4 + bar_h + 18
    maxw = W - 2 * x
    last = None
    for col, ln in lines:
        p = P(ln, 13, "mono", fill=col, maxw=maxw, lh=1.3)
        last = s.text(p, x, y, win)
        lastp = p
        y += p.h + 8
    cx = x + lastp.widths[-1] + 8
    cur_top = last.top + 0.2 * lastp.size
    s.raw(f'<rect x="{f(cx)}" y="{f(cur_top)}" width="9" height="{f(1.05 * lastp.size)}" fill="{grey}"/>')
    y += 24
    cap = P("Colours as in the real window: cyan = loader, green = plugins, red = failures. Log lines follow "
            "docs/troubleshooting.md.", 12, fill="#9a9a9a", maxw=maxw)
    s.text(cap, x, y, win)
    win.h = y + cap.h + 16 - 4
    return s.render(math.ceil(win.b + 4))


def mockup_menu() -> str:
    W = 960
    s = Svg(W, "Illustration of the V3X plugin menu on the main menu (not a screenshot)",
            "Illustration of the V3X plugin window opened from a V3X button on the main menu, with a checkbox "
            "per plugin (not a screenshot).")
    s.defs["hdr"] = ('<linearGradient id="hdr" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e2c07a"/>'
                     '<stop offset="1" stop-color="#b98b3e"/></linearGradient>')
    frame = s.box(4, 4, W - 8, 0, fill="#1d2026", stroke="#3a3f47", sw=2, rx=10)
    tag = P("ILLUSTRATION · not a screenshot", 12.5, bold=True, fill=CREAM)
    tg = s.box(W - 24 - tag.w - 24, 18, tag.w + 24, 26, fill=RED, stroke=None, rx=13, parent=frame)
    s.center(tag, tg)
    # main menu column
    mx, mw, bh, gap = 40, 190, 40, 14
    items = ["Continue", "New Game", "Load Game", "Settings", "V3X", "Exit"]
    # plugin window
    wx, wy = mx + mw + 56, tg.b + 16
    ww = W - 24 - wx
    win = s.box(wx, wy, ww, 0, fill="#2a2f38", stroke=BRASS, sw=1.6, rx=8, parent=frame)
    hdr_h = 46
    hdr = s.box(wx, wy, ww, hdr_h, fill="url(#hdr)", stroke=None, rx=8, parent=win)
    s.raw(f'<rect x="{f(wx)}" y="{f(wy + hdr_h - 8)}" width="{f(ww)}" height="8" fill="{BRASS}"/>')
    ht = P("V3X plugins", 19, "serif", True, INK)
    s.text(ht, wx + 18, wy + (hdr_h - ht.h) / 2, hdr)
    xb = s.box(wx + ww - 18 - 26, wy + (hdr_h - 26) / 2, 26, 26, fill="none", stroke=None, rx=4, parent=hdr)
    s.raw(f'<path d="M{f(xb.x + 7)},{f(xb.y + 7)} L{f(xb.r - 7)},{f(xb.b - 7)} M{f(xb.r - 7)},{f(xb.y + 7)} '
          f'L{f(xb.x + 7)},{f(xb.b - 7)}" stroke="{INK}" stroke-width="2.4" stroke-linecap="round"/>')
    y = wy + hdr_h + 14
    sub = P("Ticked = loaded at the next game start. Saved to `binaries/v3x/v3x.ini`, section [plugins].", 12.5,
            fill="#bdbdbd", maxw=ww - 36)
    s.text(sub, wx + 18, y, win)
    y += sub.h + 14
    plugins = [("crash_logger", "own crash log with C++ type names and stacks", True, None),
               ("mem_tuner", "lower memory commit, OOM rescue", True, None),
               ("keep_history", "country graphs survive a country switch", True, None),
               ("frame_pacer", "timer sleep instead of a busy-wait frame cap", True, None),
               ("lazy_mesh", "load mesh types on first use", False, "restart required"),
               ("alloc_tracker", "allocation report per call site (diagnostic)", False, None),
               ("asset_stream", "force the engine texture streamer on", False, None)]
    nw = max(P(n, 13.5, "mono", True).w for n, *_ in plugins)
    rh, rg = 36, 7
    for name, desc, on, note in plugins:
        row = s.box(wx + 14, y, ww - 28, rh, fill="#323844", stroke=None, rx=4, parent=win)
        cb = row.x + 12
        s.raw(f'<rect x="{f(cb)}" y="{f(row.cy - 10)}" width="20" height="20" rx="3" fill="#1d2026" '
              f'stroke="{BRASS}" stroke-width="1.6"/>')
        if on:
            s.raw(f'<path d="M{f(cb + 4.5)},{f(row.cy)} L{f(cb + 8.5)},{f(row.cy + 4.5)} L{f(cb + 15.5)},'
                  f'{f(row.cy - 5)}" fill="none" stroke="{BRASS_L}" stroke-width="2.4" stroke-linecap="round" '
                  f'stroke-linejoin="round"/>')
        pn = P(name, 13.5, "mono", True, "#e8e8e8")
        s.text(pn, cb + 32, row.cy - pn.h / 2, row)
        dx = cb + 32 + nw + 22
        right = row.r - 12
        if note:
            pnote = P(note, 12, bold=True, fill=BRASS_L)
            s.text(pnote, right, row.cy - pnote.h / 2, row, "end")
            right -= pnote.w + 16
        pd = P(desc, 12.5, fill="#bdbdbd", maxw=right - dx)
        s.text(pd, dx, row.cy - pd.h / 2, row)
        y += rh + rg
    win.h = y - rg + 16 - wy
    # menu buttons, centred on the window
    total = len(items) * bh + (len(items) - 1) * gap
    my = win.cy - total / 2
    v3x_btn = None
    for i, it in enumerate(items):
        hi = it == "V3X"
        b = s.box(mx, my + i * (bh + gap), mw, bh, fill=NAVY if hi else "#2a2f38", stroke=BRASS if hi else "#4a505a",
                  sw=1.6, rx=4, parent=frame)
        s.center(P(it, 16, "serif", hi, BRASS_L if hi else "#c9c9c9"), b)
        if hi:
            v3x_btn = b
    s.raw(f'<path d="M{f(v3x_btn.r + 4)},{f(v3x_btn.cy)} L{f(wx - 4)},{f(win.y + 2 * hdr_h)}" stroke="{BRASS}" '
          f'stroke-width="1.6" fill="none"/>')
    y = win.b + 18
    cap = P("Layout follows plugins/mod_menu (V3X button + plugin window). The game's real art is not reproduced. "
            "Not yet verified in game.", 12, fill="#8d8d8d", maxw=W - 2 * mx)
    s.text(cap, mx, y, frame)
    frame.h = y + cap.h + 14 - 4
    return s.render(math.ceil(frame.b + 4))


def gear_path(cx, cy, ro, ri, teeth, tw=0.42):
    pts = []
    step = 2 * math.pi / teeth
    for i in range(teeth):
        a = i * step
        for da, r in ((-step * tw / 2 - step * 0.08, ri), (-step * tw / 2, ro), (step * tw / 2, ro),
                      (step * tw / 2 + step * 0.08, ri)):
            pts.append((cx + r * math.cos(a + da), cy + r * math.sin(a + da)))
    return "M" + " L".join(f"{f(x)},{f(y)}" for x, y in pts) + "Z"


def logo_art():
    """The logo's artwork (gear, disc, monogram) for embedding; it uses the same #brass gradient."""
    src = (ASSETS / "logo.svg").read_text(encoding="utf-8")
    return re.search(r"</defs>(.*)</svg>", src, re.S).group(1)


def banner() -> str:
    W, H = 1280, 400
    s = Svg(W, "V3X: the engine-level modding platform for Victoria 3",
            "Banner: V3X emblem and title, the engine-level modding platform. Build mods that run inside Victoria "
            "3's engine: hook functions safely, read live data, add commands. Loader 0.6.0, API v6, SDK, "
            f"{N_PLUGINS} plugins. Game 1.15.1, Windows x64, single player, unofficial.")
    s.defs["brass"] = GRAD
    s.defs["grid"] = GRID
    s.defs["gridL"] = ('<pattern id="gridL" width="24" height="24" patternUnits="userSpaceOnUse"><path '
                       'd="M24,0 L0,0 0,24" fill="none" stroke="#e2c07a" stroke-width=".5" opacity=".12"/></pattern>')
    cx0 = 300  # text column
    kick = P("ENGINE WORKS FOR VICTORIA 3  ·  EST. 1836", 15, bold=True, fill=BRASS_D, ls=3)
    big = P("V3X", 112, "serif", True, NAVY)
    tag = P("the engine-level\nmodding platform", 28, "serif", False, INK, italic=True, lh=1.18)
    desc = P("Build mods that run inside Victoria 3's engine:\nhook functions safely, read live data, add commands.",
             21, fill=INK, lh=1.3)
    pills = [("LOADER", "version.dll 0.6.0"), ("API v6", "plain C ABI"), ("SDK", "anchors, tests"),
             ("PLUGINS", f"{N_PLUGINS} bundled + yours")]
    pp = [(P(a, 13, bold=True, fill=BRASS_L, ls=1.5), P(b, 12, "mono", fill=CREAM)) for a, b in pills]
    pw = max(max(a.w, b.w) for a, b in pp) + 28
    pills_w = len(pp) * pw + (len(pp) - 1) * 12
    col_w = max(big.w + 24 + tag.w, desc.w, pills_w, kick.w)
    split = math.ceil(max(cx0 + col_w + 48, 880))
    # panels
    s.bg = [f'<rect x="4" y="4" width="{W - 8}" height="{H - 8}" rx="16" fill="{CREAM}" stroke="{NAVY}" '
            f'stroke-width="3"/>',
            f'<rect x="4" y="4" width="{W - 8}" height="{H - 8}" rx="16" fill="url(#grid)"/>',
            f'<path d="M{split},5.5 L{W - 20},5.5 Q{W - 5.5},5.5 {W - 5.5},20 L{W - 5.5},{H - 20} Q{W - 5.5},'
            f'{H - 5.5} {W - 20},{H - 5.5} L{split},{H - 5.5} Z" fill="{NAVY}"/>',
            f'<path d="M{split},5.5 L{W - 20},5.5 Q{W - 5.5},5.5 {W - 5.5},20 L{W - 5.5},{H - 20} Q{W - 5.5},'
            f'{H - 5.5} {W - 20},{H - 5.5} L{split},{H - 5.5} Z" fill="url(#gridL)"/>',
            f'<rect x="{split - 6}" y="5" width="6" height="{H - 10}" fill="{BRASS}"/>',
            f'<rect x="16" y="16" width="{W - 32}" height="{H - 32}" rx="10" fill="none" stroke="{BRASS}" '
            f'stroke-width="1.2"/>',
            f'<rect x="4" y="4" width="{W - 8}" height="{H - 8}" rx="16" fill="none" stroke="{NAVY}" '
            f'stroke-width="3"/>']
    # emblem
    k = 200 / 256
    ex, ey = 40, 52
    s.raw(f'<g transform="translate({ex},{ey}) scale({f(k)})">{logo_art()}</g>')
    foot = P("GAME 1.15.1 · WINDOWS x64\nSINGLE PLAYER · UNOFFICIAL", 11, bold=True, fill=NAVY_L, ls=1.2, lh=1.45)
    s.text(foot, ex + 100, ey + 200 + 18, None, "middle")
    # text column, vertically centred
    gap1, gap2, gap3 = 4, 18, 22
    pill_h = 50
    total = kick.h + gap1 + big.h + gap2 + desc.h + gap3 + pill_h
    y = (H - total) / 2
    s.text(kick, cx0, y)
    y += kick.h + gap1
    s.text(big, cx0, y)
    s.text(tag, cx0 + big.w + 24, y + (big.h - tag.h) / 2 + 6)
    y += big.h + 6
    s.raw(f'<line x1="{cx0}" y1="{f(y + 4)}" x2="{f(split - 48)}" y2="{f(y + 4)}" stroke="{BRASS}" '
          f'stroke-width="1.5"/>')
    y += gap2 - 6
    s.text(desc, cx0, y)
    y += desc.h + gap3
    for i, (a, b) in enumerate(pp):
        bx = s.box(cx0 + i * (pw + 12), y, pw, pill_h, fill=NAVY, stroke=BRASS_D, sw=1.5, rx=6)
        t = y + (pill_h - (a.h + b.h)) / 2
        s.text(a, bx.cx, t, bx, "middle")
        s.text(b, bx.cx, t + a.h, bx, "middle")
    # right panel: gears and the API seal
    rcx = (split + W - 6) / 2
    g1 = (rcx - 10, 150, 92)
    s.raw(f'<path d="{gear_path(g1[0], g1[1], g1[2], g1[2] - 14, 18)}" fill="url(#brass)" stroke="{BRASS_D}" '
          f'stroke-width="2" stroke-linejoin="round"/>')
    seal = s.box(g1[0] - 56, g1[1] - 56, 112, 112, fill=NAVY, stroke=BRASS_D, sw=2.5, rx=56)
    sa, sb = P("API", 13, bold=True, fill=BRASS_L, ls=2), P("v6", 30, "serif", True, CREAM)
    t = seal.cy - (sa.h + sb.h) / 2
    s.text(sa, seal.cx, t, seal, "middle")
    s.text(sb, seal.cx, t + sa.h, seal, "middle")
    for gx, gy, gr, n in ((rcx - 80, 300, 44, 12), (W - 70, 262, 52, 14)):
        s.raw(f'<path d="{gear_path(gx, gy, gr, gr - 10, n)}" fill="url(#brass)" stroke="{BRASS_D}" '
              f'stroke-width="2" stroke-linejoin="round"/><circle cx="{f(gx)}" cy="{f(gy)}" r="{f(gr * 0.32)}" '
              f'fill="{NAVY}" stroke="{BRASS_D}" stroke-width="2"/><circle cx="{f(gx)}" cy="{f(gy)}" '
              f'r="{f(gr * 0.12)}" fill="{BRASS_L}"/>')
    return s.render(H)


def badge(left, right, fill_r, color_r, title) -> str:
    st = St(11, bold=True)
    lw = text_width(left, st) + 18
    rw = text_width(right, st) + 18
    W, H = math.ceil(lw + rw), 20
    s = Svg(W, title, title)
    s.defs["r"] = f'<clipPath id="r"><rect width="{W}" height="{H}" rx="3"/></clipPath>'
    s.raw('<g clip-path="url(#r)">')
    a = s.box(0, 0, lw, H, fill=NAVY, stroke=None, rx=0)
    b = s.box(lw, 0, W - lw, H, fill=fill_r, stroke=None, rx=0)
    s.raw("</g>")
    # 11px text in a 20px badge: centre the visible glyphs, not the (taller) em box
    for box, txt, col in ((a, left, BRASS_L), (b, right, color_r)):
        p = P(txt, 11, bold=True, fill=col)
        s.text(p, box.cx, H / 2 + 0.36 * 11 - ASC * 11, box, "middle")
    return s.render(H)


DIAGRAMS = {
    "banner": banner, "platform": platform, "architecture": architecture, "compare": compare,
    "anchors": anchors, "discovery": discovery, "how-it-works": how_it_works, "lifecycle": lifecycle,
    "results": results, "mockup-console": mockup_console, "mockup-menu": mockup_menu,
    "badge-v3x-api": lambda: badge("V3X", "API v6 · 1.15.1", TEAL, CREAM, "Built for V3X API v6, game 1.15.1"),
    "badge-v3x-plugin": lambda: badge("V3X", "plugin · fails closed", BRASS_L, INK, "V3X plugin, fails closed"),
}


def main(argv):
    out = ASSETS
    names = []
    it = iter(argv)
    for a in it:
        if a == "--out":
            out = Path(next(it))
        else:
            names.append(a)
    out.mkdir(parents=True, exist_ok=True)
    for name in names or DIAGRAMS:
        svg = DIAGRAMS[name]()
        (out / f"{name}.svg").write_text(svg, encoding="utf-8", newline="\n")
        print(f"wrote {out / (name + '.svg')}")


if __name__ == "__main__":
    main(sys.argv[1:])
