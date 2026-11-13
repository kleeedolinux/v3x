/**
 * Design rules that a script can check (see /design/ section 10):
 *  1. no colour literal (#rgb, #rrggbb, rgb(), hsl()) in src/ outside src/design/tokens.ts;
 *  2. every text colour token meets its contrast floor on its allowed backgrounds, in both schemes.
 *
 *   bun scripts/check-tokens.ts
 */
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative } from "node:path";
import { contrastTable } from "../src/design/tokens";

const SRC = join(import.meta.dir, "../src");
const ALLOW = new Set(["design/tokens.ts"]);
const errors: string[] = [];

function walk(dir: string, out: string[] = []) {
  for (const f of readdirSync(dir)) {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) { if (f !== "content") walk(p, out); } // synced docs are prose, not styling
    else if (/\.(astro|css|ts|tsx|js|mjs)$/.test(f)) out.push(p);
  }
  return out;
}

for (const f of walk(SRC)) {
  const r = relative(SRC, f).replace(/\\/g, "/");
  if (ALLOW.has(r)) continue;
  const lines = readFileSync(f, "utf8").split("\n");
  lines.forEach((line, i) => {
    // a hex colour: # followed by 3, 4, 6 or 8 hex digits, not part of a URL fragment or an id like #faq
    for (const m of line.matchAll(/(?<![\w&/])#([0-9a-fA-F]{3,8})\b/g)) {
      if (![3, 4, 6, 8].includes(m[1].length)) continue;
      errors.push(`${r}:${i + 1}: colour literal ${m[0]}`);
    }
    for (const m of line.matchAll(/\b(rgba?|hsla?)\(\s*\d/g)) errors.push(`${r}:${i + 1}: colour function ${m[0]}`);
  });
}

let pairs = 0;
for (const row of contrastTable()) {
  pairs++;
  if (row.ratio < row.min) errors.push(`contrast ${row.scheme}: ${row.fg} on ${row.bg} = ${row.ratio.toFixed(2)} < ${row.min}`);
}

if (errors.length) {
  for (const e of errors) console.error("  FAIL " + e);
  console.error(`check-tokens: ${errors.length} problem(s)`);
  process.exit(1);
}
console.log(`check-tokens: no colour literals outside tokens.ts; ${pairs} contrast pairs meet their floor`);
