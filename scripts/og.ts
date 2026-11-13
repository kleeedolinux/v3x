/**
 * Social share cards (1200x630 PNG) in public/og/. Run by hand when titles change:
 *   bun scripts/og.ts
 * Same look as the site: navy ground, brass V3X lockup, the repo logo. Fonts: Segoe UI / Consolas
 * from C:\Windows\Fonts (resvg cannot read the site's woff2 files).
 */
import { Resvg } from "@resvg/resvg-js";
import { mkdirSync, writeFileSync, readFileSync, existsSync } from "node:fs";
import { join } from "node:path";

const OUT = join(import.meta.dir, "../public/og");
mkdirSync(OUT, { recursive: true });
const esc = (s: string) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;");
// inlined as a nested <svg> (not <image>) so its text uses the fonts given to resvg
const logo = readFileSync(join(import.meta.dir, "../public/assets/logo.svg"), "utf8").replace(/<\?xml[^>]*>/, "").replace(/<svg[^>]*>/, (tag) => tag.replace(/\s(width|height)="[^"]*"/g, "").replace("<svg", '<svg x="860" y="50" width="280" height="280"'));

const cards = [
  { file: "en", kicker: "SCRIPT EXTENDER FOR VICTORIA 3 · 1.15.1 · WINDOWS", line: "Faster loading, fewer crashes, engine fixes.", stats: [["37.5 s → 6.7 s", "3D-model loading"], ["156 → 26 MB", "debug autosave"], ["Fixed", "New Game crash"]], foot: "v3extender.pages.dev" },
  { file: "pt-br", kicker: "SCRIPT EXTENDER PARA VICTORIA 3 · 1.15.1 · WINDOWS", line: "Carrega mais rápido, trava menos, motor corrigido.", stats: [["37,5 s → 6,7 s", "modelos 3D"], ["156 → 26 MB", "autosave debug"], ["Corrigido", "travamento no Novo Jogo"]], foot: "v3extender.pages.dev/pt-br/" },
  { file: "docs", kicker: "DOCUMENTATION · C API · SDK · SAFETY RULES", line: "Engine plugins for Victoria 3, in plain C.", stats: [["find_string", "anchor on engine code"], ["hook_safe", "change it safely"], ["--test", "test without the game"]], foot: "v3extender.pages.dev/docs/" },
];

function svg(c: (typeof cards)[number]) {
  const stats = c.stats.map((s, i) => {
    const x = 80 + i * 360;
    return `<rect x="${x}" y="430" width="340" height="120" rx="18" fill="#ffffff" fill-opacity="0.05" stroke="#ffffff" stroke-opacity="0.1"/>
<text x="${x + 24}" y="485" font-family="Segoe UI" font-weight="bold" font-size="34" fill="#a8e0dc">${esc(s[0])}</text>
<text x="${x + 24}" y="522" font-family="Segoe UI" font-size="20" fill="#a9bbd0">${esc(s[1])}</text>`;
  }).join("\n");
  return `<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="630" viewBox="0 0 1200 630">
<defs>
  <radialGradient id="g1" cx="0.85" cy="0.1" r="0.7"><stop offset="0" stop-color="#b98b3e" stop-opacity="0.35"/><stop offset="1" stop-color="#b98b3e" stop-opacity="0"/></radialGradient>
  <radialGradient id="g2" cx="0.05" cy="1" r="0.6"><stop offset="0" stop-color="#2f6f73" stop-opacity="0.4"/><stop offset="1" stop-color="#2f6f73" stop-opacity="0"/></radialGradient>
  <linearGradient id="b" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#ecd398"/><stop offset="1" stop-color="#b98b3e"/></linearGradient>
</defs>
<rect width="1200" height="630" fill="#0b1522"/>
<rect width="1200" height="630" fill="url(#g1)"/><rect width="1200" height="630" fill="url(#g2)"/>
${logo}
<text x="80" y="110" font-family="Consolas" font-size="18" letter-spacing="3" fill="#e2c07a">${esc(c.kicker)}</text>
<text x="76" y="250" font-family="Segoe UI" font-weight="bold" font-size="150" fill="url(#b)">V3X</text>
<text x="80" y="320" font-family="Segoe UI" font-weight="bold" font-size="52" fill="#ffffff">Victoria 3 eXtender</text>
<text x="80" y="380" font-family="Segoe UI" font-size="28" fill="#d4dee9">${esc(c.line)}</text>
${stats}
<text x="80" y="600" font-family="Consolas" font-size="18" fill="#7f97b5">${esc(c.foot)}</text>
</svg>`;
}

const fonts = ["segoeui.ttf", "segoeuib.ttf", "consola.ttf", "georgia.ttf", "georgiab.ttf"].map((f) => "C:/Windows/Fonts/" + f).filter(existsSync);
for (const c of cards) {
  const png = new Resvg(svg(c), { font: { fontFiles: fonts, loadSystemFonts: false, defaultFontFamily: "Segoe UI" }, fitTo: { mode: "width", value: 1200 } }).render().asPng();
  writeFileSync(join(OUT, `${c.file}.png`), png);
  console.log(`og: public/og/${c.file}.png ${png.length} bytes`);
}
