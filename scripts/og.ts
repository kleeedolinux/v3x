/**
 * Social share cards (1200x630 PNG) in public/og/. Run by hand when titles change:
 *   bun scripts/og.ts
 * Drawn from the design tokens; fonts: the site's own woff2 files if resvg can read them,
 * else Georgia / Consolas from C:\Windows\Fonts.
 */
import { Resvg } from "@resvg/resvg-js";
import { mkdirSync, writeFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { colour } from "../src/design/tokens";

const OUT = join(import.meta.dir, "../public/og");
mkdirSync(OUT, { recursive: true });
const P = (n: string) => colour(n, "light");
const esc = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;");

const cards = [
  { file: "en", kicker: "SCRIPT EXTENDER FOR VICTORIA 3 · SINGLE PLAYER · WINDOWS", title: ["Victoria 3, with its", "engine looked after."], rows: [["3D-model loading pass", "37.5 s", "6.7 s"], ["Debug-mode autosave", "156 MB", "26 MB"], ["GDP graphs on country switch", "wiped", "kept"]], head: ["", "BEFORE", "WITH V3X"], foot: "Measured on the author's PC · v3x.pages.dev" },
  { file: "pt-br", kicker: "SCRIPT EXTENDER PARA VICTORIA 3 · UM JOGADOR · WINDOWS", title: ["Victoria 3, com o", "motor bem cuidado."], rows: [["Carregamento dos modelos 3D", "37,5 s", "6,7 s"], ["Autosave no modo debug", "156 MB", "26 MB"], ["Gráficos de PIB ao trocar de país", "apagados", "mantidos"]], head: ["", "ANTES", "COM V3X"], foot: "Medido no PC do autor · v3x.pages.dev/pt-br/" },
  { file: "docs", kicker: "DOCUMENTATION · C API · SDK · SAFETY RULES", title: ["Engine plugins for", "Victoria 3, in C."], rows: [["find engine code", "find_string", "find_xref"], ["change it safely", "hook_safe", "patch"], ["test without the game", "testkit", "--test"]], head: ["", "", ""], foot: "v3x.pages.dev/docs/" },
];

function svg(c: (typeof cards)[number]) {
  const rows = c.rows.map((r, i) => {
    const y = 420 + i * 48;
    return `<line x1="80" y1="${y + 16}" x2="1120" y2="${y + 16}" stroke="${P("rule")}" stroke-width="1"/>
<text x="80" y="${y}" font-family="Old Standard TT, Georgia" font-size="28" fill="${P("ink")}">${esc(r[0])}</text>
<text x="900" y="${y}" text-anchor="end" font-family="IBM Plex Mono, Consolas" font-size="26" fill="${P("alarm")}">${esc(r[1])}</text>
<text x="1120" y="${y}" text-anchor="end" font-family="IBM Plex Mono, Consolas" font-size="26" font-weight="bold" fill="${P("on")}">${esc(r[2])}</text>`;
  }).join("\n");
  return `<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="630" viewBox="0 0 1200 630">
<rect width="1200" height="630" fill="${P("paper")}"/>
<text x="80" y="104" font-family="Old Standard TT, Georgia" font-weight="bold" font-size="64" fill="${P("ink")}">V3X</text>
<text x="230" y="104" font-family="Old Standard TT, Georgia" font-style="italic" font-size="26" fill="${P("ink-2")}">engine works for Victoria 3</text>
<rect x="80" y="126" width="1040" height="4" fill="${P("ink")}"/><rect x="80" y="134" width="1040" height="1.5" fill="${P("ink")}"/>
<text x="80" y="176" font-family="IBM Plex Mono, Consolas" font-size="18" letter-spacing="2" fill="${P("brass-ink")}">${esc(c.kicker)}</text>
<text x="80" y="252" font-family="Old Standard TT, Georgia" font-size="62" fill="${P("ink")}">${esc(c.title[0])}</text>
<text x="80" y="322" font-family="Old Standard TT, Georgia" font-size="62" fill="${P("ink")}">${esc(c.title[1])}</text>
<text x="900" y="372" text-anchor="end" font-family="IBM Plex Mono, Consolas" font-size="16" letter-spacing="2" fill="${P("ink-2")}">${esc(c.head[1])}</text>
<text x="1120" y="372" text-anchor="end" font-family="IBM Plex Mono, Consolas" font-size="16" letter-spacing="2" fill="${P("ink-2")}">${esc(c.head[2])}</text>
<line x1="80" y1="384" x2="1120" y2="384" stroke="${P("ink")}" stroke-width="1.5"/>
${rows}
<line x1="80" y1="${420 + 2 * 48 + 20}" x2="1120" y2="${420 + 2 * 48 + 20}" stroke="${P("ink")}" stroke-width="1"/>
<text x="80" y="590" font-family="IBM Plex Mono, Consolas" font-size="16" fill="${P("ink-2")}">${esc(c.foot)}</text>
<rect x="1100" y="574" width="20" height="20" fill="${P("brass")}"/>
</svg>`;
}

const fonts = [
  join(import.meta.dir, "../public/fonts/old-standard-tt-latin-400-normal.woff2"),
  join(import.meta.dir, "../public/fonts/old-standard-tt-latin-400-italic.woff2"),
  join(import.meta.dir, "../public/fonts/old-standard-tt-latin-700-normal.woff2"),
  join(import.meta.dir, "../public/fonts/ibm-plex-mono-latin-400-normal.woff2"),
  ...["georgia.ttf", "georgiab.ttf", "georgiai.ttf", "consola.ttf", "consolab.ttf"].map((f) => "C:/Windows/Fonts/" + f),
].filter(existsSync);

for (const c of cards) {
  const png = new Resvg(svg(c), { font: { fontFiles: fonts, loadSystemFonts: false, defaultFontFamily: "Georgia" }, fitTo: { mode: "width", value: 1200 } }).render().asPng();
  writeFileSync(join(OUT, `${c.file}.png`), png);
  console.log(`og: public/og/${c.file}.png ${png.length} bytes`);
}
