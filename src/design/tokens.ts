/**
 * The single source of every design value on the site.
 * BaseLayout turns these into CSS custom properties; /design/ prints them, with contrast
 * ratios computed from the same numbers, so the design page and the screen cannot disagree.
 * scripts/check-tokens.ts fails the build if a colour literal appears anywhere else in src/.
 */

export interface ColourToken {
  name: string;
  light: string;
  dark: string;
  role: string;
  /** text tokens are checked against these backgrounds (WCAG 2.x contrast) */
  textOn?: string[];
  /** minimum contrast: 4.5 body text, 3 large text / UI */
  min?: number;
}

export const colours: ColourToken[] = [
  { name: "paper", light: "#f4ecd8", dark: "#13202f", role: "Page ground. Ledger paper by day, blueprint by night. The light value is the banner card of the repo's banner.svg." },
  { name: "paper-2", light: "#ece2c8", dark: "#1a2b3f", role: "Panels, table heads, the masthead strip." },
  { name: "paper-3", light: "#e3d5b2", dark: "#22364d", role: "Code ground, bar tracks, selected rows." },
  { name: "ink", light: "#14202e", dark: "#efe6d0", role: "Body text, rules under totals, figure bars.", textOn: ["paper", "paper-2", "paper-3"], min: 4.5 },
  { name: "ink-2", light: "#46505c", dark: "#bdb6a4", role: "Secondary text: captions, dates, footnotes.", textOn: ["paper", "paper-2"], min: 4.5 },
  { name: "link", light: "#1f3550", dark: "#a8c8e8", role: "Links and the primary button. Navy of the V3X banner.", textOn: ["paper", "paper-2", "paper-3"], min: 4.5 },
  { name: "on", light: "#2a6467", dark: "#86c9c6", role: "'On by default', results with V3X. Teal of the repo diagrams, darkened 10 % for text.", textOn: ["paper", "paper-2"], min: 4.5 },
  { name: "brass", light: "#b98b3e", dark: "#c9a35a", role: "Ornament only: double rules, the section numerals' hairline. Never text.", },
  { name: "brass-ink", light: "#6f5220", dark: "#dcbb78", role: "Brass you can read: kickers and small-caps labels.", textOn: ["paper", "paper-2"], min: 4.5 },
  { name: "alarm", light: "#9b2d20", dark: "#ec9a8b", role: "Warnings and 'before' values. Red of the repo diagrams.", textOn: ["paper", "paper-2"], min: 4.5 },
  { name: "rule", light: "#bfb398", dark: "#3c516a", role: "Hairlines between rows and columns. Decorative, so no contrast floor." },
  { name: "focus", light: "#2f6f73", dark: "#86c9c6", role: "Keyboard focus ring, 2 px, offset 2 px.", textOn: ["paper"], min: 3 },
];

/** The module. 6 px = a nonpareil, the 6-point body that printers of the period set column rules and footnotes in. */
export const MODULE = 6;

export const space = [
  { name: "s1", px: 6 }, { name: "s2", px: 12 }, { name: "s3", px: 18 }, { name: "s4", px: 24 },
  { name: "s6", px: 36 }, { name: "s8", px: 48 }, { name: "s12", px: 72 }, { name: "s16", px: 96 },
];

export interface TypeToken { name: string; family: "display" | "text" | "mono"; size: number; line: number; use: string; small?: { size: number; line: number } }

/** Every size and line is a whole number of modules except the sizes of 15 and 21 px, whose lines are. */
export const type: TypeToken[] = [
  { name: "display", family: "display", size: 66, line: 72, use: "One per page: the headline.", small: { size: 42, line: 48 } },
  { name: "h1", family: "display", size: 48, line: 54, use: "Page titles of inner pages.", small: { size: 36, line: 42 } },
  { name: "h2", family: "display", size: 30, line: 36, use: "Section heads, after a numeral." },
  { name: "h3", family: "text", size: 21, line: 30, use: "Sub-heads, set in the text face, semibold." },
  { name: "lede", family: "text", size: 21, line: 36, use: "The paragraph under a headline." },
  { name: "body", family: "text", size: 18, line: 30, use: "Running text. Measure 64 characters." },
  { name: "small", family: "text", size: 15, line: 24, use: "Captions, footnotes, table text." },
  { name: "mono", family: "mono", size: 15, line: 24, use: "Numbers, code, file names, labels." },
  { name: "label", family: "mono", size: 12, line: 18, use: "Small caps substitutes: uppercase, +0.08 em tracking." },
];

export const families = {
  display: { stack: '"Old Standard TT", "Bodoni 72", Didot, Georgia, serif', file: "Old Standard TT 400, 400 italic, 700 (OFL)", why: "A revival of the 'Modern' romans of the late 19th century: the face of the period's newspapers, statute books and engineering catalogues." },
  text: { stack: '"Source Serif 4", Georgia, "Times New Roman", serif', file: "Source Serif 4 variable, optical sizes 8-60, weights 200-900 (OFL)", why: "A transitional serif with optical sizes, so 15 px captions stay sturdy and 21 px ledes stay fine. Readable for long docs." },
  mono: { stack: '"IBM Plex Mono", Consolas, "Courier New", monospace', file: "IBM Plex Mono 400, 500 (OFL)", why: "Tabular figures by construction: every digit is the same width, so columns of seconds and megabytes line up like a ledger." },
};

export const grid = { max: 1176, columns: 12, gutter: 24, measure: "64ch" };

function hexToRgb(h: string) {
  const n = parseInt(h.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
function lum(h: string) {
  const [r, g, b] = hexToRgb(h).map((v) => {
    const c = v / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
export function contrast(a: string, b: string) {
  const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p);
  return (x + 0.05) / (y + 0.05);
}

export function colour(name: string, scheme: "light" | "dark") {
  const t = colours.find((c) => c.name === name);
  if (!t) throw new Error("no colour token " + name);
  return t[scheme];
}

/** Every text pair with its ratio in both schemes. */
export function contrastTable() {
  const rows: { fg: string; bg: string; scheme: "light" | "dark"; ratio: number; min: number }[] = [];
  for (const c of colours) for (const bg of c.textOn ?? []) for (const scheme of ["light", "dark"] as const) {
    rows.push({ fg: c.name, bg, scheme, ratio: contrast(c[scheme], colour(bg, scheme)), min: c.min ?? 4.5 });
  }
  return rows;
}

export function tokensCss(): string {
  const v = (scheme: "light" | "dark") => colours.map((c) => `--${c.name}:${c[scheme]};`).join("");
  const sp = space.map((s) => `--${s.name}:${s.px}px;`).join("");
  const ty = type.map((t) => `--t-${t.name}:${t.size}px/${t.line}px var(--f-${t.family});`).join("");
  const tySmall = type.filter((t) => t.small).map((t) => `--t-${t.name}:${t.small!.size}px/${t.small!.line}px var(--f-${t.family});`).join("");
  const fam = `--f-display:${families.display.stack};--f-text:${families.text.stack};--f-mono:${families.mono.stack};`;
  return `:root{color-scheme:light dark;--m:${MODULE}px;${sp}${ty}${fam}--max:${grid.max}px;--gutter:${grid.gutter}px;--measure:${grid.measure};${v("light")}}` +
    `@media (prefers-color-scheme: dark){:root{${v("dark")}}}` +
    `@media (max-width: 720px){:root{${tySmall}}}`;
}
