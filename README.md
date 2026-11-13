# v3x-site

The website for V3X (Victoria 3 eXtender), published at https://v3extender.pages.dev (Cloudflare Pages).
Astro, static output, Tailwind CSS v4. A small inline script adds motion (scroll reveal, counters,
copy buttons, sticky nav state); every page reads fully without JavaScript and honours
`prefers-reduced-motion`. This is the orphan branch `pages` of the V3X repository.

## Build

```sh
bun install
bun run build        # sync docs from the V3X repo, astro build, check links
```

`bun run build` runs, in order:

1. `scripts/sync-docs.ts`: copies the V3X docs (`docs/*.md`, `templates/plugin_template/`, `include/v3x.h`)
   into `src/content/docs/` and `docs/assets/*.svg` into `public/assets/`, rewriting relative links.
   Source: `$V3X_REPO`, default `D:/SteamLibrary/steamapps/common/Victoria 3/v3x`. If that path does
   not exist (CI), the committed copies are used. New `docs/*.md` files are picked up automatically;
   give them a group and summary in the `DOCS` list.
2. `astro build` into `dist/` (Tailwind through `@tailwindcss/vite`, prose through `@tailwindcss/typography`).
3. `scripts/check-links.ts`: every internal link and #anchor in `dist/` resolves; no script, font,
   style or image from another origin; one h1, title, description per page; sitemap entries exist.

`bun run build:nosync` skips step 1 (Cloudflare builds with it). `bun run og` redraws the share cards
in `public/og/`.

## Deploy (not automated)

```sh
bun run build
bunx wrangler pages deploy dist --project-name v3extender
```

Needs a Cloudflare login (`bunx wrangler login`) on the machine that deploys.

## Layout

| path | what |
|---|---|
| `src/styles/global.css` | Tailwind entry: `@theme` palette (from the repo's brand SVGs), fonts, components, motion |
| `src/layouts/Base.astro` | sticky nav, footer, SEO meta, hreflang, JSON-LD, the inline motion script |
| `src/components/Landing.astro`, `HeroArt.astro`, `src/i18n/landing.ts` | front page, English and pt-BR text |
| `src/components/InstallPage.astro` | /install/ and /pt-br/instalar/ |
| `src/components/DocsShell.astro`, `DocsNav.astro` | three-column docs layout (nav, prose, on-page TOC) |
| `src/pages/guide.astro`, `src/pages/pt-br/guia.astro` | players' guide |
| `src/pages/docs/` | docs index and one page per synced document |
| `src/data/facts.ts` | numbers, install one-liner and plugin defaults the pages print (from the repo docs) |
| `public/assets/` | brand SVGs synced from the repo (logo, diagrams, mockups, icons) |
| `public/fonts/` | self-hosted Inter (variable, opsz) and JetBrains Mono (variable), OFL licences alongside |
