# RE: lazy mesh loading (Victoria 3 1.15.1)

Goal: stop loading all ~2118 pdxmesh types (`NPdxMesh::CMeshType`) at startup (measured 37.5 s wall on 8
threads, ~139 ms per type, plus a large share of the ~9 GB RAM), and load each one the first time something
is about to use it.

Binary `binaries/victoria3.exe` 1.15.1. **[V]** = verified in the disassembly (static, `tools/re_mem_lib.py`,
`tools/re_sim_lib.py`, `tools/rtti_dump.py`-style RTTI). **[I]** = inferred. Nothing here was run in-game yet.
Plugin: `plugins/lazy_mesh/`. Offline checks: `tools/re_lazymesh_check.py`, `test/lazy_mesh_test.c`.

## 1. Engine flow

| RVA | what | |
|---|---|---|
| 006aff80 | big init function; calls `0x3794de0(cl=0)` at 006b1020, the 3D type pass `0x3ab3f10` at 006b1028, then `0x3794de0(cl=1)` at 006b104a | [V] call order; [I] the flag selects the asset pass (0 = pdxmesh etc., 1 = entities) |
| 03793db0 | `CAssetFactory` vtable slot 4 (pdx assetfactory.cpp): per `.asset` block; keyword 0x180 creates a `CMeshType` (ctor 03adb0c0, `[+0x34]=0x180`), parses it (vslot 3) and registers it (03ab4010, "a 3d-type with the name ... already exists") | [V] |
| 03ab3f10 | the pass: getter 03ae4b80 -> `IterateTypesThreaded<CMeshType>` 03ae4d80 -> then for every mesh type, serially, the animation import 03ae1400 -> then 03ab39b0/03ab6c10 (compacts the blend-shape registry) -> then vslot 6 for every type of the second map (+0x28, arrows) | [V] |
| 03ae4d80 | copies all `[bucket+0x28]` type pointers into an array, `CNonBlockingJob<lambda>` over them, each item = thunk 02322020 (`mov rax,[rcx]; jmp [rax+0x30]`) | [V] |
| 03adcfd0 | `CMeshType::Load(this)` = vtable 04ace220 slot 6. Only input `rcx`. Early-out `cmp [this+0x4c],0; jne` (03add042) | [V] (contract also by re-loading, chat #4150/#4158) |
| 03ae1400 | animation import (`Could not find animation set {} declared as import in mesh type {}`): if `[+0x148]` (import list) is empty -> return; if `[+0x38]==0` (no mesh data) it skips the import **and clears the list** (03ae16a3); else imports from the global animation registry, then clears the list | [V] |
| 03adb790 | `CMeshType::Reload`: `+0x38=0`, destroy `+0x40` array, clear `+0x130`, vslot 6 (Load), refresh every registered mesh object (`+0x1e0` map, vcall +0x10), the same import as 03ae1400, 0x3b23f90. Callers: 03795380 (asset reloader, `.mesh` file watcher) and vslot 7 03adeeb0 | [V] -> calling Load after startup is an engine path |
| 03ad80a0 | `CMeshObject::CMeshObject(this, const CMeshType *type)`: stores type at `+0x50`, then reads `[type+0x38]`, `[type+0xcc]` (blend shape count), copies `type+0x1d0`. Vtable 04ace8f0 is referenced only here and in the dtor 03ad84a0, so **every** CMeshObject is built by this function | [V] |

### CMeshType fields (ctor 03adb0c0 vs. Load)

| off | content | filled by |
|---|---|---|
| +0x08 | name (MSVC std::string) | parse [V] |
| +0x34 | 3D type id (0x180 = pdxmesh) | parse [V] |
| +0x38 | mesh data pointer (0 until loaded) | Load (03add87a) [V] |
| +0x40 / +0x4c | pdx array {ptr,cap,size} of LOD/render data; size `+0x4c` is the engine's own "already loaded" test | Load (LOD setup 03adc9d0) [V] |
| +0x78, +0x90, +0xa8 | meshsettings / LOD percentage arrays | parse [I], read by Load |
| +0xc0 / +0xcc | blend shapes (0x58 bytes each) | Load [V]; registered by name in a spin-locked registry (03ae1710 -> singleton 03ab39b0) |
| +0xe8..+0x110 | LOD info, bounding sphere/box (box init FLT_MAX / -FLT_MAX) | Load [V] |
| +0x118 / +0x124 | animations | Load [V] |
| +0x130 / +0x13c | locators | Load [V] |
| +0x148 | animation imports (strings, 0x20 each) | parse [I]; consumed by 03ae1400 [V] |
| +0x198 | mesh file path (std::string) | parse [V] (used by Load's file lookup 03dffd50) |
| +0x1b8, +0x1c0, +0x1c8 | three materials (03adc630 x3) | Load [V] |
| +0x1d0 | ref-counted render data (built by 03ad7860) | ctor allocates, Load fills [V] |
| +0x1e0 | map of mesh objects using the type (Reload refreshes them) | runtime [V] |

There is no separate "loaded" flag. "Loaded" = `[+0x4c] != 0` (re-loading: a type that legitimately loads 0
entries keeps 0). lazy_mesh therefore keeps its **own** per-type state.

## 2. Consumer audit

All functions that call the registry getters 03ae4b80 / 03ae4c70 (22 + 24 call sites, 20 functions) plus the
external callers of the pdxmeshtype/pdxmeshobject API (functions in 03ad0000..03ae6000 called from
outside). "Reads loaded data" = reads a field from the table above that only Load fills.

| consumer | what it does with the type | reads loaded data? | handled by |
|---|---|---|---|
| 03ab3f10 / 03ae4d80 | the pass | - | hooks `pass`, `load`, `anim` |
| 03ab4010, 03ab4200 | register a type at parse | no | - |
| 037b1590, 037b2740 (pdx_entity.cpp, entity types, asset pass 1 = after the mesh pass [I]) | store type at `entity+0x108`. If the entity has overrides (`[+0x448]>0`) call **03ad7860(type, &entity+0x178)** + 03ad7b10: copies the `+0x40` LOD data. For blend-shape attributes call **037a9520(out, attr, type, name)**: looks the mesh up in the blend-shape registry. Reads `[type+0x124]` only for a warning ("Missing default_state ... animated mesh") | yes | hooks `rdata` (03ad7860, `rcx`) and `blend` (037a9520, `r8`); the warning is harmless |
| 037ae860 (entity instance) | `[entity+0x108]` -> CMeshObject ctor directly (037ae9ba..037ae9eb) | via ctor | hook `ctor` |
| 018ce8c0 (theme.cpp, menu backgrounds) | lookup -> CMeshObject ctor | via ctor | `ctor` |
| 0290c000 / 029129f0 / 02913960 (city_graphics.cpp) | city graphics type (02922f60) stores type pointers in arrays; buildings get CMeshObjects via 0290c000 | via ctor | `ctor` |
| 02ecbfe0 / 02ed0950 (province_map_interface, map objects) | type -> `[mapobjtype+0xb8]`; the grid builds CMeshObjects (02ed0950). No other `[+0xb8]` -> type-field read found in 02e00000..02f00000 | via ctor | `ctor` |
| 02fa9c40 | builds a CMeshObject | via ctor | `ctor` |
| **028a5d40 (route_graphics.cpp, "Missing '{}' bridge mesh")** | picks the bridge mesh by reading the bounding box `[type+0x110]-[type+0x104]` **without** a mesh object | **yes** | kept eager: `eager_contains = bridge` (bridge types are `<prefix>_bridge_NN_<size>_mesh`, defines `DIRTROAD_BRIDGES`/`RAILROAD_1_BRIDGES`) |
| 037ab6f0, 037ace50, 037af3a8 (entity attachments/locators) | read `+0x130/+0x13c` through `[meshobject+0x50]` | via an existing mesh object | `ctor` (type loaded before the object exists) |
| 03798790 (`CAnimationReloader`) | reads `+0x118/+0x124` | yes, but only re-binds animations; empty list = no-op | tolerated |
| 03795380 (asset reloader, `.mesh`) | matches `+0x198` path, then Reload | path only (parse data) | Reload of a deferred type is claimed in `h_load` |
| 02be1240, 02c9f590, 03ab44c0, 02be0330... (map_editor / nudge) | editor: store type pointers, clone | editor only | not handled (not used in normal play) |
| 03aee490 (`CTransactionResource<SMeshTypesAndArrows>` slot 2) -> 03ab3c00 -> 03adf160 (`CMeshType` copy-assign) | transactional copy of types | copies whatever state the source has | not handled; a copy is a new pointer that lazy_mesh does not track [I: editor/transactions only] |
| 0296bd50, 03ab43f0 (supply/movement arrows) | second map (+0x28), not CMeshType | no | - |

Not found statically: any other code reading CMeshType fields without a CMeshObject. The search was
heuristic (register flow after every lookup, `+0x108`/`+0xb8`/`+0x50` -> field reads in the entity and
map-object modules). A reader in code that reaches the type through some other stored pointer cannot be
excluded; that is the main residual risk (see 6).

## 3. Design (plugins/lazy_mesh)

* `pass` hook (03ab3f10) sets `g_pass_active` for the duration of the startup pass.
* `load` hook (03adcfd0), in the pass: insert the type in a lock-free table (CAS on the key; 16384 slots,
  full at 3/4 -> load eagerly), copy name/path, `lm_decide()`:
  eager file name > `eager_contains` (default `bridge`) > `skip_debug` (`gfx/models/debug/`) > `defer_dirs`
  (default `gfx/models/portraits/`, `gfx/models/military/`) > `defer_all` > eager. Deferred -> state
  DEFERRED and return without loading. Eager -> LOADING(owner) -> original Load (timed) -> DONE.
  Outside the pass (engine Reload of a deferred type) -> claim DEFERRED->LOADING, let the engine load, DONE.
* `anim` hook (03ae1400): return immediately for a DEFERRED type, so its `+0x148` import list survives.
* On demand (`lm_ensure`): DEFERRED->LOADING by CAS (one winner), winner runs original Load + original
  anim import (exactly what the pass does), DONE. Others wait (yield/Sleep(1)) until DONE or
  `wait_timeout_ms`. A request from the loading thread itself (Load calls 03ad7860 on its own type) returns
  REENTRANT immediately. Choke points: `ctor` (03ad80a0, `rdx`), `rdata` (03ad7860, `rcx`), `blend`
  (037a9520, `r8`). Unknown pointers (not in the table) pass straight through.
* `measure` mode: same hooks, nothing deferred; records per type: uses, first-use time after the pass,
  first site, load phase (from fast_load's `load_transition` event), pass load ms, and the decision lazy
  WOULD take. `v3x/lazy_mesh_used.txt` is rewritten every 30 s (if changed), at the end of the pass and on
  `lazymesh dump`. Used types are data lines (name first), unused ones `# unused` comments, so the file can
  be copied to `v3x/lazy_mesh_eager.txt` unchanged.
* `prefetch_after_s=N` (default 0): N s after the pass, a below-normal thread loads the remaining deferred
  types one by one (safety net; gives the RAM back).
* Console: `lazymesh` (status), `lazymesh dump`, `lazymesh load <type>` (loads on the console thread: a
  way to test an on-demand load off the worker pool, as re-loading suggested).
* Fail closed: every target is found by a string or RTTI anchor plus byte shapes (A[] in lazy_mesh.c,
  mirrored in `tools/re_lazymesh_check.py`): `load` = xref of `gfx/models/mesh_not_found_fallback_object.mesh`
  and it must be slot 6 of the `.?AVCMeshType@NPdxMesh@@` vtable; `anim` = xref of the animation-import
  string; `pass` = the only E8 caller of `anim` with the call at +0x64; `ctor` = the function storing the
  `.?AVCMeshObject@NPdxMesh@@` vtable at +0x71; `rdata` = the call after `lea rdx,[r13+0x1d0]; mov rcx,r13`
  inside Load; `blend` = xref of the blend-shape string. measure needs load+pass+ctor; lazy needs all 6
  **and** all 6 hooks installed before `g_armed` is set, otherwise nothing is ever deferred.
* No hook target is shared with fast_load (03ae4d80, 02322020) or asset_stream (03c3fdc0).

## 4. Threads

* At startup Load runs on the 8 IterateTypesThreaded workers while the main thread waits [V by re-loading's
  in-game timeline]. So Load must not need the main thread, and its resource creation cannot depend on the
  D3D11 immediate context without its own locking [I]. The engine's Reload calls it again outside the pool
  (thread not verified).
* On demand, Load runs on whatever thread creates the mesh object (main/interface thread for city graphics
  `Update`, map-object grid, theme, portraits/units/GUI via entities; entity parse for overrides/blend
  shapes) [I]. The blend-shape registry is spin-locked [V, 03ae17b4]; the animation registry uses an SRW
  lock [V, 03ae149a]. Material/texture creation goes through vtables (03acfcf0 via 03ad12e0->03ad0d70) and
  textures through the global GetOrLoadTexture cache (asset-streaming, chat #4172) - not proven thread-safe
  against a concurrently rendering frame. **Not verified.**
* Deadlock risk [I]: a thread that holds an engine lock L creates a mesh object -> on-demand Load needs L.
  At startup no game lock is held by the waiting main thread, so this was never exercised by the engine.

## 5. How to use

1. `python tools/re_lazymesh_check.py ../binaries/victoria3.exe` -> `ALL ANCHORS OK` (after every patch).
2. `mode = measure` in `binaries/v3x/lazy_mesh.ini`, play a normal session (menu, load a save, zoom around
   the map, open character/military windows), `lazymesh dump`. Read the header line of
   `v3x/lazy_mesh_used.txt` (types / used / what would be deferred) and the per-type `pass_ms`.
3. Copy `lazy_mesh_used.txt` to `lazy_mesh_eager.txt` (every used type stays eager), set `mode = lazy`.
   Start: `v3x.log` shows `startup pass: ... N deferred` and every `on-demand load <name> <ms>`.
4. Wider deferral: `defer_all = 1` with a good eager file (biggest gain, biggest risk).

Expected gain [I]: the pass wall time shrinks roughly with the share of deferred pass time
(`pass_ms` column; note it is thread time on 8 workers). portraits/attachments are 680 of the asset files,
military ~250; asset-streaming estimates portraits + units + their textures at ~2 GB RAM, debug types at
~124 MB on disk (11 types). A type that is deferred but needed at game start is loaded **serially** on the
requesting thread (~139 ms each), which can make the start slower than eager loading - that is what the
measure step and the eager file prevent.

## 6. Risks / not verified

* Not run in-game at all. All behaviour above is static RE plus the offline test with fake engine functions.
* Unknown readers of CMeshType fields outside the hooked choke points (section 2 search is heuristic).
  A deferred type read that way shows ctor defaults: empty arrays, inverted bounding box, null material
  pointers (`+0x1b8..+0x1c8`). Empty arrays/boxes are benign; a null material dereference would crash.
  Mitigations: eager file from measure, `prefetch_after_s`, the default defer set is limited to
  portraits/military/debug.
* Thread safety of Load against rendering and possible lock inversions (section 4).
* Hitches: each on-demand load blocks the requesting thread (~139 ms average per type at startup;
  a portrait with ~10 accessories ~1.4 s the first time).
* The meaning of `0x3794de0`'s flag (asset pass 0/1) is inferred from the call order around the pass.
* Copies made by `CTransactionResource<SMeshTypesAndArrows>` (editor/transactions) are new pointers that
  are not tracked; if one is copied from a deferred type it stays unloaded [I: not used in normal play].
* measure counts a Load-internal `03ad7860` call as a use only outside the pass (engine Reload); harmless.

## 7. First in-game session (1.15.1, lead, 4 h, mode=lazy, defaults) and the hitch fixes

Data: `binaries/v3x/lazy_mesh_used.txt` + `v3x.log`. No crash. Startup pass 6.7 s wall (2118 types, 1086 loaded,
1032 deferred). 531 on-demand loads (28.4 s total); the log shows the first 400:
* 363 `entity_blendshape` + 1 `entity_override`, all between +0 and +14 s after the pass, on one thread
  (entity parsing): **~14 s of serial loading right after the pass** - most of the remaining startup cost.
* 36+ `mesh_object` in game (worst 944 ms, 453, 330 ms) on the main/interface thread = visible hitches.

Fixes (plugin, offline-tested, not run in-game yet):
1. `startup_from_report=1`: at plugin load the previous `lazy_mesh_used.txt` is read (before it is rewritten);
   types whose first site was `entity_blendshape` / `entity_override` (364 in that file) get decision
   `eager:prev_startup` and load in the parallel pass instead (8 threads instead of 1).
2. `prefetch_on_game=1`: on the first `load_transition` to `Game` (API v3 event from fast_load), one
   THREAD_PRIORITY_LOWEST thread loads the still-deferred types that the previous report lists as used,
   most uses first, one at a time with `prefetch_gap_ms` (20) between, through `lm_ensure` (a game request for
   the type being prefetched just waits for it). Types never used before stay deferred (`prefetch_all=0`),
   so the RAM saving stays for them. Leaving `Game` stops it.
3. Session summary in v3x.log at every load transition and in `lazymesh`: count, total ms, main-thread
   count/ms, >100/>500 ms, worst (name, site), per site. Per-load lines now mark `(main)`.

Placeholder mesh for main-thread requests: **not done, judged unsafe.** The CMeshObject ctor copies type data
(`+0x38`, `+0xcc` blend count, `+0x1d0` render data) at construction, and callers read locators/bounds/
materials of the type right after (e.g. 037ab6f0 attachments). A deferred load would need the engine's Reload
path (refreshes objects registered in `+0x1e0`) to fix objects already built, which is unverified (we do not
know that every object registers there) and leaves a window with null materials. On-demand loads stay synchronous;
prefetch is what removes them.

**Merge (`merge_report=1`, default):** the report is no longer overwritten. It is the union of the previous file
(loaded once at plugin load) and this session: uses add up, an entity-parse first site from before is kept, and
types used only before stay as data lines (`phase prev`, decision `carried`). A short session therefore never
shrinks the startup (`eager:prev_startup`) or prefetch sets. No decay (counts only order the prefetch).
