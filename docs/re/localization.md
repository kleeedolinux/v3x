# Localization loading and the loc_fallback plugin (Victoria 3 1.15.1)

Scope: how the engine finds localization files, what that means for an English fallback, and how
`plugins/loc_fallback` works. Offline cross-check: `tools/re_loc_fallback.py`; test: `test/loc_fallback_test.c`.

## 1. Engine facts (static evidence)

| Fact | Evidence |
|---|---|
| Localization is read from the VFS folder `localization` (recursive) and filtered by extension `.yml`; `localization/replace` is special (overrides existing keys). | exe .rdata strings next to each other: `localization` 0x469b010, `.yml` 0x469b438, `localization/replace` 0x469b440 (dump: `tools/pestrings.py ... 3`). |
| The language of an entry comes from the header line (`l_xxx:`), not from the folder. | vanilla `game/localization/modifiers/` holds all 11 languages in one folder; `languages.yml` has 11 `l_xxx:` blocks in one file. |
| Language ids and folders: `l_english, l_braz_por, l_french, l_german, l_polish, l_russian, l_spanish, l_simp_chinese, l_japanese, l_korean, l_turkish`; folder = id without `l_`. | `game/localization/languages.yml`, folder names in `game/localization/`, `launcher/settings-layout.json`. |
| Duplicate key in normal files: the first one loaded is kept (`Duplicated key {} will not be created from file: {}:{}` 0x469d890; `pdx_localize.cpp:199/231` in debug.log). Keys in `replace/` win. | strings + `logs/debug.log`. |
| Only the active language is loaded: in a session the duplicate-key warnings name only `localization/english/` files, while the mod's `braz_por` file has the same duplicates. | `logs/debug.log` vs `3194757702/localization/{english,braz_por}/advcm_*.yml` (6 copies of `SAKUYA_TEXT_ADMIN_POWER_1` in each). |
| Other search roots with localization: `clausewitz/localization` (cw_*), `jomini/localization`. DLC folders (`game/dlc/*`) have no localization in 1.15.1. | directory listing. |

Not verified in code (inferred): the exact filename rule (whether `_l_<lang>.yml` is required besides the
header). The generated file satisfies both: name `zz_v3x_fallback_l_<lang>.yml`, header `l_<lang>:`, UTF-8 BOM.

## 2. IMPORTANT: the game currently ignores `l_braz_por`

Every `debug.log` since crash snapshot `01261013_111337` (and all runs of 2026-11-13) starts with
`[pdx_json_settings.cpp:343] Failed to read "language" setting. Reason : ""l_braz_por" is not part of enum"`;
the snapshots `01261012_*` do not have it. The exe has no `braz_por`/`simp_chinese`/`l_turkish` strings
(only an old Clausewitz table: `l_english l_french l_german l_spanish l_polish l_russian l_portugese ...`
at RVA 0x494c8c0-0x4953040), so the game builds the enum at runtime, probably from data read during
early start. Something since 10-13 breaks that and the game falls back to English. This is NOT fixed by
loc_fallback; it needs a bisect (V3X plugins that touch early file reads, or the pdx_settings.json edit
at 2026-11-13 00:24, `pdx_settings.json.bak-20261113-002535`). `tools/re_loc_fallback.py` prints this verdict.

## 3. plugins/loc_fallback

Data only, no engine hook. In `v3x_plugin_load` (before the game's CRT/WinMain, so before vfs_init and
long before localization is read):

1. Language = `System.language` of `Documents/Paradox Interactive/Victoria 3/pdx_settings.json`
   (Documents via `SHGetFolderPathW(CSIDL_PERSONAL)`, then `%USERPROFILE%`/`%OneDrive%`), or `language=` in
   `loc_fallback.ini`. English = remove generated files, done.
2. Roots, lowest priority first (VFS mount order, docs/re/vfs.md, docs/re/modloading.md): clausewitz,
   jomini, game, other content packs (alphabetical), DLC, enabled mods in `content_load.json` order (last
   wins). Every `localization/**.yml`; the same relative path resolves to the highest root (file override).
3. Files are pre-filtered by name (`l_<lang>` in the name; files without any language in the name, e.g.
   `languages.yml`, are always read) and parsed with a tolerant parser (BOM, CRLF, `#` comments, several
   headers per file, `key:0 "v"`, `key: "v"`, `key:"v"`, value up to the last quote before blanks/comment).
   The header decides the language.
4. Mods and packs are parsed first. Only if English keys are still missing are the vanilla files of the
   language read; their key hashes are cached in `binaries/v3x/loc_fallback.cache.keys` (keyed by vanilla
   paths/sizes/mtimes), so a mod update does not re-read vanilla.
5. English keys (mods/packs; vanilla too with `also_fill_vanilla=1`) that no root defines in the language
   are written verbatim to `binaries/v3x/content_packs/v3x_loc_fallback/localization/<folder>/zz_v3x_fallback_l_<lang>.yml`
   (BOM, `l_<lang>:` header, a `# ---- <mod> (<id>)` comment per mod). Duplicate English definitions:
   `replace/` beats normal, then the higher-priority root.
6. All inputs (root paths, file paths/sizes/mtimes, language, config, format version) are hashed into
   `binaries/v3x/loc_fallback.cache`; same hash + output present = no parse at all.

Why it is safe: the file only contains keys that no file of the language defines, so it never creates a
duplicate of a real translation, and content packs sit above game/ but below mods. Stale files of other
languages are deleted. If nothing is missing no file exists (no checksum change).
Risk: when keys are filled, a new file under `localization/` is in the VFS; the checksum manifest hashes
localization (docs/re/modloading.md), so the game checksum differs like with any mod.

Console: `locfallback` (summary per mod), `locfallback rebuild` (regenerate now; used at the next start).

## 4. Measurements (2026-11-13, lead's 23 mods, read-only, `loc_fallback_test.exe <tmp> <game root> <docs> <lang> <dll>`)

| Case | Keys filled | Time |
|---|---|---|
| l_braz_por (current setting), cold | 0 (every mod with English text ships a complete braz_por file) | 36-75 ms (83 mod files parsed, vanilla skipped) |
| l_braz_por, inputs unchanged | 0 | 14-30 ms (directory walk only) |
| l_braz_por, also_fill_vanilla=1 | 7 (all vanilla) | 115-203 ms warm |
| l_german, cold (vanilla never read before, 8-thread prefetch) | 265 (CMF 102, Cheat Menu Pro 1.12 78, cheat decrees 45, Historical AI 29, others 11) | 578 ms |
| l_spanish, cold | 3583 | 756 ms |
| l_french, cold, before the prefetch | 3583 | 2954 ms (sequential cold opens, ~10 ms each) |
| mod change, vanilla from .keys cache | same | 33-47 ms |

`tools/re_loc_fallback.py --lang <id> --packs <empty dir>` gives the same counts (0 / 265 / 3583).
