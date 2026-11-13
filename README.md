# v3x-site

The website for V3X, published at https://v3x.pages.dev (Cloudflare Pages). Astro, static output,
no client JavaScript. This is the orphan branch `pages` of the V3X repository.

## Build

```sh
bun install
bun run build        # sync docs from the V3X repo, check tokens, astro build, check links
```

`bun run build` runs, in order:

1. `scripts/sync-docs.ts`: copies the V3X docs (`docs/*.md`, `templates/plugin_template/`, `include/v3x.h`)
   into `src/content/docs/` and `docs/assets/*.svg` into `public/assets/`, rewriting relative links.
   Source: `$V3X_REPO`, default `D:/SteamLibrary/steamapps/common/Victoria 3/v3x`. If that path does
   not exist (CI), the committed copies are used. New `docs/*.md` files are picked up automatically;
   give them a group and summary in the `DOCS` list.
2. `scripts/check-tokens.ts`: no colour literal outside `src/design/tokens.ts`; contrast floors.
3. `astro build` into `dist/`.
4. `scripts/check-links.ts`: every internal link and #anchor in `dist/` resolves; no non-JSON-LD
   script, no external fonts/styles/images; one h1, title, description per page; sitemap entries exist.

`bun run build:nosync` skips step 1. `bun run og` redraws the share cards in `public/og/`.

## Deploy (not automated)

```sh
bun run build
bunx wrangler pages deploy dist --project-name v3x
```

Needs a Cloudflare login (`bunx wrangler login`) on the machine that deploys.

## Layout

| path | what |
|---|---|
| `src/design/tokens.ts` | every colour, size and space value; generates the CSS variables and the /design/ page |
| `src/styles/site.css` | all styles, tokens only |
| `src/layouts/Base.astro` | masthead, footer title block, SEO meta, hreflang, JSON-LD |
| `src/components/Landing.astro`, `src/i18n/landing.ts` | front page, English and pt-BR text |
| `src/pages/guide.astro`, `src/pages/pt-br/guia.astro` | players' guide |
| `src/pages/docs/` | docs index and one page per synced document |
| `src/data/facts.ts` | numbers and plugin defaults the landing and guide print (from the repo docs) |
| `public/fonts/` | self-hosted Old Standard TT, Source Serif 4, IBM Plex Mono (OFL, licences alongside) |
