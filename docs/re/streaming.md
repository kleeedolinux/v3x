# RE: asset/resource management and streaming (Victoria 3 1.15.1)

Worker: asset-streaming. Static analysis only; the game was not launched. RVAs are relative to the image base.
Tags: **[V]** = seen in the disassembly or the data files. **[I]** = inferred, not checked at runtime.
Tools: `tools/re_stream_lib.py` (dis with skipdata + string annotation, `xs` = strings/calls in a function, `str`,
`callers`), `tools/re_stream_flags.py` (flag argument at every call to a function), `tools/re_stream_vt.py`
(RTTI class of a vtable slot), `tools/re_stream_assets.py` (texture/mesh memory per category),
`tools/re_stream_check.py` (offline anchor check of `plugins/asset_stream`).

Goal (lead): CK3-1.20-style streaming. CK3 saved >1 GB RAM and 33-50% VRAM by streaming meshes and illustrations and
by shrinking animation metadata (1200 MB -> 12 MB). Measured here: Empty->Game 199.7 s, peak ~9-10 GB private memory,
mesh pass 37.5 s (2118 items).

## 1. Engine map

There is **no generic resource manager**: no class named `*ResourceManager*`, `*AssetManager*` or `*Streaming*` in
RTTI [V]. Each asset kind has its own loader and cache:

| kind | source file(s) | entry points (1.15.1) | eager or lazy |
|---|---|---|---|
| texture get-or-load | `pdx_gfx2/gfx_texture.cpp`, `gfx_texture_loader.cpp`, `gfx_dds_loader.cpp` | **GetOrLoadTexture `0x3c5b4f0`**(out, path, flags): finds `path` in the global cache map `0x5421820` (`0x3c7f100`), otherwise calls **CreateTexture `0x3c5af20`**(out, path, ?, flags) and inserts the result (`0x7f1fa0`). 92 call sites. [V] | on request [V]. Who requests at startup is listed below. |
| texture streaming | `pdx_gfx2/gfx_texture_streamer.cpp` | CGfxTextureStreamer = master context + 0x10. Ctor `0x3c60930`, **Start `0x3c613e0`**, Add `0x3c615a0` (log "Texture: {} was added with {} Mips on update {}"), Remove `0x3c617b0`, tasks "TextureStreaming" / "TextureUpdateRequests" (task fns `0x3c62960`, `0x3c62460`) [V] | **off**: only started when the Graphics setting `texture_streaming` is on (section 2) |
| 3D types (meshes) | `pdx_graphics/pdxmeshtype.cpp`, `pdx_mesh_asset.cpp`, `pdxassetutil.cpp`, `mesh2/pdx_mesh2_file.cpp` | `NPdxMesh::CMeshType` vtable `0x4ace220`, slot +0x30 = **Load `0x3adcfd0`** [V via RTTI]. Driven by IterateTypesThreaded `0x3ae4d80` (thunk `0x2322020` = `jmp [rax+0x30]`). Owner: worker re-loading. | **eager**: every `pdxmesh` of every `.asset` (section 3) |
| mesh material textures | `pdxassetutil.cpp` | `0x3ad09a0` calls CreateTexture with flags `0xa` or `8` (bit 3 = streamable). Chain from Load [V]: `0x3adcfd0` -> `0x3ad4ac0` -> `0x3ad17a0` -> `0x3ad12e0` -> `0x3ad0d70` -> `0x3ad7110` -> `0x3ad09a0` | eager, inside CMeshType::Load |
| skeletal animation | `pdx_anim/pdx_anim*.cpp`, `pdx_animhelper.cpp` | loaded from CMeshType::Load (strings "Failed to load animation for asset "{}"...", "Animation [{}] is not compatible with mesh [{}]" in `0x3adcfd0`) [V] | eager with the mesh type |
| entities / particles | `clausewitzlib/graphics/assetfactory.cpp`, `pdx_entity.cpp` | entity parser `0x3793db0` (also reaches `0x3ad09a0` through `0x3793af0`) [V] | eager at database load [I] |
| GUI textures/sprites | `pdx_gui/pdx_gui_texture.cpp`, `pdx_gui_sprite_texture.cpp` | through GetOrLoadTexture [I: the flags of most of the 92 call sites come from registers and were not resolved; immediates seen are 0, 1, 2] | on widget creation [I] |
| portraits | `jomini/modules/portraits/*` (portraitmanager.cpp, portrait_info_cache.cpp, portraitaccessories.cpp) | strings `GetPortraitTextureFromDna`, `PurgePortraits`, `Portraits.RebuildEveryFrame`; portrait textures are rendered on demand into a cache [I]. Portrait **meshes** are ordinary CMeshTypes (eager). | render: lazy [I]; meshes: eager [V by count] |
| coat of arms | `jomini/modules/coat_of_arms_gui/coat_of_arms_graphics.cpp` | atlases from `common/coat_of_arms/options/atlases.txt` ("Cache-size list should have a single entry per texture quality setting") [V] | atlases allocated up front [I] |
| shaders | `pdx_gfx2/gfx_shadercache.cpp` | `preload_shader_list.txt`, `-gfxdebug_disable_shader_preload` (graphics.md section 3) [V] | preloaded at startup [V string, I behaviour] |

`gfx/load_settings.settings` [V] already drops top mips per folder by `texture_quality` (array = mips skipped at
low/medium/high/ultra; `gfx/models` -2/-1/0/0, `gfx/map/terrain` -3/-2/-1/0, `gfx/portraits` 0). Only `.dds` is
affected; `gfx/interface` is not listed, so GUI textures always load at full size.

## 2. The engine's own texture streamer (verified chain)

1. Settings `texture_streaming` (bool, registration `0x60fff0`, setting global `0x5c63828`) and
   `texture_streaming_cache` (MB, registration `0x610100`, global `0x5c61b38`, value range 0x80..0x1000 = 128..4096
   [I: read from the registration stores]) [V].
2. They are read **only** in the renderer setup `0x3783730` (other xrefs: the registrations, and `0x45397c0` / `0x4539800` in `.text` outside any `.pdata` function, probably static destructors [I])
   [V]. It fills a descriptor on the stack: `+0x00` renderer, `+0x10` texture_quality, **`+0x14` streaming (u8)**,
   **`+0x18` cache bytes = MB << 20** (default before the read: `0x8000000` = 128 MB), and passes it to `0x3c375b0`
   -> `0x3c3efc0` -> **gfx master-context init `0x3c3fdc0`** (rdx; saved in r13) [V].
3. Gate at `0x3c407af` [V]: `if (desc[0x14]) Start(ctx+0x10, desc[0x18])`.
4. Start `0x3c613e0` [V]: `enabled [+0x1b1] = 1`, `budget [+0x1a0] = bytes`, starts both tasks.
5. CreateTexture `0x3c5af20` [V]: `streamed = [ctx+0x1c1] (= streamer enabled) && (flags & 8)`; streamed textures get
   load-option bit 0 and are registered with Add after creation. The update task compares resident + requested bytes
   with `[+0x1a0]` and evicts above budget (`0x3c6254d`, `0x3c627b7`) [V].
6. `game/compound_settings.txt` forces `texture_streaming` `enabled = no` in low/medium/high [V]; the lead's
   `pdx_settings.json` has only vsync/render_scale/quality [V], so streaming is off.

Not verified: whether the DDS loader reads fewer mips when option bit 0 is set (so whether streaming also cuts load time
and file I/O), which mips the streamer picks (UV factors: `Mesh.TextureStreaming`, `CalculateTextureStreamingUvFactors`
suggest on-screen size), and why Paradox disables it in every preset (pop-in? bugs?).

## 3. Memory per category (from files, `tools/re_stream_assets.py ../game --quality medium [--dlc]`)

Resident size of the full mip chain (= GPU allocation; on the lead's iGPU this is system RAM/commit), then after the
load_settings mip skip at medium. Base game / with all DLC folders:

| category | dds | full MB | medium MB | .mesh MB | pdxmesh types | loaded |
|---|---|---|---|---|---|---|
| portrait models | 2176 | 1134 | 364 | 527 | 724 | eager (mesh pass) |
| military models | 687 / 691 | 979 / 983 | 261 / 262 | 172 / 173 | 296 / 297 | eager |
| building models | 1243 / 1892 | 355 / 452 | 100 / 125 | 158 / 371 | 475 / 739 | eager |
| other models (+environment, papermap, ui) | 493 / 631 | 349 / 494 | 192 / 332 | 58 / 109 | 250 / 343 | eager |
| debug models (`gfx/models/debug`) | 91 | 76 | 19 | 48 | 11 | eager, never used in play |
| map (+terrain) | 331 / 341 | 1334 / 2121 | 494 / 1281 | - | - | eager [I] |
| GUI icons | 3323 | 722 | 722 (uncompressed RGBA) | - | - | on demand [I] |
| GUI other + frontend | 791 | 634 | 634 | - | - | on demand [I] |
| loading screens (3840x2160 RGBA) | 10 / 22 | 348 / 487 | same | - | - | one at a time [I] |
| illustrations / event / state / unit pictures | 248 | 435 | same | - | - | on demand [I] |
| coat of arms emblems | 1038 | 421 | 123 | - | - | on demand into atlases [I] |
| total | 11682 / 12838 | 7181 / 8730 | 3876 / 5343 | 963 / 1228 | 1756 / 2114 | |

- **The mesh pass loads every declared mesh type**: 2114 `pdxmesh` blocks (base + all DLC folders) vs 2118 items in
  the log [I: count match; the lead's DLC set is not known]. So every model texture is resident after load: about
  **1.1 GB at medium (3.1 GB full) plus 1.2 GB of mesh files** before decompression [I].
- CoA atlases (`atlases.txt`): 181 MB as RGBA8 [I: format], incl. a 768x512 atlas used only by `coa_export`.
- Animations: 605 `.anim` / 28.8 MB on disk in base `gfx`. There is no 1.2 GB-class animation metadata on disk; any
  CK3-like blow-up would be in-memory expansion, which needs a live measurement (alloc_tracker) [I].
- `.bk2` videos (1.19 GB, event "pictures" are mostly videos) are streamed by Bink from disk [I].

## 4. Design: per category (ranked)

Score = RAM saved / load time saved / risk. "Hook" = where the deferral goes.

| # | item | hook point | placeholder | RAM | load time | risk |
|---|---|---|---|---|---|---|
| 1 | **Force the engine texture streamer** (implemented, section 6) | master init `0x3c3fdc0`, descriptor +0x14/+0x18 | engine: lower mips | up to ~0.6 GB at medium with a 512 MB budget (model textures 1.1 GB) [I] | 0..some s if the loader skips mips [I] | low-medium: engine code, but disabled by Paradox in all presets |
| 2 | Data recipe: `load_settings.settings` (content pack) | none (data) | - | medium -> low skip for `gfx/models`: ~1.1 GB -> ~0.3 GB [I] | small | low; it is a quality trade, not streaming |
| 3 | **Never load debug mesh types** (`gfx/models/debug`, 11 types) | CMeshType::Load `0x3adcfd0`, filter by asset path | engine fallback `gfx/models/mesh_not_found_fallback_object.mesh` | ~67 MB | <1 s | low, but the hook is on re-loading's path. Keep `gfx/models/debug/no_normal.dds`: the spline styles use it [V] |
| 4 | **Lazy portrait mesh types** (724 types, 527 MB mesh, 364 MB tex at medium) | split CMeshType::Load: register type, defer load to first portrait use. Name lookups are inlined at ~19 sites (re-loading), so defer at the portrait accessory resolution (`portraitaccessories.cpp`) instead of the lookup | invisible/neutral accessory | ~0.9 GB [I] | ~15-20 s of the 37.5 s mesh pass (41% of types, 55% of mesh bytes) [I] | high: portraits are built on worker tasks; loading on demand inside them must not deadlock the task scheduler |
| 5 | Lazy unit/building/map-object mesh types | same as 4, at map-object / unit entity creation (not mapped yet) | fallback mesh | ~0.6-0.9 GB [I] | ~10-15 s [I] | high (map objects are batched/instanced) |
| 6 | Shrink CoA atlases | data: `atlases.txt` tile counts | engine re-renders | up to ~100 MB | 0 | low-medium (atlas overflow = flicker) |
| 7 | GUI textures already on demand; add eviction of unused cache entries | GetOrLoadTexture `0x3c5b4f0` / cache map `0x5421820` | reload on next request | unknown: needs a live count of cache entries | 0 | medium: the cache ownership/refcount rules are not RE'd |
| 8 | Shader preload | `preload_shader_list.txt` | compile on first use | small | unknown | stutter; not recommended |

Thread-safety rules for any own deferral: the D3D11 device (`CreateTexture2D`, buffer creation) is free-threaded, the
immediate context is not. Creating resources on a loader task is fine; anything that touches the context
(`UpdateSubresource`, copies, mip uploads) must go through the engine's render-thread path, the way the streamer does
with its "TextureUpdateRequests" task. Texture objects are refcounted (`lock xadd [obj+8]` in `0x3c5af20` / `0x3c5b4f0`) [V], so a placeholder
must be a real engine object, not a NULL handle.

## 5. Never-needed audit (lead request)

| candidate | where loaded (anchor) | size / time | can a hook skip it safely? |
|---|---|---|---|
| debug models `gfx/models/debug` | CMeshType::Load `0x3adcfd0` (every `.asset` under gfx) | 11 types, 48 MB mesh + 19 MB tex at medium | yes, by path prefix (item 3). Not the textures (`no_normal.dds` is used). |
| editor/tool modules (map editor, entity editor, GUI animation editor, node editor) | compiled in (`jomini/modules/tools_core`, `map_editor`); data in `game/tools` (8.9 MB), `gfx/editor_gui`, `gfx/tools`, `gfx/node_editor` | small (MBs) | probably already lazy (textures such as `gfx/node_editor/unknown.dds` come from GetOrLoadTexture call sites inside the editors, e.g. `0x34541d0`) [I]. Not worth a hook until a live trace shows them at startup. |
| other languages | `pdx_localize.cpp` loader (`l_english` xrefs `0x3dad2b6`, `0x3dad9a0`; `localization/languages.yml` at `0x3dab3c4`) | 11 languages x ~5-7 MB text = ~60 MB on disk; whole localization phase ~5 s | unknown whether other-language files are parsed or only filtered by header [I]. At most a few s and tens of MB. |
| DLC not owned/active | DLC folders are mounted by `dlc.cpp` (`0x3b6d500`, docs/re/vfs.md) | 3.1 GB of DLC folders on disk | an inactive DLC is not mounted, so its assets are not loaded [I]. Never touch ownership. Gain expected ~0. |
| `gfx/unit_illustrations/unused` (24 MB), `gfx/event_pictures/*.unused` | only if referenced | 24 MB | not referenced = never loaded [I]. Gain 0. |
| mips above the quality setting | engine already skips via `load_settings.settings` [V] | - | GUI (`gfx/interface`, 2.6 GB uncompressed on disk) is never mip-skipped, but it loads on demand [I]. |
| LODs above `mesh_lod_bias` | CMeshType::Load loads all LODs [I] | part of 1.2 GB mesh | needs the mesh2 format; high risk. |
| portrait/CoA variants never used | portrait accessory meshes (724 types) are all loaded although one game uses a subset of cultures | ~0.9 GB [I] | = item 4 (needs lazy load, not a skip: any culture can appear). |
| animation metadata | inside CMeshType::Load | 28.8 MB `.anim` on disk | no CK3-size problem visible on disk; measure live first [I]. |
| CoA export atlas 768x512 | `atlases.txt` (`coa_export` console only) | 1.5 MB | yes (data), negligible. |

Conclusion: Vic3 has no single "never needed" block of CK3's size. The big items are the eager mesh pass (portrait
and unit meshes with their textures, ~2 GB incl. textures at medium) and model textures that the streamer would trim.

## 6. plugins/asset_stream (piece 1, built, offline-verified)

- `asset_stream.c`: hooks the master init (`0x3c3fdc0`). Before the original runs it sets descriptor `+0x14 = 1` and
  `+0x18 = cache_mb << 20`, then logs the streamer state read back from `ctx+0x10` (`+0x1b1` enabled, `+0x1a0` budget).
  Console command `assetstream`. The engine's own console has `Texture.Streamer.Info/Pause/Unpause/FullyLoadAndPause`.
- Config `binaries/v3x/asset_stream.ini` (`asset_stream.ini.example`): `enabled = 0` (default: nothing installed),
  `texture_streaming = 1`, `cache_mb = 512` (clamped to 128..4096; 0 = keep the setting's value).
- Fail closed (`as_resolve.c`): the anchor string has exactly one xref at fn+0x23; the prologue bytes include
  `mov r13,rdx; mov r12,rcx`; the gate pattern is unique and inside that function; the Start call target is a
  function start whose head writes `[rcx+0x1b1]` and whose +0xa9 writes `[rdi+0x1a0]`. A descriptor whose streaming
  byte is not 0/1, or whose cache is not a whole number of MB in 1..65536, is left untouched.
- Offline checks: `python tools/re_stream_check.py ../binaries/victoria3.exe` (ALL OK) and `test/asset_stream_test.c`
  (parser, clamp, descriptor logic, built-in ini == example, real-exe resolve, end to end through the real
  `v3x_plugin_load` with a fake hook and a fake engine gate).
- Not built into `build/`, not installed, not run in game.

In-game test protocol: enable, start the game, check the `asset_stream` line in v3x.log (`streamer enabled=1`), run
`Texture.Streamer.Info` in the engine console, and compare peak private memory/commit and Empty->Game time against a
run with `enabled = 0` (same settings, 3 runs each). Watch for blurry or popping models.

## Not verified

- Anything at runtime: memory savings, load-time effect, visual quality, stability with streaming on.
- Whether the loader reads fewer mips for streamed textures (option bit 0).
- That the 2118 mesh-pass items are exactly the 2114 counted `pdxmesh` blocks (depends on the lead's DLC set).
- Which textures GUI code requests at startup, and whether the texture cache map keeps textures alive forever.
- Localization: whether non-selected languages are parsed.
