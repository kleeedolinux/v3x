# Web research: Paradox engine performance (2026-11-12)

Source: research agent. Reddit and the Paradox forum could not be fetched, so most sources are
the Steam News API (full dev-diary text), Steam pages and GitHub.

## CK3
- **DD #187 (1.18, 2025-10-15)**
  - The tick has a parallel "pre-update" phase where state is frozen; a deterministic serial apply follows.
  - A holding-income cache was fixed.
  - IsScopeOK compares scope ids first.
  - Variable-timeout checks dropped from ~500k to hundreds.
  - Succession scoring runs in parallel (3x faster).
  - Tooltip memory work saved GBs.
  - Adaptive Framerate was added; Max FPS defaults to the refresh rate.
  - `-script_optimizer` launch flag.
  - https://steamcommunity.com/ogg/1158310/announcements/detail/535490230242770975
- **"By God Alone" DD #6 (1.20, 2026-09-08)**
  - A nullobject indirection was removed (~3%).
  - The allocator was made thread-safe, which let temporary-modifier cleanup run in parallel (~4%).
  - **Localization loads in parallel: startup 84 s → 16 s.**
  - The checksum is computed once, after the menu appears.
  - Vulkan uploads no longer wait on the whole queue.
  - **Mesh/illustration streaming: VRAM −33..50%** (VRAM overflow had caused crashes).
  - Full-screen buffers were halved.
  - **Animation metadata went from 1200 MB to 12 MB (RAM −1 GB+).**
  - https://steamcommunity.com/ogg/1158310/announcements/detail/687515890081597529

## Stellaris
- **Official**
  - DD #397: the monthly UpdateModifiers stutter.
  - DD #410 (4.3): dependency tracking, smaller batches, memory reuse; 11.5 s/yr vs 30.5 s/yr in 3.14.
- **Yidhar/stellaris-perf** (MinHook; https://github.com/Yidhar/stellaris-perf)
  - Tooltip reuse until input or the day changes.
  - Modifier InvalidateChildren fires only on real change (rebuilds −65%).
  - Save writer −45%.
  - Fast integer sprintf.
  - 128 KB+ block recycler.
  - RenderFrame capped at 10 fps during ticks.
  - Opinion/rule caches are OFF by default because of multiplayer determinism.
- **KeinNiemand/LargePageInjectorMods**: mimalloc with large pages. Needs SeLockMemoryPrivilege. Claims 5%+. Note: NVIDIA's driver also reads `MIMALLOC_*` env vars.
- **ljy-23/stellaris-largepage**: no gain. Lesson: CreateThread inside DllMain crashed.
- **MattMills/stellarstellaris**: throttled per-frame GUI updates on the map.

## Victoria 3
- **DD #76 (architecture)**
  - The tick is a TickTask dependency graph (`TickTask.Graph`).
  - Most expensive tasks: employment, pop need cache, modifier update.
  - Expensive countries are scheduled as separate jobs.
  - The UI blocks the session thread while reading state (CScopedGameStateRelease).
  - **mimalloc has been statically linked since 1.2** (verified in the exe: `mimalloc_%s` option strings).
- **Version notes**
  - 1.15: sway, law and building-modifier caching; a fix for a crash on many-core CPUs.
  - 1.15.1: army-selection fix only.
- **Console tools**: `Threading.TaskThreadCount`, `log_ticktask_performance`, `ScriptProfiling.Dump`, `memory_stats`, `gfx.texture_limit`, `-debug_mode`.
- **Existing mods**: Workshop performance mods are script-level only; no native Vic3 performance mod was found.
- **New Game crashes**: blamed on RAM/VRAM in Steam threads; a 32 GB pagefile helped some users (https://steamcommunity.com/app/529340/discussions/0/3489753940062842492/).
- **E-cores/SMT**: users report gains keeping the game off E-cores or disabling SMT (https://steamcommunity.com/app/529340/discussions/0/4034724270482513992/).

## Other native loaders
- CAPIE (HOI4): version.dll proxy, SHA-256 pin, each pattern must match exactly once, fails closed.
- EU4dll / CK2dll (matanki-saito).
- stellaris-mcp: RTTI dumper, MinHook on Present.

## Ranked ideas (before our own RE)
1. mimalloc large pages and option tuning (not an allocator swap)
2. Tooltip/GUI caching
3. Render cap during ticks / adaptive FPS
4. Thread count and P-core affinity
5. Gate modifier invalidation on real changes
6. Save writer / sprintf fast paths
7. iGPU out-of-memory guard (texture limit, commit check)
8. Per-day script memos (off in multiplayer)
