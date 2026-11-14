/**
 * Check the generated diagrams in a real browser, and screenshot them.
 *
 *   bun tools/svg/check_assets.ts [--dir docs/assets] [--shots DIR] [names...]
 *
 * Needs playwright with Chromium (PLAYWRIGHT_BROWSERS_PATH may point at the browser cache). If the
 * package is not installed next to this file, set V3X_PLAYWRIGHT to its directory
 * (e.g. D:/somewhere/node_modules/playwright).
 *
 * Each SVG is loaded inline at scale 1 and measured with getBoundingClientRect. Rules (the data-*
 * attributes are written by tools/svg/gen_assets.py):
 *   - a <text data-box="X"> lies inside <rect data-id="X">;
 *   - no text intersects a box that is not its own box or one of that box's ancestors;
 *   - no two texts intersect;
 *   - a box with data-parent lies inside its parent, and sibling boxes do not overlap;
 *   - no arrow (path[data-arrow]) crosses a text;
 *   - everything lies inside the viewBox.
 * Exit code 1 on any violation. Screenshots: each file as <img> at 900 px and 1400 px wide, and as
 * the README uses it (GitHub's ~880 px column; width="100%", "49%", 64 or 32 px).
 */
import { readFileSync, readdirSync, mkdirSync, writeFileSync } from "node:fs";
import { join, resolve, basename } from "node:path";

const args = process.argv.slice(2);
const opt = (k: string, d: string) => {
  const i = args.indexOf(k);
  if (i < 0) return d;
  const v = args[i + 1];
  args.splice(i, 2);
  return v;
};
const ROOT = resolve(import.meta.dir, "../..");
const DIR = resolve(opt("--dir", join(ROOT, "docs/assets")));
const SHOTS = opt("--shots", "");
const only = args;

const pwPath = process.env.V3X_PLAYWRIGHT ?? "playwright";
const { chromium } = await import(pwPath);

const files = readdirSync(DIR)
  .filter((f) => f.endsWith(".svg"))
  .filter((f) => only.length === 0 || only.includes(basename(f, ".svg")))
  .sort();

/** How the README shows each file (img width inside GitHub's ~880 px column). */
function readmeWidth(name: string): string {
  if (name.startsWith("icon-")) return "32px";
  if (name === "logo") return "64px";
  if (name.startsWith("badge-")) return "auto";
  if (name.startsWith("mockup-")) return "49%";
  return "100%";
}

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1600, height: 1200 }, deviceScaleFactor: 1 });
let total = 0;

for (const file of files) {
  const name = basename(file, ".svg");
  const src = readFileSync(join(DIR, file), "utf8").replace(/^<\?xml[^>]*>\s*/, "");
  await page.setContent(`<!doctype html><html><body style="margin:0;background:#fff">${src}</body></html>`);
  await page.evaluate(() => (document as any).fonts?.ready);
  const problems: string[] = await page.evaluate(() => {
    const out: string[] = [];
    const svg = document.querySelector("svg") as SVGSVGElement;
    const vb = svg.viewBox.baseVal;
    svg.setAttribute("width", String(vb.width));
    svg.setAttribute("height", String(vb.height));
    const o = svg.getBoundingClientRect();
    type R = { x0: number; y0: number; x1: number; y1: number };
    const rel = (el: Element): R => {
      const r = el.getBoundingClientRect();
      return { x0: r.left - o.left, y0: r.top - o.top, x1: r.right - o.left, y1: r.bottom - o.top };
    };
    const T = 0.5; // tolerance in px
    const inside = (a: R, b: R) => a.x0 >= b.x0 - T && a.y0 >= b.y0 - T && a.x1 <= b.x1 + T && a.y1 <= b.y1 + T;
    const hits = (a: R, b: R) => a.x0 < b.x1 - T && b.x0 < a.x1 - T && a.y0 < b.y1 - T && b.y0 < a.y1 - T;
    const fmt = (r: R) => `[${r.x0.toFixed(1)},${r.y0.toFixed(1)} ${r.x1.toFixed(1)},${r.y1.toFixed(1)}]`;
    const view: R = { x0: 0, y0: 0, x1: vb.width, y1: vb.height };

    const boxes = new Map<string, { r: R; parent: string | null }>();
    for (const el of Array.from(svg.querySelectorAll("rect[data-id]")))
      boxes.set(el.getAttribute("data-id")!, { r: rel(el), parent: el.getAttribute("data-parent") });
    const chain = (id: string | null) => {
      const s = new Set<string>();
      while (id) {
        s.add(id);
        id = boxes.get(id)?.parent ?? null;
      }
      return s;
    };
    const texts = Array.from(svg.querySelectorAll("text")).map((el) => ({
      box: el.getAttribute("data-box"),
      r: rel(el),
      s: (el.textContent ?? "").slice(0, 48),
    }));
    for (const t of texts) {
      if (t.r.x1 - t.r.x0 <= 0) continue;
      if (!inside(t.r, view)) out.push(`text outside the viewBox: "${t.s}" ${fmt(t.r)}`);
      if (t.box) {
        const b = boxes.get(t.box);
        if (!b) out.push(`text "${t.s}" names a missing box ${t.box}`);
        else if (!inside(t.r, b.r)) out.push(`text leaves its box ${t.box}: "${t.s}" ${fmt(t.r)} not in ${fmt(b.r)}`);
      }
      const own = chain(t.box);
      for (const [id, b] of boxes)
        if (!own.has(id) && hits(t.r, b.r)) out.push(`text "${t.s}" ${fmt(t.r)} overlaps box ${id} ${fmt(b.r)}`);
    }
    for (let i = 0; i < texts.length; i++)
      for (let j = i + 1; j < texts.length; j++)
        if (hits(texts[i].r, texts[j].r))
          out.push(`texts overlap: "${texts[i].s}" ${fmt(texts[i].r)} and "${texts[j].s}" ${fmt(texts[j].r)}`);
    const ids = Array.from(boxes.keys());
    for (const id of ids) {
      const b = boxes.get(id)!;
      if (!inside(b.r, view)) out.push(`box ${id} outside the viewBox ${fmt(b.r)}`);
      if (b.parent) {
        const p = boxes.get(b.parent);
        if (!p) out.push(`box ${id} names a missing parent ${b.parent}`);
        else if (!inside(b.r, p.r)) out.push(`box ${id} ${fmt(b.r)} leaves its parent ${b.parent} ${fmt(p.r)}`);
      }
    }
    for (let i = 0; i < ids.length; i++)
      for (let j = i + 1; j < ids.length; j++) {
        const a = boxes.get(ids[i])!, b = boxes.get(ids[j])!;
        if (a.parent === b.parent && hits(a.r, b.r)) out.push(`sibling boxes overlap: ${ids[i]} ${fmt(a.r)} and ${ids[j]} ${fmt(b.r)}`);
      }
    for (const el of Array.from(svg.querySelectorAll("path[data-arrow]"))) {
      const r = rel(el);
      const g: R = { x0: r.x0 - 4, y0: r.y0 - 4, x1: r.x1 + 4, y1: r.y1 + 4 };
      for (const t of texts) if (hits(g, t.r)) out.push(`arrow ${fmt(r)} crosses text "${t.s}"`);
    }
    return out;
  });
  total += problems.length;
  console.log(`${problems.length === 0 ? "ok  " : "FAIL"} ${file}${problems.length ? ` (${problems.length})` : ""}`);
  for (const p of problems) console.log(`     ${p}`);

  if (SHOTS) {
    mkdirSync(SHOTS, { recursive: true });
    const url = "file:///" + join(DIR, file).replace(/\\/g, "/");
    const shots: [string, string, number][] = [["readme", readmeWidth(name), 880]];
    if (!name.startsWith("icon-") && !name.startsWith("badge-") && name !== "logo")
      shots.push(["900", "900px", 900], ["1400", "1400px", 1400]);
    for (const [tag, w, col] of shots) {
      const html = join(SHOTS, `_page.html`);
      writeFileSync(html, `<!doctype html><html><body style="margin:0;background:#fff;padding:8px">` +
        `<div style="width:${col}px"><img id="i" src="${url}" style="width:${w};height:auto;display:block"></div></body></html>`);
      await page.setViewportSize({ width: col + 40, height: 1200 });
      await page.goto("file:///" + html.replace(/\\/g, "/"));
      await page.waitForFunction(() => (document.getElementById("i") as HTMLImageElement).complete);
      await page.locator("#i").screenshot({ path: join(SHOTS, `${name}.${tag}.png`) });
    }
  }
}
await browser.close();
console.log(`${files.length} files, ${total} violation(s)`);
process.exit(total === 0 ? 0 : 1);
