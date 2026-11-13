import type { APIRoute } from "astro";
import { getCollection } from "astro:content";
import sync from "../data/sync.json";

/** Pages with a translation: [en, pt-BR]. Others are English only. */
const pairs: [string, string][] = [["/", "/pt-br/"], ["/guide/", "/pt-br/guia/"]];

export const GET: APIRoute = async ({ site }) => {
  const s = (p: string) => new URL(p, site).href;
  const today = (sync as { syncedAt: string }).syncedAt.slice(0, 10);
  const docs = await getCollection("docs");
  const urls: { loc: string; lastmod: string; priority: string; alt?: [string, string] }[] = [];
  for (const [en, pt] of pairs) {
    urls.push({ loc: s(en), lastmod: today, priority: en === "/" ? "1.0" : "0.9", alt: [s(en), s(pt)] });
    urls.push({ loc: s(pt), lastmod: today, priority: en === "/" ? "1.0" : "0.9", alt: [s(en), s(pt)] });
  }
  urls.push({ loc: s("/docs/"), lastmod: today, priority: "0.8" });
  for (const d of docs.sort((a, b) => a.data.order - b.data.order))
    urls.push({ loc: s(`/docs/${d.id}/`), lastmod: d.data.updated, priority: d.data.group === "engine" ? "0.5" : "0.7" });
  urls.push({ loc: s("/design/"), lastmod: today, priority: "0.3" });

  const body = `<?xml version="1.0" encoding="UTF-8"?>
<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" xmlns:xhtml="http://www.w3.org/1999/xhtml">
${urls.map((u) => `  <url>
    <loc>${u.loc}</loc>
    <lastmod>${u.lastmod}</lastmod>
    <priority>${u.priority}</priority>${u.alt ? `
    <xhtml:link rel="alternate" hreflang="en" href="${u.alt[0]}"/>
    <xhtml:link rel="alternate" hreflang="pt-BR" href="${u.alt[1]}"/>
    <xhtml:link rel="alternate" hreflang="x-default" href="${u.alt[0]}"/>` : ""}
  </url>`).join("\n")}
</urlset>
`;
  return new Response(body, { headers: { "Content-Type": "application/xml; charset=utf-8" } });
};
