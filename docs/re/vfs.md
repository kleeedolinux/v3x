# VFS (PhysFS) search path - content packs

Scope: how the engine builds its virtual file system search path, and how `plugins/content_packs`
adds folders to it. Nothing here touches DLC/mod lists, ownership or checksums.

RVAs are for the victoria3.exe on disk at the time of writing (`game_version()` release/1.x). The
plugin never uses them: it finds everything by string xrefs + byte patterns. Re-check after a
patch with `python tools/re_vfs_resolve.py` (exit 0 = all resolved). Disassemble with
`python tools/re_vfs_lib.py <exe> dis|calls|callers <rva>`.

## Functions

| name (ours) | rva | how found | notes |
|---|---|---|---|
| vfs_init | 0x3e17d10 | fn referencing `"../platform_specific_game_data"` | one arg (rcx). Calls MountData 3x: platform data candidates, `../platform_specific_game_data` (`../`, `../../`, `../../../`), then `../game` candidates. |
| MountData | 0x3e17820 | fn referencing `"No subdirs mounted for game dir from candidates: {}"` | logs `"Mounted Data: {}"` per mounted dir (virtualfilesystem_physfs.cpp). |
| mount | 0x3dff0b0 | in MountData: `45 33 C0 E8 rel32` (`xor r8d,r8d; call`) | `int mount(void *unused, const char *dir, const char *mount_point /*NULL = "/"*/)`. Takes the PhysFS lock, returns 1 if `dir` is already in the search path, else creates a dir handle (0x3dfdce0) and **prepends** it (`[new+0x30] = head; head = new`) -> highest priority. 0 on failure. Same call shape at all 7 callers (incl. dlc.cpp 0x3b6d500 for mods/DLC). |
| refresh | 0x3e16bd0 | in MountData: target of `45 85 FF 0F 85 rel32` (`test r15d,r15d; jne`) is `E8 refresh` | `void refresh(void)`: frees and rebuilds the cached search-path list (0x3dfd2c0), normalises `\` to `/`. Called after mounting by MountData and dlc.cpp. |
| mount-highest-priority | 0x3e190b0 | `"Failed to mount directory as highest priority"` | NOT used: after mounting it also sets the PhysFS write dir (`"Failed to set write directory"`). |

Path format accepted by mount: native absolute path with `/` separators and no trailing `/`
(0x3e190b0 converts `\` -> `/` and 0x3dd6f00 strips trailing `/` before calling mount).

## Plugin behaviour (plugins/content_packs/content_packs.c)

- Root: `<game>/binaries/v3x/content_packs/` (created on load). Every sub-folder not starting with
  `_` or `.` is a pack, mounted at `/`.
- Hook vfs_init; after it returns, mount packs in case-insensitive alphabetical order, then call
  refresh once. Each mount prepends, so the alphabetically last pack has the highest priority and
  all packs sit above `game/`. Mods/DLC mounted later by dlc.cpp prepend above the packs.
- Optional `content_packs/order.txt`: one pack name per line, top line = highest priority, blank
  lines and `#` comments ignored, case-insensitive, UTF-8 BOM / CRLF / trailing slash tolerated.
  Listed packs go on top in file order; unlisted packs stay below them alphabetically; names that
  are not existing packs (missing, or `_`/`.` prefixed) are ignored. Re-read at every vfs_init.
- Re-running vfs_init re-mounts; already-mounted dirs are a no-op success.
- Console: `packs` lists root, init count and mounted packs.
- If a signature is not found the plugin logs it and stays inactive (returns 0, game unaffected).
