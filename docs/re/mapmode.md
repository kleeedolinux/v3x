# Map-mode / GUI-view lag: texture re-reads (1.15.1)

Lead report: a 4-hour session lagged badly when opening views such as decrees and when switching map modes.
Evidence: `binaries/v3x/alloc_report.txt` (#389), `v3x.log` (frame_pacer), `profile_Game_to_Empty_2.csv` and
`profile_edges_Game_to_Empty_2.csv` (devtools sampler, about 1 kHz per sampled thread, whole Game session).
[V] = verified statically in the exe, [P] = from the profile, [I] = inference.

## 1. What the alloc_report stack really is

`site 0x84800b (fn 0x847fe0) 167 GB total, 77,119 big blocks, max 172.9 MiB`, first stack
`0x677eac <- 0x84800b <- 0x3c5b134 <- 0x3c5b551 <- 0x3c3cdfc <- 0x3c3c2b1`.

| frame | function | what it is |
|---|---|---|
| 0x677eac | allocator | engine heap alloc |
| 0x84800b | `0x847fe0` | **generic `CPdxArray<uint8>::Reserve(arr, n)`** [V]: `cmp edx,[rcx+8]; jle`, alloc through the array's allocator (vslot +8), copy, free old. **1219 direct call sites** [V]. |
| 0x3c5b134 | CreateTexture `0x3c5af20` (+0x214) | reserve of a buffer the size of the whole texture file (file vslot +0x20 = size), then read, then DDS parse `0x3c5e0d0`, then GPU create [V] |
| 0x3c5b551 | GetOrLoadTexture `0x3c5b4f0` | cache miss -> CreateTexture [V] |
| 0x3c3cdfc | `0x3c3cc30` | loads every `File =` texture sampler of a shader effect (array of 0x30-byte sampler descs, path at desc+0x28 -> +0x70, sRGB at +0x90) [V] |
| 0x3c3c2b1 | `0x3c3be90` | effect creation in `pdx_gfx2/gfx_effect.cpp` ("Could not find main code '{}'") [V] |

**Caveat:** alloc_tracker groups by call site, and the site here is inside the generic Reserve. So the 167 GB
covers all 1219 callers, and the stack shown is only the first block >= 1 MiB that was ever seen. The
172.9 MiB maximum matches none of the game's textures (the largest DDS is `flatmap*.dds`, at most 151.0 MiB). It
does fit a save-game serialization buffer: the session's autosaves are 147-149 MiB each, written every 1-3
minutes, and a growing buffer overshoots [I]. Most of the 167 GB is therefore probably *not* texture reads.
map_cache attributes every reserve of 16 MB or more by return address, which settles this split after one
session.

## 2. Why textures are re-read: the texture cache is weak [V]

- GetOrLoadTexture `0x3c5b4f0(out, path, flags)` calls find `0x3c7f100` on the global cache `0x5421820`.
  On a miss it calls CreateTexture `0x3c5af20(out, path, ?, flags)` and then inserts.
- find does `lock xadd [tex+8], 1`. **If the old count was <= 0, it treats the entry as a miss** and erases it
  (`0x3c933a0`, `0x7c0970`). The cache never holds a reference itself.
- Refcount: int at `tex+8`. On the final release, `owner=[tex+0x10]` gets vslot 1 called with (owner, tex),
  and then the texture's own vslot 1 is called with (tex). The same sequence appears at GetOrLoadTexture+0x86,
  `0x3c3ce29` and `0x3c62d3b`.
- So any texture whose users all go away (a GUI view closes, a shader effect is rebuilt, a map mode stops
  showing it) is destroyed. The next request reads the whole file from disk again, parses it and uploads it.
- The path is an MSVC `std::string` (SSO 16, size +0x10, cap +0x18), built by `0x3c5a7e0` [V].

## 3. Who requests those reads (profile, samples about ms) [P]

`tools/re_map_chain.py EDGES 0x3C5AF20` shows CreateTexture at 18,812 samples inclusive. Its callers:

| caller | samples | what |
|---|---|---|
| GetOrLoadTexture `0x3c5b4f0` | 13,156 | the cache misses |
| ... `0x32c5250` <- `0x32c9220`/`0x32c8e40` <- `0x32c64f0` (coat_of_arms_graphics.cpp) | 6,029 | **coat-of-arms rendering**: COA pattern/emblem textures re-read each time COAs are regenerated (jobs `0x3dba2d0`, map `country_coa_graphics.cpp` `0x2927ec0`, GUI `0x1b9d4a0`). Views full of flags (decrees, diplomacy, outliner) trigger it [I]. |
| ... `0x2802520` <- `0x3dee090` <- `0x27fea20` ("MapTextures", "ColormapTree") / `0x2807c20` (graphicalmap) | 4,431 | map texture set loads (tweakable-driven map textures) |
| ... `0x3c58600` <- `0x37fe920` <- GUI render (`CPdxGuiContext::Render` `0x38aa110`) | 2,138 | GUI widget textures loaded when a view is drawn |
| ... `0x3c3cc30` (effect samplers) | 8 | the effect path from the alloc stack (rare) |
| `0x3c62c60` <- `0x3c62960` <- job `0x3dfb760` | 3,967 | async in-place reload (flags \|0x28, then texture vslot +0x18), runs off the main thread |
| `0x3ad09a0` (mesh materials) | 1,689 | CMeshType::Load material textures |

Inside CreateTexture, the VFS open `0x3e1c4f0` (physfs) is the biggest part at 8,213 samples. Then come
`0x3e1c090` (5,112) and the DX11 pixel buffer upload `0x3c6ac30` (4,266). The edge table overflowed
(1,970,768 inserts dropped), so these numbers are lower bounds.

A map-mode switch also recolours every province: `SetProvinceColors` `0x2869660` (mappaintingmanager.cpp)
was 11,828 samples inclusive over the session. The profile adds up the whole session, so it cannot split
one 3.5 s frame among these. The per-spike log of map_cache is meant to do that.

## 4. Plugin map_cache (plugins/map_cache)

Hooks are resolved by anchors (mirrored in `tools/re_map_check.py`, exit code 0 = all OK):
- **create**: the function that references `"Could not find texture"`, with shape checks at +0, +0x2c
  (argument moves) and +0x204 (`cmp ebx,[rbp-0x24]; jle; mov edx,ebx; lea rcx,[rbp-0x30]; call Reserve`).
- **getload**: the direct caller of create whose call sits at +0x5c, with shape checks at +0 and +0x35
  (lea of the cache + find) and +0x51, plus **+0x86, the full refcount release sequence the plugin copies**.
- **reserve**: the rel32 target of create+0x20f, shape-checked. File size = its `n` when the return address is
  create+0x214 (0x3c5b134).
If any anchor fails, the plugin installs nothing. The test breaks one byte of getload, and separately one byte
of create, to check this.

Modes (`v3x/map_cache.ini`; with no ini the plugin runs as `measure`):
- `measure`: no behaviour change. It logs:
  - every CreateTexture >= `load_log_ms` or >= `load_log_mb` (path, MB, ms, read count, cache-miss vs direct, thread);
  - every main-thread gap >= `spike_ms` (between on_frame ticks), with the texture disk reads inside it
    (count, re-reads, MB, ms, slowest path);
  - every `summary_sec` seconds, and when leaving Game, `v3x/map_cache_report.txt`: top paths by load time,
    GetOrLoadTexture miss rate, and big reserves (>= `big_reserve_mb`) grouped by return address.
- `on`: adds a keep-alive. When GetOrLoadTexture causes the `repeat_min`-th disk read of a path, the plugin
  adds one reference to the returned texture. Limits: `max_texture_mb` (default 32, so the 151 MiB flatmaps
  are never pinned), an LRU byte budget `budget_mb` (file bytes, default 256), and `max_pins`. Cache hits on a
  pinned texture refresh its LRU slot. If the engine creates a new object for a pinned path, the pin moves to
  the new object. Pins are released with the engine's own sequence, on the main thread: over budget (each
  frame), on leaving Game, and on `mapcache flush`.
- Console: `mapcache [status|report|flush]`.

Risks of `on`:
- More resident textures. The lead has 16 GB RAM with about 1 GB free, and the budget counts file size, not
  VRAM.
- Hot-reloading a pinned texture is only handled by the pin swap.
- It assumes no engine code depends on a texture being destroyed. Nothing seen statically suggests one does
  [I].
Not verified in game.

Not done, on purpose: caching generated per-map-mode data. No reliable dirty signal was found, and the
painting path (`SetProvinceColors`) recomputes colours from game state.

## Next step
Run one session with `mode=measure`, then read `map_cache_report.txt`:
- If the spikes on map-mode/decrees show many re-reads, try `mode=on`.
- If the big-reserve table shows that the 172.9 MiB blocks come from save serialization, that is a separate
  issue: autosave frequency and buffer reuse.
