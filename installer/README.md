# installer/: V3X-Setup.exe

One self-contained Windows executable for players: install, repair and uninstall V3X without
admin rights. Player guide: [docs/install.md](../docs/install.md).

| file | what |
|---|---|
| `setup.c` | the installer (Win32 TaskDialog GUI + CLI). Payload format, checks and exit codes: its header comment |
| `setup.rc`, `setup.manifest` | embedded manifest: asInvoker (no UAC prompt), Common Controls v6 (TaskDialog), per-monitor DPI |
| `profile.txt` | which plugins ship on (1), ship off (0) or are not shipped (-), with the reason. Edit this to change the player set |
| `builds.txt` | supported game builds (`game_build_id` + `caligula_branch.txt`); setup refuses (CLI) or warns (GUI) on others |
| `make_dist.py` | builds `build/dist/V3X-Setup.exe` + `SHA256SUMS.txt` |
| `test_installer.py` | end-to-end test against fake game folders |

## Build

```sh
pip install ziglang
python installer/make_dist.py               # runs ./build.sh, then packages
python installer/make_dist.py --no-build    # package the current build/ (warns if a DLL is older than its source)
python installer/make_dist.py --version 0.5.0
```

`make_dist.py` fails when a folder in `plugins/` is missing from `profile.txt`, so a new plugin is
never shipped (or forgotten) silently. When C: is short on space, point zig's caches and TEMP to
another drive (`ZIG_GLOBAL_CACHE_DIR`, `ZIG_LOCAL_CACHE_DIR`, `TEMP`, `TMP`).

## Payload

`setup.c` is compiled into a stub; `make_dist.py` appends the files, a text index (kind, offset,
size, SHA-256, path) and a 64-byte footer with the index's SHA-256. At start setup verifies the
footer, the index hash and every file hash; a damaged download exits with code 6 and writes
nothing. Contents: `version.dll`, `v3x/plugins/<p>.dll` for every plugin marked 1 or 0,
`plugins/<p>/<p>.ini.example` as `v3x/<p>.ini` (created only when missing), a generated
`v3x/v3x.ini` (`[plugins]` with `<p>=1` / `<p>=0`) and `meta.txt` (version, builds, profile).

## Behaviour

* **Finding the game:** `HKCU\Software\Valve\Steam\SteamPath` (then HKLM `InstallPath`), its
  `steamapps/libraryfolders.vdf` library paths, `steamapps/common/Victoria 3/binaries/victoria3.exe`;
  then common library folders on every fixed drive; then a folder picker (GUI) or exit 4 (CLI).
  `--game-dir` accepts the `Victoria 3` folder, its `binaries` folder or the exe.
* **Supported build:** PE `TimeDateStamp-SizeOfImage` of `victoria3.exe`, computed exactly like
  `read_module_layout` in `src/core.c` (`game_build_id`), must be in `builds.txt`.
* **Game running:** every process named `victoria3.exe` whose image path is this folder's exe
  (another running install does not block). Unreadable path = treated as running.
* **Foreign version.dll:** a `binaries/version.dll` that is neither in the manifest, nor the
  payload's, nor a V3X loader (contains `V3X_DISABLE` and `v3x_plugin_load`) is copied to
  `v3x/backup/version.dll` (`version.N.dll` if a different backup exists) and restored on uninstall.
* **Settings:** `*.ini` are written only if missing. An existing `v3x.ini` only gets `<p>=0`
  lines for shipped-off plugins that have no line yet (inserted under `[plugins]`), so a plugin
  that ships off never loads by surprise; existing lines are never changed.
* **Manifest** `binaries/v3x/install_manifest.txt`: `file bin|config <sha256> <path>`, plus
  `backup <sha256> <path>`. Repair rewrites missing/changed `bin` files; an update removes `bin`
  files of an older manifest that the new payload no longer has (only if unchanged).
* **Uninstall:** deletes manifest `bin` files whose hash still matches (version.dll also if it is
  a V3X loader), `config` files only if unchanged, restores the backup, removes the manifest and
  empty folders. Logs, reports, changed settings and user files stay; `--purge` deletes
  `binaries/v3x/` entirely.
* No registry writes (no Add/Remove Programs entry): uninstall = run setup again.

## Test

```sh
python installer/test_installer.py            # uses build/dist/V3X-Setup.exe, fake games in D:/v3x_installer_test/run
```

Builds fake game folders (only the 4 KB PE header of the real `victoria3.exe` is copied, the real
exe is only read), then checks: fresh install, re-install keeps user inis byte for byte, `v3x.ini`
merge, repair after deleting / corrupting plugins, update removes stale files, uninstall keeps
logs and changed settings, foreign `version.dll` backed up and restored byte-identical, `--purge`,
manual (build.sh) install not backed up, unsupported build refused (and `--force`), game running
(a zig-built sleeper named `victoria3.exe`) refused for install and uninstall, damaged payload,
missing game, bad usage. The fake folders are deleted at the end (`--keep` keeps them).
