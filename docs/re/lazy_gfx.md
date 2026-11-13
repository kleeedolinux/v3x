# Why texture loading is slow, and lazy proxies (Victoria 3 1.15.1)

Worker: lazygfx. The game was not launched for this work. RVAs are relative to the image base.
Tags: **[V]** = verified in the disassembly, the data files or the offline test. **[M]** = measured in a
recorded in-game run (logs/profiles listed below). **[I]** = inferred, not checked.

Evidence:
- `binaries/v3x/v3x.log`, session 14:25-14:31 on 2026-11-13: map_cache `spike` and `load` lines, and cpu_sched thread names.
- `binaries/v3x/map_cache_report.txt` (session ending 14:05) and `binaries/v3x/mod_report.txt` (mod_loader measure).
- `D:/v3x_tmp/sessions/20261113-122224/profile_edges_Empty_to_Game_2.csv`: devtools sampler, about 1 kHz per sampled thread.
- Tool: `tools/re_gfx_why.py` with three commands, `edges` (CreateTexture subtree plus the Win32 leaves),
  `log` (spikes and per-type cost) and `csv` (lazy_gfx records).

## 1. Answer: where the time goes

**The biggest single cause is a fixed cost per file in PhysFS path probing. Bytes and decoding matter much less.**

| evidence | number |
|---|---|
| main-menu stall 14:28:02 [M] | spike 9522 ms; texture reads in it: 704 files, 208.4 MB, 9107 ms = **12.9 ms per file** (96 % of the stall) |
| small files cost the same [M] | 14:27:52: 28 files, **0.1 MB**, 275 ms = 9.8 ms per file. 14:27:39: 30 files, 1.8 MB, 10.2 ms per file. Per-file time hardly depends on size |
| CreateTexture subtree, Empty->Game profile [M] | 32,500 samples in direct callees: **VFSOpen 0x3e1c4f0 55.9 %**, texture object 0x3c42ef0 15.9 % (almost all of it is its second getRealDir, below), file read 11.0 %, DX11 CreatePixelBuffer 10.2 %, decode 5.6 % |
| Win32 leaves under them [M] | **CreateFileW 32,105**, **GetFileAttributesExW 24,578**, ReadFile 6,360, d3d11 3,774 samples. The first two are PhysFS DIR openRead / stat probes (docs/re/modloading.md section 2) |
| probe volume [M] | mod_loader measure, whole session: 42 roots, stat 2.31 M (2.25 M misses), open 1.22 M (1.19 M misses), **103.8 s in misses** (all threads, all file types), 0 index mismatches |
| per type, logged slow loads only [M] | .dds 95 files 10.6 ms/MB; **.png 8 files 47.5 ms/MB**; .jpg 3 files 110.6 ms/MB. The single slowest load of the session is `gfx/interface/illustrations/treaties/diplomacy_bg.png` (13.9 MB, 369-466 ms) |
| threads [M] | every GUI/menu cache-miss load logged before the Game state ran on **tid 3332 = cpu_sched "worker normal #0"**, not on the main thread (13252). All 39 lines are on one thread, so these loads are serial. Model/material textures during Empty->Game ran on 6+ workers in parallel |

Why per file [V + M]: each CreateTexture probes the PhysFS search path twice for stat and once for open:
- `getRealDir` 0x3dffd50 at VFSOpen+0x40 calls GetFileAttributesExW on every root until the file is found.
- `openRead` calls CreateFileW on every root until the file is found.
- texture object 0x3c42ef0 at +0xa3 calls `0x3e14180(path)`. Its result goes into `[tex+0x7c]`. It runs getRealDir again (GetFileAttributesExW).
- 34 mod/DLC roots sit above `game/`, so a vanilla texture pays about 3 x 34 failed Win32 calls. At the measured ~30 us per miss
  (103.8 s / 3.44 M) that is ~3 ms [I], plus the first open of the real file (AV scan, ~4.6 ms in the mod_loader bench [I]), plus read, decode and upload.
  This roughly adds up to the measured 10-13 ms per file.

What is not the cause:
- **PNG decode** is FreeImage + libpng, statically linked (strings `FreeImage`, `libpng`, `IHDR` in the exe [V]), and runs on the CPU.
  It costs about 4.5x more per MB than DDS, but by count it is small. Decode is 5.6 % of CreateTexture samples, and this includes any CPU mip or format work
  inside the loader.
  The playset's mods ship 2201 PNG / 349 JPG / 12449 DDS files in total (all subscribed workshop items). The enabled ones with PNGs are Cheat Menu Pro 3194757702 (179) and 3717461054 (141) [V file count].
- **Block compression / mip generation**: no evidence of a run-time BC encoder (no squish/Compressonator/DirectXTex strings [V]).
  CPU work in the loader is bounded by the 5.6 % above.
- **GPU upload**: 10.2 % (CreateTexture2D with initial data on the iGPU).

Decode dispatch [V] (`0x3c5e0d0`): it picks the loader **by content**, not by extension. It checks a KTX-style 12-byte magic first (`0x3c3e290`), then
`'DDS '` (`0x3c37c50`). After that comes the extension `.tga`, then FreeImage (`0x3c5cc10`) with "Failed to load image '{}': unknown format.".

## 2. The CreateTexture pipeline (anchors used by plugins/lazy_gfx)

CreateTexture `0x3c5af20(out, std::string *path, mips, flags)` [V]:

| call site (ret) | callee | what |
|---|---|---|
| +0xe3 (+0xe8) | `0x3c5df50(&opts, &view)` | load_settings lookup; view = {char *p; int len} of the path without the `??_/srgb` suffix |
| +0x1bb | VFSOpen `0x3e1c4f0` (par_load hooks it) | getRealDir `0x3dffd50` at +0x40 (ret +0x45; returns the PhysFS DirHandle, `+8` = mounted dir name), then openRead |
| +0x1c7 (+0x1cc) | `0x3e3c720` | moves the returned file object = "VFSOpen returned" |
| +0x20f | Reserve `0x847fe0` (map_cache hooks it) | buffer = file size, then file vslot +8 = read |
| +0x248 (+0x24d) | decode `0x3c5e0d0(&img, &view, buf, size, &opts)` | img = 0x40-byte struct at CreateTexture rbp-0x80; layout not RE'd |
| +0x365 (+0x368) | `[ctx]+0x50` = `CGfxDX11MasterContext` vtable 0x4ae8400 slot 10 = `0x3c6ac30` | "Failed creating pixel buffer"; CreateTexture2D |
| +0x3fa (+0x3ff) | `0x3c42ef0(ctx, &out, pixbuf, &desc, path, (flags>>4)&1)` | `[ctx]+0x60` = `0x3c6b0d0` builds the `CGfxDX11Texture`, copies the path to `[tex+0x48]+0x10`, `[tex+0x7c] = 0x3e14180(path)` (at +0xa3, ret +0xa8), bit 4 = register in the live list `0x3c43660` |

Flags [V]: bit 0 clears a load option. Bit 2 sets option +0x69. **Bit 3 (8) = streamable**, effective only when the streamer
is enabled (`[ctx+0x1c1]`). Bit 4 = live-list registration. Bit 5 (0x20) = do not Add to the streamer.

## 3. Lazy proxies: what the engine already has

1. **In-place swap [V]**: `CGfxDX11Texture` (vtable 0x4ae8cf8, 4 slots) slot 3 = `0x3c7b6b0(tex, other)` swaps
   `+0x40` (GPU resource), `+0x68..+0x7b` (20-byte description: size/format/mips [I]), `+0x7c` (file stamp) and `+0x80` (view)
   between two texture objects. It is plain moves, not atomic. It does not touch the refcount (+8), owner (+0x10), path desc (+0x48) or streaming state (+0x50).
2. **The engine's own deferred reload uses it [V]**: `0x3c62c60` (streamer update, reached from the job `0x3dfb760`, off
   the main thread) works like this:
   - finds the live texture by path (`0x3c43430`);
   - calls `CreateTexture(&new, path, mips, 0x28 | (srgb ? 2 : 0) | (0x10 unless [tex+0x18] & 0xff000000 == 0xff000000))`;
   - calls `tex->vtbl[3](tex, new)` (the swap);
   - releases `new` with the standard sequence.

   So "create the real texture elsewhere, then swap it into the object everyone already holds" is the engine's own mechanism.
3. **The streamer (flag bit 3)** is not suitable for GUI [I]:
   - it only manages DDS mips;
   - it chooses mips from mesh UV factors, so a GUI texture could stay at a low mip;
   - it is started only when the `texture_streaming` setting is on (docs/re/streaming.md);
   - PNG has no mips.
4. CreateTexture runs fine on worker threads [M: the 'direct' loads on tids 10620/11588/17784/19416]. So the deferred load can run on a worker.

**Proxy design (not implemented, see section 5).**
- **Request.** On a GetOrLoadTexture miss for an eligible GUI path, return a real `CGfxDX11Texture` built by
  CreateTexture from the same path, but feed it placeholder bytes. At decode-pre, swap `buf/size` for a cached tiny DDS of the
  **same width/height/format** (decode dispatches by content, see section 1). The object then has the real path desc, refcount and owner, and
  the cache keys it normally.
- **Real load.** Queue the real load on a worker: `CreateTexture(&real, path, 0, flags as in 0x3c62c60)`.
- **Swap.** Swap with slot 3 on the main thread between frames (on_frame), then release `real` (it now holds the placeholder resources).
- **First use = first request.** GUI textures are requested when a widget is created or drawn (`0x3c58600 <- 0x37fe920 <-
  CPdxGuiContext::Render 0x38aa110`, docs/re/mapmode.md). So "defer to first use" only pays off for textures requested
  at startup but drawn later. lazy_gfx measure records the requester chain to find them.
- **Risks.**
  - (a) Widgets read the size at creation ("size from texture", sprite frames). A placeholder with other dimensions
    breaks the layout permanently, so the dimensions must come from a previous run's record.
  - (b) The swap is not atomic. The engine does the same from a job, but whether the render thread can read +0x40/+0x80 during the swap is not RE'd.
  - (c) The decode-pre buffer swap needs `buf` to stay valid until the pixel buffer is created.

## 4. Quick wins, ranked

| # | what | expected | status |
|---|---|---|---|
| 1 | **mod_loader `mode=on`**: predicted misses return "not found" without a syscall | removes most of the ~3 x 34 failed probes per texture, up to ~97 s of miss time over all threads/files (index-predicted); for the 704-file menu stall ~2 s of 9.1 s [I] | implemented by modload, measured 0 mismatches in game [M]; not enabled |
| 2 | **lazy_gfx prefetch** (`mode=on`): warm reads of the previous run's startup/menu texture files on 4 background threads | the serial thread's cold opens and reads hit the OS cache. The expected gain is the first-open + read part of 12.9 ms/file [I] | implemented, offline-tested; the `prefetched` CSV column measures it |
| 3 | skip the second getRealDir (`0x3e14180` in the texture object, 15.9 %) | ~1/6 of CreateTexture | not done: it writes `[tex+0x7c]`, which the swap and reload compare; meaning not RE'd [I: file stamp for hot reload] |
| 4 | **processed-texture cache** (lead): PNG/JPG/TGA -> DDS of the engine's exact decode output, keyed by real file + size + mtime + `game_build_id()`; decode-pre feeds the cached DDS (dispatch by content [V]) | PNG ~47 ms/MB -> DDS loader (zero copy); at most the decode share (5.6 % overall, more in PNG-heavy menus) | **implemented, `png_cache=off` by default**, offline-tested (section 6); needs build -> verify -> on in game
| 5 | shared gfx file index (lead): path -> root, size, mtime (+ format/dims) persisted across launches | removes probing (= #1) and lets prefetch/cache resolve paths without the engine | asked modload (chat #4323) to persist mod_loader's per-root index in a shared format; lazy_gfx will only read it |
| 6 | parallel decode of independent textures | GUI loads are synchronous calls that return an object, so parallelism needs the proxy (section 3) | not done |

## 5. plugins/lazy_gfx (built, offline-verified)

- Files: `lazy_gfx.c` (anchors, hooks, reports, prefetch start), `lg_core.c/.h` (config, records, aggregation, report/CSV,
  manifest, prefetch threads), `lazy_gfx.ini.example`.
- Hooks: 7 `hook_safe` pre+post on opts / getRealDir / vfs_move / decode / pixbuf / obj / stamp.
  - Each reacts only when its return address is the call site in section 2 (other callers pass straight through).
  - Not hooked: CreateTexture/GetOrLoadTexture/Reserve (map_cache) and VFSOpen (par_load).
- Fail closed (`lg_resolve`): every hook is reached from the CreateTexture anchor (string plus the map_cache shapes plus every call
  site), and the targets are checked:
  - VFSOpen = the "VFSOpen Error: " function;
  - decode = the "unknown format." function;
  - pixbuf = the slot 0x10 before a "Failed creating texture" function in a vtable whose RTTI is `CGfxDX11MasterContext` (both strings exist twice: DX11 + Vulkan);
  - all prologues.

  If any check fails, nothing is hooked. If a hook_safe call fails, every hook installed so far is removed.
- `mode=measure` (default, also when there is no ini) changes no behaviour.
  - Per texture it records: phase (fast_load transitions), thread, type, size, ms in opts/stat/open/read/decode/upload/obj/stamp, requester RVAs
    (frames above CreateTexture; cache misses flagged), PhysFS root, the img struct.
  - Outputs: `v3x/lazy_gfx_report.txt` (stage split per phase x thread x type, per-thread table, slowest 40) and
    `v3x/lazy_gfx_textures.csv`, after every transition and at exit (`lazygfx report` on demand). It also writes `v3x/lazy_gfx_prefetch.txt` when the
    first Game load begins.
- `mode=on`: measure, plus the prefetch of that manifest (size+mtime must match; `prefetch_max_mb` cap). It stops when Game is reached.
- Test: `test/lazy_gfx_test.c`. The build line is in the file header. Output: `lgt.exe ../binaries/victoria3.exe` -> ALL OK. It covers:
  - config + example == defaults;
  - a fake GetOrLoadTexture -> CreateTexture request path firing the hooks in engine order with the call-site return addresses;
  - foreign callers ignored; a missing file gives an incomplete record; a worker thread;
  - phases, report/CSV, manifest, prefetch (skip on change, byte budget, `prefetched` flag), record cap;
  - real-exe anchors == the 1.15.1 RVAs;
  - fail-closed with one changed byte in 3 places;
  - `v3x_plugin_load` end to end (7 hook_safe calls).

## 6. Processed-texture cache (`png_cache`, implemented, off by default)

Static RE of the decoded-image struct ("img", CreateTexture rbp-0x80) [V]:
- FreeImage path `0x3c5cc10` writes `+0` width, `+4` height, `+0x10` pdx format byte (0x12/0x1f/0x21/0x25), `+0x13 = 1`, `+0x16 = 2`.
  It allocates the pixels (`0x677e60`) into `+0x40` (CreateTexture frees them) and pushes ONE 40-byte subresource with
  `0x169bc30` into the CPdxArray at `+0x18` (ptr), `+0x20` (cap), `+0x24` (size). The subresource holds `+0xc` w, `+0x10` h, `+0x14` depth 1, `+0x18` bytes, `+0x20` data ptr.
  So PNG gets no mips and no conversion at load.
- DDS loader `0x3c37c50` pushes one subresource per mip whose data pointer points **into the file buffer** (zero copy,
  `[rbp-0x70] = buf + off`). CreateTexture keeps that buffer until after the pixel buffer is created.

Pipeline (`plugins/lazy_gfx/lg_cache.c`, `lazy_gfx.c` dec_pre/dec_post). Everything is in the decode hook; no new hooks:
- **Layout gate.** Every plain 2D DDS the engine decodes is checked against its own header: dims (allowing load_settings mip drops),
  per-mip dims, and data pointers inside the buffer. The cache acts only after `layout_min` (16) confirmations and zero contradictions.
  The same DDS decodes teach "pdx format byte -> DDS pixel-format header".
- **build.** After the engine decodes a png/jpg/tga, a background thread writes `<cache>/<key>.v3xtex`. It holds a header (source path, size,
  mtime, build id, the engine's decode ms, the **digest** of the engine output: img +0x08..+0x17, every subresource
  +0x00..+0x1f, a hash of all pixel bytes) and a single-mip DDS of exactly those bytes, with the learned header for that format byte.
- **verify.** As build. If an entry exists, the fresh engine decode must match its digest (this tests FreeImage determinism). On a mismatch the entry is deleted and the cache is off.
- **on.** Decode-pre replaces `buf/size` with the cached DDS (hook_safe allows changing args). The engine's DDS loader decodes it.
  Post compares the result with the stored digest:
  - equal = hit;
  - different = entry deleted, cache off for the session;
  - decode failed with nothing pushed = re-decode the real source through the trampoline (exact 5-argument prototype).

  The buffer is freed when the texture object exists.
- Not covered: block-compressing PNGs. That would change the output, so it is not "identical". Mips for PNG: none in either path.

## 7. What is left per file once VFS probing is gone

mod_loader `mode=on` was installed at 14:38 and answers predicted-miss stat/openRead without syscalls. That covers getRealDir in VFSOpen and
the second one inside `0x3e14180` (texture object); both go through the same DIR stat. The lazy_gfx report now prints, from the kept records:
- `per file: path lookup (stat+open+stamp) X ms, rest (opts/read/decode/upload/obj) Y ms`;
- the average of files < 64 KB (= the fixed cost).

Read the remainder from the next measure session:
- If small files still cost several ms, the remaining fixed part is the first open of the real file (CreateFileW hit, AV scan [I]) + read. Use prefetch (`mode=on`).
- If upload dominates, a proxy is the only lever.

## 8. Proxy: status and go/no-go

Not implemented. It cannot live in lazy_gfx's hooks: by decode time the file is already found, opened and read, so a placeholder there saves
only decode + upload of a same-size texture. A useful proxy must answer at GetOrLoadTexture/CreateTexture (map_cache's hooks), which needs:
- (1) a placeholder `CGfxDX11Texture` carrying the real path desc. `[tex+0x48]` is not swapped by slot 3, and `0x3c62c60` compares it with the live-list request, so desc ownership and live-list registration (flag bit 4, `0x3c43660`) must be RE'd first;
- (2) the real dimensions/format before the file is read (from the previous run's records: img +0/+4/+0x10 in the CSV);
- (3) render-thread safety of the slot-3 swap.

Go if the next measure run shows, after mod_loader on + prefetch, more than ~1 s of serial texture time before the main menu
in textures that the requester column shows are not created by the GUI render path (`0x3c58600 <- 0x37fe920 <- 0x38aa110`).
Otherwise, no go.

## Not verified

- Anything at runtime for lazy_gfx: hook overhead on getRealDir (called for every VFS lookup), the stage split, prefetch gain.
- That tid 3332's serial loads are what blocks the main thread during the menu stall. The spike texture-ms ~= spike-ms points to it [I].
- The ~3 ms probing / ~4.6 ms first-open split of the 12.9 ms per file. The split is arithmetic from mod_loader numbers [I]; lazy_gfx measure gives it directly.
- The img layout at runtime (the layout gate checks it before the cache acts), that FreeImage output is deterministic
  (verify mode), that the DDS loader reproduces it (on mode post-check), the 20 bytes at tex+0x68, render-thread safety of the slot-3 swap.
