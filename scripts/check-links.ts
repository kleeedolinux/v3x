/**
 * Checks dist/ after a build. Fails (exit 1) on:
 *  - an internal href/src that does not resolve to a file in dist/ (or a #fragment with no matching id)
 *  - a <script> loaded from another origin, an external stylesheet / font / image
 *  - an <img> without alt, a page without exactly one <h1>, a <title> or a meta description
 * External links (https://...) are listed, not fetched.
 *
 *   bun scripts/check-links.ts
 */
import { readdirSync, readFileSync, statSync, existsSync } from "node:fs";
import { join, relative } from "node:path";

const DIST = join(import.meta.dir, "../dist");
const SITE = "https://v3extender.pages.dev";

function walk(dir: string, out: string[] = []) {
  for (const f of readdirSync(dir)) {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) walk(p, out);
    else out.push(p);
  }
  return out;
}

if (!existsSync(DIST)) { console.error("check-links: dist/ missing, run the build first"); process.exit(1); }
const files = walk(DIST);
const htmlFiles = files.filter((f) => f.endsWith(".html"));
const rel = (p: string) => "/" + relative(DIST, p).replace(/\\/g, "/");

const ids = new Map<string, Set<string>>();
const pages = new Map<string, string>();
for (const f of htmlFiles) {
  const html = readFileSync(f, "utf8");
  pages.set(rel(f), html);
  const set = new Set<string>();
  for (const m of html.matchAll(/\sid="([^"]+)"/g)) set.add(decodeEntities(m[1]));
  for (const m of html.matchAll(/\sname="([^"]+)"/g)) set.add(decodeEntities(m[1]));
  ids.set(rel(f), set);
}

function decodeEntities(s: string) {
  return s.replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&#x27;/g, "'");
}

/** URL path -> dist file */
function resolve(path: string): string | null {
  const clean = decodeURIComponent(path);
  const cands = clean.endsWith("/") ? [clean + "index.html"] : [clean, clean + "/index.html", clean + ".html"];
  for (const c of cands) if (existsSync(join(DIST, c)) && statSync(join(DIST, c)).isFile()) return "/" + c.replace(/^\/+/, "");
  return null;
}

const errors: string[] = [];
const external = new Set<string>();
let checked = 0;

for (const [page, html] of pages) {
  const is404 = page === "/404.html";
  // structure
  const h1 = (html.match(/<h1[\s>]/g) ?? []).length;
  if (h1 !== 1) errors.push(`${page}: ${h1} <h1> elements (want 1)`);
  if (!/<title>[^<]+<\/title>/.test(html)) errors.push(`${page}: no <title>`);
  if (!/<meta name="description" content="[^"]+"/.test(html)) errors.push(`${page}: no meta description`);
  if (!is404 && !/<link rel="canonical"/.test(html)) errors.push(`${page}: no canonical link`);
  for (const m of html.matchAll(/<img\b[^>]*>/g)) if (!/\salt="/.test(m[0])) errors.push(`${page}: <img> without alt: ${m[0].slice(0, 80)}`);
  // scripts and external resources
  // inline scripts (progressive enhancement only) are allowed; scripts from another origin are not
  for (const m of html.matchAll(/<script\b([^>]*)>/g)) {
    const src = m[1].match(/\ssrc="([^"]+)"/)?.[1];
    if (src && /^(https?:)?\/\//.test(src)) errors.push(`${page}: external script: ${src}`);
  }
  for (const m of html.matchAll(/<link\b[^>]*rel="(stylesheet|preload|icon)"[^>]*>/g)) {
    const href = m[0].match(/href="([^"]+)"/)?.[1] ?? "";
    if (/^(https?:)?\/\//.test(href)) errors.push(`${page}: external ${m[1]}: ${href}`);
  }
  for (const m of html.matchAll(/<img\b[^>]*\ssrc="([^"]+)"/g)) if (/^(https?:)?\/\//.test(m[1])) errors.push(`${page}: external image: ${m[1]}`);
  for (const m of html.matchAll(/url\((['"]?)(https?:)?\/\/[^)]+\)/g)) errors.push(`${page}: external url() in inline CSS: ${m[0]}`);

  // links. Skip <head> meta that holds absolute site URLs, but check canonical/alternate point at real pages.
  const refs = [...html.matchAll(/\s(?:href|src)="([^"]+)"/g)].map((m) => decodeEntities(m[1]));
  for (let ref of refs) {
    if (ref.startsWith(SITE)) ref = ref.slice(SITE.length) || "/";
    if (/^(mailto:|tel:|data:)/.test(ref)) continue;
    if (/^(https?:)?\/\//.test(ref)) { external.add(ref); continue; }
    checked++;
    const [pathPart, frag] = ref.split("#");
    let target: string | null;
    if (pathPart === "") target = page;
    else if (pathPart.startsWith("/")) target = resolve(pathPart);
    else { errors.push(`${page}: relative link ${ref} (use absolute paths)`); continue; }
    if (!target) { errors.push(`${page}: broken link ${ref}`); continue; }
    if (frag && target.endsWith(".html")) {
      const set = ids.get(target);
      if (!set?.has(decodeURIComponent(frag))) errors.push(`${page}: missing anchor #${frag} in ${target}`);
    }
  }
}

// CSS files: url() must be local and exist
for (const f of files.filter((p) => p.endsWith(".css"))) {
  const css = readFileSync(f, "utf8");
  for (const m of css.matchAll(/url\(\s*(['"]?)([^'")]+)\1\s*\)/g)) {
    const u = m[2];
    if (u.startsWith("data:")) continue;
    if (/^(https?:)?\/\//.test(u)) { errors.push(`${rel(f)}: external url() ${u}`); continue; }
    checked++;
    if (u.startsWith("/") && !resolve(u)) errors.push(`${rel(f)}: missing ${u}`);
  }
}

// sitemap entries must exist
const sm = existsSync(join(DIST, "sitemap.xml")) ? readFileSync(join(DIST, "sitemap.xml"), "utf8") : "";
if (!sm) errors.push("sitemap.xml missing");
for (const m of sm.matchAll(/<loc>([^<]+)<\/loc>|href="([^"]+)"/g)) {
  const u = (m[1] ?? m[2]).replace(SITE, "");
  checked++;
  if (!resolve(u)) errors.push(`sitemap.xml: ${u} does not exist`);
}

// the one-liner `irm https://v3extender.pages.dev/install.ps1 | iex` needs dist/install.ps1, byte-equal
// to the repo's installer/install.ps1 when the repo is on this machine
const PS1 = join(DIST, "install.ps1");
const REPO = process.env.V3X_REPO ?? "D:/SteamLibrary/steamapps/common/Victoria 3/v3x";
const REPO_PS1 = join(REPO, "installer/install.ps1");
let ps1Note = "not checked";
if (!existsSync(PS1)) errors.push("install.ps1 missing from dist/ (public/install.ps1 is copied by scripts/sync-docs.ts)");
else {
  const bytes = readFileSync(PS1);
  if (bytes.some((b) => b > 0x7f)) errors.push("install.ps1 is not ASCII");
  if (existsSync(REPO_PS1)) {
    if (!bytes.equals(readFileSync(REPO_PS1))) errors.push(`install.ps1 in dist/ differs from ${REPO_PS1} (run bun run sync)`);
    else ps1Note = `${bytes.length} bytes, equal to the repo copy`;
  } else ps1Note = `${bytes.length} bytes, repo not here so not compared`;
}
const headers = existsSync(join(DIST, "_headers")) ? readFileSync(join(DIST, "_headers"), "utf8") : "";
if (!/^\/install\.ps1\r?\n(?:[ \t]+.*\r?\n)*?[ \t]+Content-Type: text\/plain/m.test(headers)) errors.push("_headers: no Content-Type rule for /install.ps1");

console.log(`check-links: install.ps1 ${ps1Note}`);
console.log(`check-links: ${htmlFiles.length} pages, ${checked} internal references checked, ${external.size} distinct external links (not fetched)`);
if (process.argv.includes("--external")) for (const e of [...external].sort()) console.log("  ext " + e);
if (errors.length) {
  for (const e of errors) console.error("  FAIL " + e);
  console.error(`check-links: ${errors.length} problem(s)`);
  process.exit(1);
}
console.log("check-links: OK");
