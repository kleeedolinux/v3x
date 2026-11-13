# V3X Low-Spec Shaders (Victoria 3 1.15.x mod)

A normal Paradox mod: no V3X plugin is needed, it only overrides three vanilla shader files.
The aim is a cheaper map on integrated GPUs such as Intel Iris Xe. It changes graphics only, not gameplay.

## Install
1. Copy this folder to `Documents/Paradox Interactive/Victoria 3/mod/v3x_lowspec_shaders/`. It needs `.metadata/metadata.json` and `gfx/`.
2. Enable it in the launcher playset.
3. The first start after enabling recompiles the affected shader permutations. Expect a longer load; later starts use `shadercache/`.
4. Uninstall: disable the mod. You may delete `Documents/.../shadercache/` to reclaim space; it is rebuilt.

## What it changes
Every edit is a one-line switch, applied by `make_mod.py` to a fresh copy of the vanilla file. The original line is kept as a comment.

| file | edit | effect | cost avoided (inferred from the skipped vanilla code, not profiled) |
|---|---|---|---|
| `gfx/FX/pdxterrain.shader` | effects `PdxTerrain` and `PdxTerrainSkirt` use `VertexShaderLowSpec(Skirt)` + `PixelShaderLowSpec` | Terrain uses the engine's own low-spec path (`lowspec_terrain.fxh`) at **every** shader_quality. The engine normally switches to `PdxTerrainLowSpec` only at shader_quality=low (victoria3.exe 1.15.1 fn 0x2802050). | detail-texture array blending, PBR terrain lighting, shadows, seasons/harvest terrain layers |
| `gfx/FX/pdxwater.shader` | `#if defined( LOW_QUALITY_SHADERS )` -> `#if 1`; shadow block `#ifndef LOW_QUALITY_SHADERS` -> `#if 0` | All water (sea, lakes, ponds, canals, UI water) uses `CalcWaterLowSpec` and does not sample the shadow map | reflections/refraction sampling, foam and flow maps, shadow lookups (the most expensive full-screen-coverage pass on the map) |
| `gfx/FX/tree.shader` | the 4 vertex shaders: wind block `#if !defined( GUI_SHADER )` -> `#if 0` | Trees and bushes no longer sway, in both the main and the shadow pass | per-vertex wind math for every tree instance, twice when shadows are on |
| `gfx/FX/tree.shader` | pixel shader: the 3 `LOW_QUALITY_SHADERS` branches -> `#if 0` | Trees always use the low-quality pixel path: no pollution/devastation/seasons/harvest tint, no PBR sun + second sun, no backlight | per-pixel lighting on heavily overdrawn alpha-tested foliage |

Everything else stays vanilla and keeps following the in-game settings: buildings/meshes (`pdxmesh.shader`), rivers, borders, post effects.
So the mod is mainly useful when you keep **shader_quality = medium** for nicer buildings but want cheap terrain, water and trees.
At shader_quality = low, the terrain and water edits change nothing; the tree wind removal still applies.

## Expected effect (not measured)
At medium shader quality, terrain + water + trees cover almost every map pixel, so the iGPU fragment load should drop a lot. The real gain is unmeasured: compare FPS at the same camera position with the mod on and off.
Visual cost: flatter terrain lighting, simpler water, static trees.

## Updating after a game patch
```
python mods/v3x_lowspec_shaders/make_mod.py --check   # do the edits still apply?
python mods/v3x_lowspec_shaders/make_mod.py           # regenerate gfx/FX/* from the new vanilla files
```
The script refuses to write anything if any vanilla text it edits has changed. Shipping a stale copy of an old shader is the main risk of shader mods: it can break rendering after an update.

## Risks
- Not compiled or tested in-game yet. If a shader fails to compile, the game logs `Compile error` in `Documents/.../logs/error.log`, and that surface may render black or missing. Disable the mod in that case.
- Other mods that override the same three files conflict. The last one loaded wins.
