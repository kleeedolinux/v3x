/**
 * Copy the V3X repo's documentation into src/content/docs/ and the diagrams into public/assets/.
 *
 *   bun scripts/sync-docs.ts            # source: $V3X_REPO or the default path below
 *
 * What it does to each file:
 *  - drops the first "# Title" line and stores it as frontmatter `title` (the page renders it);
 *  - rewrites relative links: a link to another synced file becomes /docs/<slug>/#anchor,
 *    docs/assets/*.svg becomes /assets/*.svg, anything else in the repo becomes a GitHub URL;
 *  - records the source path and its modification date in frontmatter.
 *
 * It also copies installer/install.ps1 byte-for-byte to public/install.ps1 (served for
 * `irm https://v3extender.pages.dev/install.ps1 | iex`) and turns installer/profile.txt (which
 * plugins setup ships, and which are on) into src/data/profile.json. Both outputs are committed.
 *
 * If the repo is not on this machine (e.g. a CI build), it prints a note and exits 0, so the
 * copies committed in src/content/docs/, public/install.ps1 and src/data/profile.json are used.
 */
import { existsSync, mkdirSync, readFileSync, writeFileSync, readdirSync, copyFileSync, statSync, rmSync } from "node:fs";
import { join, dirname, posix } from "node:path";

const REPO = process.env.V3X_REPO ?? "D:/SteamLibrary/steamapps/common/Victoria 3/v3x";
const GITHUB = "https://github.com/kleeedolinux/v3x/blob/main/";
const GITHUB_TREE = "https://github.com/kleeedolinux/v3x/tree/main/";
const SITE = join(import.meta.dir, "..");
const OUT = join(SITE, "src/content/docs");
const ASSETS = join(SITE, "public/assets");

type Group = "players" | "modders" | "engine";
interface Doc {
  src: string; // repo-relative, posix
  slug: string;
  group: Group;
  order: number;
  summary: string;
  /** extra repo files appended as code blocks */
  append?: { src: string; lang: string }[];
  /** render the whole file as one code block (for headers) */
  code?: { lang: string; title: string };
}

export const DOCS: Doc[] = [
  { src: "docs/install.md", slug: "install", group: "players", order: 1, summary: "V3X-Setup.exe: download, install, what it changes, repair, uninstall." },
  { src: "docs/faq.md", slug: "faq", group: "players", order: 2, summary: "Common questions, from multiplayer to which compilers work." },
  { src: "docs/troubleshooting.md", slug: "troubleshooting", group: "players", order: 3, summary: "Save the evidence, read v3x.log and crash.log, hangs, build problems." },
  { src: "docs/configuration.md", slug: "configuration", group: "players", order: 4, summary: "v3x.ini and every plugin's ini keys with their defaults." },
  { src: "docs/plugins.md", slug: "plugins", group: "players", order: 5, summary: "The catalog: every bundled plugin, its class, default, test and history." },
  { src: "docs/doctor.md", slug: "doctor", group: "players", order: 6, summary: "The in-game health check: what doctor looks at, what each result means, doctor fix." },
  { src: "docs/updating.md", slug: "updating", group: "players", order: 7, summary: "How V3X updates itself with the updater plugin, and how to update by hand." },

  { src: "docs/getting-started.md", slug: "getting-started", group: "modders", order: 10, summary: "Install the loader, build the template plugin and run it, in about 10 minutes." },
  {
    src: "templates/plugin_template/README.md", slug: "plugin-template", group: "modders", order: 11,
    summary: "The minimal plugin that follows every safety rule, with its source and offline test.",
    append: [
      { src: "templates/plugin_template/my_plugin.c", lang: "c" },
      { src: "templates/plugin_template/my_plugin.ini.example", lang: "ini" },
      { src: "templates/plugin_template/test_my_plugin.c", lang: "c" },
    ],
  },
  { src: "docs/api-reference.md", slug: "api-reference", group: "modders", order: 12, summary: "Every field of V3X_API, thread rules, the manifest, events, return codes." },
  { src: "docs/api-stability.md", slug: "api-stability", group: "modders", order: 13, summary: "What a plugin can rely on when V3X, the game, or both are updated." },
  { src: "include/v3x.h", slug: "v3x-h", group: "modders", order: 13, summary: "The API header itself: plain C, versioned, append-only.", code: { lang: "c", title: "include/v3x.h" } },
  { src: "docs/safety.md", slug: "safety", group: "modders", order: 14, summary: "The rules that keep the game alive. Each one was learned from a crash." },
  { src: "docs/scripting.md", slug: "scripting", group: "modders", order: 15, summary: "Ship game script, localization and GUI files from a native plugin." },
  { src: "docs/testing.md", slug: "testing", group: "modders", order: 16, summary: "Build, repository layout, the offline SDK testkit and end-to-end tests." },
  { src: "docs/console-commands.md", slug: "console-commands", group: "modders", order: 17, summary: "Every V3X console command and the events between plugins." },

  { src: "docs/reverse-engineering.md", slug: "reverse-engineering", group: "engine", order: 20, summary: "The RE workflow: strings, xrefs, functions, profiles, anchors, offline checks." },
  { src: "docs/engine-map.md", slug: "engine-map", group: "engine", order: 21, summary: "What is known about the engine: tick, threads, memory, rendering, saves, VFS." },
];

/**
 * Never published (lead rule): agent-generated reports, and anything the V3X repo gitignores.
 * The list is the floor even without git; `git check-ignore` adds whatever .gitignore says.
 */
const NEVER = [/^docs\/sessions\//, /^docs\/maturity\.md$/, /^docs\/[^/]*audit\.md$/];
const ignoreCache = new Map<string, boolean>();
function isExcluded(src: string): boolean {
  src = src.replace(/\/$/, "");
  if (NEVER.some((r) => r.test(src) || r.test(src + "/"))) return true;
  let hit = ignoreCache.get(src);
  if (hit === undefined) {
    const r = Bun.spawnSync(["git", "-C", REPO, "check-ignore", "-q", "--", src], { stdout: "ignore", stderr: "ignore" });
    hit = r.exitCode === 0; // 0 = ignored, 1 = not ignored, other = no git / not a repo
    ignoreCache.set(src, hit);
  }
  return hit;
}

function toPosix(p: string) { return p.replace(/\\/g, "/"); }

let bySrc = new Map<string, Doc>();

/** Rewrite one link target found in file `from` (repo-relative). */
function rewrite(target: string, from: string): string | null {
  if (/^[a-z][a-z0-9+.-]*:/i.test(target) || target.startsWith("//")) return target; // absolute URL, mailto:
  if (target.startsWith("#")) return target; // same page
  const [pathPart, hash = ""] = target.split("#");
  const anchor = hash ? "#" + hash : "";
  const resolved = posix.normalize(posix.join(posix.dirname(from), pathPart));
  if (isExcluded(resolved)) return null; // not published: the caller drops the link, keeps its text
  const doc = bySrc.get(resolved);
  if (doc) return `/docs/${doc.slug}/${anchor}`;
  if (resolved.startsWith("docs/assets/")) return "/assets/" + resolved.slice("docs/assets/".length);
  if (resolved === "README.md") return "/" + anchor;
  const abs = join(REPO, resolved);
  const isDir = pathPart.endsWith("/") || (existsSync(abs) && statSync(abs).isDirectory());
  return (isDir ? GITHUB_TREE : GITHUB) + resolved.replace(/\/$/, "") + anchor;
}

/** Rewrite markdown links and images, skipping fenced code blocks and inline code. */
function rewriteLinks(md: string, from: string): string {
  const out: string[] = [];
  let fence: string | null = null;
  for (const line of md.split("\n")) {
    const f = line.match(/^\s*(```|~~~)/);
    if (f) {
      if (fence === null) fence = f[1];
      else if (f[1] === fence) fence = null;
      out.push(line);
      continue;
    }
    if (fence !== null) { out.push(line); continue; }
    // split on inline code so `[x](y)` inside backticks is left alone
    const parts = line.split(/(`+[^`]*`+)/);
    out.push(parts.map((p, i) => (i % 2 ? p : p
      // a link to an unpublished file becomes its plain text
      .replace(/(?<!!)\[([^\[\]]*)\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g, (m, text, t) => (rewrite(t, from) === null ? text : m))
      .replace(/(\]\()([^)\s]+)((?:\s+"[^"]*")?\))/g, (_m, a, t, b) => a + (rewrite(t, from) ?? "#") + b)
      .replace(/(\b(?:src|href)=")([^"]+)(")/g, (_m, a, t, b) => a + (rewrite(t, from) ?? "#") + b)
    )).join(""));
  }
  return out.join("\n");
}

function yamlString(s: string) { return JSON.stringify(s); }

export type PluginState = "on" | "off" | "none";
export interface ProfileEntry { name: string; state: PluginState; reason: string }

/** Drops "(docs/sessions/...)"-style citations of files the site does not publish. */
function unpublishedRefs(s: string) {
  return s.replace(/\s*\(((?:docs\/)?[^()\s]+\.md)(?::\d+)?\)/g, (m, f) => (isExcluded(f.startsWith("docs/") ? f : "docs/" + f) ? "" : m));
}

/** installer/profile.txt: `<name> <1|0|-> <reason>` per line; `#` starts a comment line. */
export function parseProfile(text: string): ProfileEntry[] {
  const out: ProfileEntry[] = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const m = line.match(/^(\S+)\s+([10-])(?:\s+(.*))?$/);
    if (!m) throw new Error(`sync-docs: installer/profile.txt: cannot parse line: ${line}`);
    out.push({ name: m[1], state: m[2] === "1" ? "on" : m[2] === "0" ? "off" : "none", reason: unpublishedRefs((m[3] ?? "").trim()) });
  }
  return out;
}

function syncInstaller() {
  const ps1 = join(REPO, "installer/install.ps1");
  if (existsSync(ps1)) {
    const bytes = readFileSync(ps1); // a Buffer, written back as is: CRLF and ASCII kept
    writeFileSync(join(SITE, "public/install.ps1"), bytes);
    console.log(`sync-docs: installer/install.ps1 -> public/install.ps1 (${bytes.length} bytes)`);
  } else console.warn("sync-docs: installer/install.ps1 missing; public/install.ps1 left as it is");

  const prof = join(REPO, "installer/profile.txt");
  if (!existsSync(prof)) { console.warn("sync-docs: installer/profile.txt missing; src/data/profile.json left as it is"); return; }
  const plugins = parseProfile(readFileSync(prof, "utf8"));
  writeFileSync(join(SITE, "src/data/profile.json"), JSON.stringify({ source: "installer/profile.txt", plugins }, null, 2) + "\n");
  const on = plugins.filter((p) => p.state === "on").map((p) => p.name);
  console.log(`sync-docs: installer/profile.txt -> src/data/profile.json (${plugins.length} plugins; on: ${on.join(", ")})`);
}

function main() {
  if (!existsSync(REPO)) {
    console.log(`sync-docs: ${REPO} not found; using the committed copies (src/content/docs/, public/install.ps1, src/data/profile.json)`);
    return;
  }
  syncInstaller();
  // docs/*.md the list above does not know yet are still published, at the end of the modders group
  for (const f of readdirSync(join(REPO, "docs")).sort()) {
    const src = "docs/" + f;
    if (!f.endsWith(".md") || f === "README.md" || DOCS.some((d) => d.src === src)) continue;
    if (isExcluded(src)) { console.log(`sync-docs: ${src} not published (agent report or gitignored)`); continue; }
    DOCS.push({ src, slug: f.replace(/\.md$/, ""), group: "modders", order: 50, summary: "" });
    console.log(`sync-docs: new doc ${src} added (give it a summary in scripts/sync-docs.ts)`);
  }
  bySrc = new Map(DOCS.map((d) => [d.src, d]));
  rmSync(OUT, { recursive: true, force: true });
  mkdirSync(OUT, { recursive: true });
  mkdirSync(ASSETS, { recursive: true });

  const manifest: Record<string, unknown>[] = [];
  for (const d of DOCS) {
    const abs = join(REPO, d.src);
    if (isExcluded(d.src)) { console.warn(`sync-docs: ${d.src} is not published (agent report or gitignored), skipped`); continue; }
    if (!existsSync(abs)) { console.warn(`sync-docs: missing ${d.src}, skipped`); continue; }
    const raw = readFileSync(abs, "utf8").replace(/\r\n/g, "\n");
    const mtime = statSync(abs).mtime.toISOString().slice(0, 10);
    let title: string;
    let body: string;
    if (d.code) {
      title = d.code.title;
      body = "```" + d.code.lang + "\n" + raw.replace(/\n$/, "") + "\n```\n";
    } else {
      const m = raw.match(/^#\s+(.+)\n/m);
      title = m ? m[1].trim() : d.slug;
      body = m ? raw.replace(m[0], "") : raw;
      body = rewriteLinks(body, d.src);
      for (const a of d.append ?? []) {
        const p = join(REPO, a.src);
        if (!existsSync(p) || isExcluded(a.src)) continue;
        const name = posix.basename(a.src);
        body += `\n\n## Source: \`${name}\`\n\n[${a.src} on GitHub](${GITHUB + a.src})\n\n` +
          "```" + a.lang + "\n" + readFileSync(p, "utf8").replace(/\r\n/g, "\n").replace(/\n$/, "") + "\n```\n";
      }
    }
    const fm = [
      "---",
      `title: ${yamlString(title)}`,
      `summary: ${yamlString(d.summary)}`,
      `group: ${d.group}`,
      `order: ${d.order}`,
      `source: ${yamlString(d.src)}`,
      `sourceUrl: ${yamlString(GITHUB + d.src)}`,
      `updated: ${yamlString(mtime)}`,
      "---",
      "",
    ].join("\n");
    // the site is v3extender.pages.dev; repo docs that still name the old domain get the new one
    body = body.replace(/\bv3x\.pages\.dev\b/g, "v3extender.pages.dev");
    writeFileSync(join(OUT, `${d.slug}.md`), fm + body.replace(/^\n+/, ""));
    manifest.push({ slug: d.slug, src: d.src, updated: mtime });
  }

  const assetDir = join(REPO, "docs/assets");
  let n = 0;
  for (const f of readdirSync(assetDir)) {
    if (!f.endsWith(".svg") || isExcluded("docs/assets/" + f)) continue;
    copyFileSync(join(assetDir, f), join(ASSETS, f));
    n++;
  }
  writeFileSync(join(SITE, "src/data/sync.json"), JSON.stringify({ syncedAt: new Date().toISOString(), repo: toPosix(REPO), docs: manifest }, null, 2) + "\n");
  console.log(`sync-docs: ${manifest.length} docs -> src/content/docs/, ${n} svgs -> public/assets/`);
}

main();
