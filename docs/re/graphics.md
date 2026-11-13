# V3X RE: rendering and graphics settings (Victoria 3 1.15.1, `victoria3.exe`)

Worker: re-graphics. Binary base is 0x140000000 and every address here is an RVA. Static analysis only: the game was not launched.
Tags: **[V]** means verified in the exe or data (instruction or string seen). **[I]** means inferred and not yet checked at runtime.
Tools: `v3x/tools/pestrings.py`, `pexref.py` and `peptr.py`, plus throwaway scripts (annotated disasm, pdata function bounds, E8 callers, IAT call sites). Those scripts were not committed.

Lead's machine: i5-1235U, Iris Xe (log reports "GPU Dedicated Memory: 128"), 16 GB RAM. Current pdx_settings.json: `quality=medium`, `render_scale=0.5`, `vsync=false`. The renderer key is absent, so the default is used, which is DX11 (only `shadercache/dx11` exists).

---

## 1. Renderer backends

- **[V]** Clausewitz `pdx_gfx2` has two backends in this build: **DX11** (`pdx_gfx2\DX11\gfx_dx11_*.cpp`) and **Vulkan** (`pdx_gfx2\Vulkan\gfx_vk_*.cpp`, loaded at runtime through `vulkan-1.dll` @ 0x4af4a08). There is no DX12 pdx_gfx2 backend. The `D3D12_*` strings (0x4610520..) belong to the statically linked SDL renderer, not the game renderer.
- **[V]** Enum: dx11=0, dx12=1, vulkan=2, 3=unsupported ("Unsupported render backend: {} for platform, falling back to DirectX", fn 0x37a0b30), null=4.
- **[V]** The settings menu `renderer` (Graphics) offers only **DX11** and **Vulkan**. The option table is built at fn 0x60e5f0 and stored at 0x5c6c520.
- **[V]** Launch args are parsed in fn 0x37a0830: `-renderer=<dx11|dx12|vulkan|random|null>`, the shorthand flags `-dx11`, `-dx12`, `-vulkan`, and `-randomrenderer` (random picks DX11 or Vulkan).
- **[V]** Imports: `d3d11!D3D11CreateDevice` (fn 0x3c682c0), `dxgi!CreateDXGIFactory1`, `D3DCOMPILER_47!D3DCompile/D3DReflect` (FXC, DX11 path), `dxcompiler!DxcCreateInstance` (fn 0x3d25400, Vulkan path, `-fspv-target-env=vulkan1.1`).

### Present / vsync / tearing  **[V]**
- `CGfxDX11SwapChain::Present` is at **0x3c7a8d0** (fn 0x3c7a8d0-0x3c7b143). It calls `IDXGISwapChain::Present` through `call [rax+0x40]` at 0x3c7a92b.
  - `this+0x40` holds the IDXGISwapChain*.
  - SyncInterval is `this[+0x3d] != 0` (the vsync flag).
  - Flags: `1` (DXGI_PRESENT_TEST) when the global byte 0x5cc6bfc is set (probably occluded or minimised). Otherwise `0x200` (DXGI_PRESENT_ALLOW_TEARING) when vsync is off and `this[+0x49]` is set (tearing supported, so this is a flip-model swapchain). Otherwise 0.
  - It handles 0x887a0005 (DEVICE_REMOVED) next.
  - Signature for `pattern_scan`: `48 89 5C 24 10 55 48 8D 6C 24 F0 48 81 EC 10 01 00 00 C5 F8 29 B4 24 00 01 00 00 33 DB 89 5D 20 38 1D ?? ?? ?? ?? 74 08 41 B8 01 00 00 00`. A string anchor also works: this function xrefs `gfx_dx11_swapchain.cpp` (0x4aec440) at 0x3c7aa6b.
- **Hooking Present.** The best option is to hook the engine wrapper 0x3c7a8d0 directly (MinHook, `rcx`=this). It is simpler and more stable than a DXGI vtable hook because there is no dummy device and no overlay conflicts. Fallback: create a dummy D3D11 device and swapchain in the plugin and hook vtable slot 8 (Present) / 22 (Present1). Avoid that before WinMain because d3d11 is not loaded yet; do it on the first `on_frame`.

### Frame limiter  **[V]**
- Setting `setting_framerate_cap` (Graphics, in the UI). Options (fn 0x60fba0): off / 30 / 60 / 90 / 120 / 144 / 165. The setting object pointer is at global **0x5c63588** and its value array is at `+0x1b8`.
- An on-change lambda at **0x37d91c0** writes `target_frame_time = 1.0/fps` (double) to `obj+0x38` and `enabled` to `obj+0x37` on the graphics-settings owner. Where the wait happens was not traced **[I]**.
- Console command `Graphics.CapFramerate N` (fn 0x379ebc0) only accepts values that exist in the enum.
- The cap is static. There is **no adaptive or idle cap**: no "paused" or "background" framerate strings were found.

### Rendering inside the simulation tick  **[V strings / I behaviour]**
- Launch arg `-disable_renderframeifneeded`: "Sacrifice sub-tick rendering for tick speed". Profiler zone `GameStateTick.RenderFrameIfNeeded` (0x475faa0, xref 0x65867).
- `RenderFrameIfNeeded` is xref'd from 12 tick tasks (0x8e8166, 0x9b2c5d, 0xbc2bde, ...). So the session thread pumps frames during long ticks, which is the same mechanism Stellaris-perf caps.

## 2. Settings: all graphics keys

Presets live in `game/compound_settings.txt`. **[V]** for the medium preset, which the lead uses: anti_aliasing=MSAAx2, aniso x8, portrait MSAA x4, portrait_render_scale medium, texture_quality medium, mesh_lod_bias medium, lod_fade on, ssao on, animated_portraits on, portraits_ssao on, bloom high, DOF on, refraction high, shadowmap 2048, shader_quality medium, mapobject medium, buildings_cutoff on. `terrain_smoothing` and `texture_streaming` are **off in every preset** (lines ~288-300).

| key (pdx_settings "Graphics") | values (verified registration) | in UI? |
|---|---|---|
| render_scale | float **0.5..2.0** (min imm `0x3f000000` at 0x60f6df, max 2.0) | yes |
| vsync, display_mode (windowed/fullscreen/borderless), resolution, display_index, device | | yes |
| setting_framerate_cap | off,30,60,90,120,144,165 | yes |
| renderer | DX11, Vulkan | yes |
| anti_aliasing | DISABLED, FXAA, MSAAxN (fn 0x60ec10) | yes |
| multi_sampling | DISABLED/xN (clausewitz layout) | jomini layout only |
| shader_quality | low, medium, high -> HLSL macros `LOW_QUALITY_SHADERS` / `HIGH_QUALITY_SHADERS` (0x469b470/0x469b458) | yes |
| texture_quality | low, medium, high, ultra | yes |
| shadowmap_resolution | disabled, 1024x1024, 2048x2048, 4096x4096 (fn ~0x655c00) | yes |
| bloom_quality | disabled, low, high | yes |
| mapobject_quality | **off**, low, medium, high (fn ~0x554300) | yes |
| refraction_quality, ssao, depthoffield, mesh_lod_bias, mesh_lod_fade, buildings_cutoff, animated_portraits, portraits_ssao, portrait_* , character_panning | | yes |
| texture_streaming, texture_streaming_cache (MB) | bool / MB (0x60fff0, 0x610100) | yes, but presets force off |
| **upscale** | off + runtime list {fsr2=0, dlss=1, xess=2} (fn 0x37a3900, reads device caps array `[g_5c3f428+0x8a0]`) | **hidden** |
| **upscale_quality** | ultra_performance, performance, balanced, quality, ultra_quality | **hidden** |
| **lensflare**, **secondary_lensflare** | bool (fn 0x5c32c0 / 0x5c3300) | **hidden** |
| **terrain_smoothing** | bool (fn 0x58e570) | **hidden** |
| seasons_enabled / seasons_foliage / seasons_sea_ice, daynight_cycle | categories `seasons`, `daynight_cycle` | other UI page |

### Upscalers (XeSS / DLSS / FSR2)  **[V + I]**
- **[V]** `libxess.dll` is XeSS SDK **1.0.1.12** and exports only `xessD3D12*`. `XeFX.dll` 0.1.200.5 has a D3D11 path, but `XeFX_Loader` reads `HKLM\SOFTWARE\INTEL\IGFX\XEFX`, which is an Intel-driver feature.
- **[V]** `victoria3.exe` contains **no string reference** (ASCII or UTF-16) to `libxess`, `nvngx`, `XeFX`, or any FidelityFX/FSR2 symbol. The upscale framework exists (`CGfxUpscaleContext::Dispatch`, `PDX_UPSCALING_ENABLED` in `clausewitz/gfx/FX/cw/upscale_utils.fxh`, motion-vector shader), but the backends look unbuilt or unloaded in this Windows build.
- **[I]** The `upscale` setting would only ever offer "off" on DX11. **Conclusion: XeSS is not usable on Iris Xe in 1.15.1.** It would need DX12 (absent) or a custom integration, which is out of scope.
- The working knob is `render_scale` (lead is already at the 0.5 minimum). The engine's own upscale is the "Pre/PostUpscalePostEffects" path with a bilinear-type resolve **[I]**.

### Console variables of interest (dotted strings in .rdata) **[V names, defaults not extracted]**
`Graphics.CapFramerate`, `Graphics.DumpMemoryInformation`, `Texture.Streamer.{Info,Pause,Unpause,FullyLoadAndPause,ResetAndPause}`, `Mesh.LodBias`, `Mesh.LodFade(Range)`, `Mesh2.ThreadedLodEnabled`, `Mesh2.NumLodTaskThreads`, `Mesh.InstancingEnabled`, `Terrain.Quadtree.{ScreenSpaceErrorThreshold,LodSwitchFactor,DistanceLodFactor,ForceLodLevel,UseCache}`, `SSAO.*` (Enabled, Radius, Blur.*), `Shadows.*`, `Shadow.Update`, `Draw.{Shadows,TerrainShadows,Water,Rivers,MapObjects,Terrain,Transparent}`, `FogOfWar.AlwaysDirty`, `PostEffectVolumes.Enabled`, `Upscale.*`, `gfx.upload_data_size_limit`.
`gfx.texture_limit` ("Set texture video memory limit in megabytes", 0x4aedbf0, registered in fn 0x3cd85f0) is **Vulkan render context only**, as is `Graphics.Vulkan.ForceLowOnTextureMemory`.
Other launch args: `-gfxdebug_disable_shader_preload`, `-gfxdebug_upload_buffer_size=<MB>`, `-gfxdebug_disable_shadercache`, `-taskthreads=N`, `-skip_cache_checksum`.

## 3. Shaders: compile and cache (stutter sources)  **[V]**
- Cache location: `Documents/Paradox Interactive/Victoria 3/shadercache/dx11/{vs_5_0,ps_5_0}/<64-bit hash>.bin|.scache`. The lead's copy holds 4898 PS + 4752 VS files and totals **912 MB**. Vulkan uses `shadercache/vulkan/hlsl/`.
- `CGfxDX11MasterContext::CreateShader` is fn **0x3c6cf20**. It picks a profile (vs/ps_5_0 down to 4_0_level_9_1) and calls fn **0x3c6c640**, which looks up the cache and on a miss calls **`D3DCompile` (FXC) at 0x3c6c9dc**, then writes `.bin` + `.scache`.
  - Flags = 0 in release, which is FXC default optimisation level 1. `0x404` (skip-opt + flow-control) is used only with `-gfxdebug_shader`, and `1` only with debug.
- Permutations are compiled on demand. `-gfxdebug_disable_shader_preload` shows that the engine has a preload pass at startup. A cache miss during play (new map mode, new effect, a setting change that alters defines such as shader_quality or MSAA) means a synchronous FXC compile, which is a hitch **[I: thread not proven]**.
- Shader quality is purely macro-driven. `LOW_QUALITY_SHADERS` gates the cheaper branches in `pdxterrain.shader`, `pdxwater.shader`/`pdxwater_game.fxh`, `tree.shader`, `pdxmesh.shader`, `pdxmesh_decal.shader`, `road_mulpass.shader`, `river_*`, `coloroverlay.fxh`, `fog_of_war.fxh` and `naval_route.shader`. Dedicated low-spec effects exist: `PdxTerrainLowSpec(Skirt)` (`lowspec_terrain.fxh`, effect name xref'd at 0x28020b7) and `lowspec_water.fxh`.

## 4. Ranked opportunities (expected iGPU gain, highest first)

### A. Data and settings only (no code; shippable as a normal mod or a settings recipe)
1. **Settings recipe for Iris Xe (big, safe).** Put these in pdx_settings.json:
   - `anti_aliasing=FXAA` or `DISABLED`. MSAAx2 on an iGPU costs about 15-30% **[I]**.
   - `ssao=false`, `depthoffield=false` (DOF does 16 samples, per the `environment.txt` warning), `bloom_quality=low|disabled`, `refraction_quality=disabled` (no scene copy for water), `shadowmap_resolution=1024x1024|disabled`, `lensflare=false`, `secondary_lensflare=false` (hidden keys).
   - `shader_quality=low`, `mapobject_quality=low|off`, `buildings_cutoff=true`, `mesh_lod_bias=low`, `portraits_ssao=false`, `animated_portraits=false`.
   - Disabling seasons and day/night removes the season noise bake and extra passes.
   - Combined: likely +40-80% FPS versus medium **[I]**.
2. **Shader override mod (big, moderate risk).** A mod ships `gfx/FX/*.shader|*.fxh` at the same paths. Mod files override vanilla by path, as with all Jomini VFS; the error string "does not override a corresponding file in the skinned path" confirms path-based overrides.
   - Main target: make `shader_quality=low` cheaper still. In `pdxterrain.shader` route the normal `PdxTerrain` effect to `PixelShaderLowSpec`. Drop detail-texture layers, seasons and harvest-condition overlays from the terrain PS. Collapse `pdxwater` to the `lowspec_water.fxh` path. In `tree.shader` drop backlight/SSS and alpha-to-coverage.
   - Risks: mods must be re-merged every patch, the first run triggers a full recompile (912 MB cache), and the checksum changes (fine in single-player).
3. **Defines and environment mod (medium, low risk).**
   - `environment.txt`: `ssao.samples 8->4`, `blur_enabled=no`, `dof_samplecount 16->4`.
   - `NJominiGraphics.BLOOM_ITERATIONS` lower (note: environment.txt overrides it).
   - `NGraphics.DRAW_SHADOWS_CUTOFF` 400->200.
   - `NCities.CITY_BUILDINGS_CUTOFF_DISTANCES/FACTORS` more aggressive, `CITY_VFX_MAX_ENTITIES` 128->32, `CITY_PARTICLES_ZOOM_CUTOFF` lower.
   - `NMapName.NAME_DRAW_DISTANCE`, `NJominiMapGraphics.MAP_OBJECT_CELL_COUNT`.
   - CPU-side: `NGraphics.MAP_MARKER_FULL_UPDATE_FREQUENCY` 1.0->2.0, `COA_FLAGS_FRAMES_BETWEEN_UPDATES` 30->60, `ALERTS_FRAMES_BETWEEN_UPDATES` 5->15. These are the map icon / GUI throttles, the analogue of the Stellaris GUI throttle.
4. **Texture streaming on** (`texture_streaming=true`, cache in MB). Presets force it off. On a 128 MB shared-memory iGPU it can reduce RAM/VRAM pressure. Effect on FPS is unknown and it may add pop-in **[I]**. Test with `Texture.Streamer.Info`.

### B. V3X plugins (code)
1. **Adaptive / idle frame cap (big for battery and thermals, which means sustained FPS on a 15 W U-chip).**
   - Hook Present 0x3c7a8d0 (or use `on_frame`). Measure frame time, and when the game is paused, the window is unfocused, or there has been no input for N seconds, sleep to 20-30 FPS.
   - Otherwise cap at the user value. Precise waits use a high-resolution waitable timer.
   - Risk: low. The hook only adds a sleep before calling the original.
2. **Render cap during simulation ticks (Stellaris-perf technique).** Throttle `RenderFrameIfNeeded` (anchor: string "RenderFrameIfNeeded" 0x475fbe8 / zone "GameStateTick.RenderFrameIfNeeded" 0x475faa0, xref 0x65867) to about 10 Hz. The zero-code version is the built-in `-disable_renderframeifneeded` launch arg. Gain: tick speed rather than FPS. Risk: low-moderate.
3. **Lower the render_scale minimum** by patching the imm32 at 0x60f6df+6 (`0x3f000000`, 0.5) to 0x3e800000 (0.25) for 0.33-0.4 scale. Gain: big at very low quality, but blurry. Risk: other clamps may exist **[I]**, so verify in the UI.
4. **Shader stutter**:
   - (a) Pre-warm by keeping `.scache`, and ship advice not to change shader-affecting settings mid-game.
   - (b) Hook `D3DCompile` (IAT slot used at 0x3c6c9dc) to add `D3DCOMPILE_OPTIMIZATION_LEVEL3 (0x8000)`. Small GPU gain at best: the driver recompiles DXBC anyway, and it requires clearing the cache.
   - (c) Move cache-miss compiles off-thread. Large effort and risky.
   - Overall rank: low-medium.
5. **Force cheaper permutations without editing files.** Hook shader-macro injection to always define `LOW_QUALITY_SHADERS` (string 0x469b470 is the anchor; find its xref) while keeping other settings. Equivalent to `shader_quality=low`, so this only matters if mixed control is wanted. Low priority.

### C. Not viable / dropped
- XeSS/DLSS/FSR2: no loader in the exe and XeSS is DX12-only; see section 2.
- DX12 backend: not present.
- Vulkan on Iris Xe: selectable, may differ in performance either way. It is an easy A/B test for the lead (`renderer=Vulkan`), at the cost of a fresh shader compile.

## 5. Not verified
- Default values of the console vars and the texture_streaming_cache default.
- Which thread runs `D3DCompile`.
- Where the frame-cap wait executes.
- Actual FPS gains: every percentage in section 4 is an estimate.
- Whether the `upscale` list is empty at runtime.
- Render target sizes/formats (lead item 3): not reached.

## 6. Frame cap and sub-tick render throttle: trace + implementation (frame_pacer)

### Engine frame cap = busy-wait  **[V]**
- fn **0x379a540** (main-thread end of frame, inferred from: frame counter `inc [0x5c3ef98]`, queues the render task via `QueueRenderTaskContext`, then the cap):
  `rdx = self->[0x20]` (graphics-settings owner); if `owner[0x37]` (cap on) and the **vsync** setting (global `0x5c629a8`, registered by the SETTING_VSYNC fn 0x60ea90) is off:
  `deadline = owner[0x38] (1/fps, double) + self[0x38] (last frame start)`; loop at **0x379a710..0x379a751**: read timer `self[0x30]`, `vcomisd`, **`pause`; jmp** -> a 100% CPU spin until the deadline. Afterwards `self[0x38] = now`.
- `owner[0x37]/[0x38]` are written by the setting_framerate_cap on-change lambda 0x37d91c0 (1.0/fps).
- Present (0x3c7a8d0): on DXGI_STATUS_OCCLUDED (0x087A0001) it sets the occluded flag (next Present uses DXGI_PRESENT_TEST) and calls `sleep_for(33 ms)` (`std::this_thread::sleep_for` = fn 0x6be220). Minimised is already roughly 30 fps on the render side **[V]**.

### Sub-tick rendering  **[V]**
- Tick tasks construct a guard at fn **0x1468a60** `(this, bool active, bool check_time, name)` (10 call sites, e.g. 0x8e81b8, 0x9b2c83, 0xbc2c89). If the debug var `GameStateTick.RenderFrameIfNeeded` (byte 0x540b565, default 1; cleared by `-disable_renderframeifneeded` at 0x691613) is on, and, when check_time is set, the lock-hold timer `[0x5c70298]` shows at least **`GameStateTick.MinLockTime`** ms (int at **0x5414c40**, default **1**, registered at 0x65a30), it releases the game state (scoped release at `this+0x20`, `gamestatetick.cpp:262`) so the main thread can render mid-tick.
- So, as with the Stellaris technique, the knob is MinLockTime: 100 ms means the tick yields for rendering at most about 10 times per second. It is a plain int in .data, so no hook is needed.

### plugins/frame_pacer (implemented, offline-tested)
- Hook fn 0x379a540. Before calling the original: compute the engine cap (owner[0x37], !vsync, 1/owner[0x38]), choose the policy fps (`fp_choose_fps`: minimized 5 > background 20 > idle 20 when there has been no input for 30 s, or for 3 s while the sim is paused), then sleep on a **high-resolution waitable timer** (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, with a final 0.7 ms spin) to the lowest positive cap. `owner[0x37]` is cleared for the duration of the original call, so the engine's spin is skipped (`replace_busy_wait=1`), then restored.
- Hook fn 0x1468a60 only to time-stamp tick activity. "Sim paused" means no tick activity for `sim_idle_ms` (3000) **[I: heuristic]**.
- Write MinLockTime = 1000 / `tick_render_hz` (10 Hz -> 100 ms) at load and again on the first frame.
- Anchors: spin-block pattern (unique, 1 match) -> `function_start` must be exactly match - 0x171 + prologue bytes; `gamestatetick.cpp:262` string (1 xref) -> function_start + prologue bytes; `GameStateTick.MinLockTime` string -> the `lea rdx` 13 bytes before its xref, cross-checked against the guard's `movsxd rax,[MinLockTime]` at +0xbf. Each part fails closed on its own.
- The Present wrapper is resolved and logged but **not hooked**. Pacing belongs at the engine's own cap point (main thread); Present runs from the render-task queue **[I]**.
