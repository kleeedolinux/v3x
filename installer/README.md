# installer/: V3X-Setup.exe

One self-contained Windows executable for players: install, repair and uninstall V3X without
admin rights. Player guide: [docs/install.md](../docs/install.md).

| file | what |
|---|---|
| `setup.c` | the installer (Win32 TaskDialog GUI + CLI). Payload format, checks and exit codes: its header comment |
| `setup.rc`, `setup.manifest` | embedded manifest: asInvoker (no UAC prompt), Common Controls v6 (TaskDialog), per-monitor DPI |
| `profile.txt` | which plugins ship on (1), ship off (0) or are not shipped (-), with the reason. Edit this to change the player set |
| `builds.txt` | supported game builds (`game_build_id` + `caligula_branch.txt`); setup refuses (CLI) or warns (GUI) on others |
| `make_dist.py` | builds `build/dist/V3X-Setup.exe`, `v3x-<version>.zip` (manual install), a copy of `install.ps1` and `SHA256SUMS.txt` (all three) |
| `install.ps1` | the one-line installer `irm https://v3x.pages.dev/install.ps1 \| iex`: finds the game, downloads + checks `V3X-Setup.exe`, runs it `--silent`. Options and exit codes: its header |
| `test_installer.py` | end-to-end test of `V3X-Setup.exe` against fake game folders |
| `test_install_ps1.py` | end-to-end test of `install.ps1` (Windows PowerShell 5.1 and pwsh 7) against fake games, a fake Steam and a local release |

Language: the GUI of `V3X-Setup.exe` and all of `install.ps1` are English or Brazilian Portuguese
(Windows display language; `V3X_LANG=en|pt` overrides). `--silent` output stays English, so it can
be parsed. `install.ps1` is kept pure ASCII (accents as `\u00XX`, decoded at run time) because
`irm | iex` on PowerShell 5.1 can decode a download as Latin-1.

Releases: `.github/workflows/release.yml` builds, tests and publishes these four files on a `v*`
tag; `ci.yml` does the same on every push / pull request and uploads them as an artifact. The site
(v3x.pages.dev) serves a copy of `install.ps1`.

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
python installer/test_install_ps1.py          # uses build/dist (install.ps1 + V3X-Setup.exe), fake games in D:/v3x_tmp/install_ps1
```

Without `../binaries/victoria3.exe` (CI) both tests use a synthetic 4 KB PE header carrying the
first build id of `builds.txt` (`--synthetic` forces it in `test_installer.py`). `test_installer.py`
also checks the profile policy: every plugin except the developer tools is shipped and only
`crash_logger`, `content_packs`, `mod_menu`, `script_ext` are on (`EXPECT_ON` / `EXPECT_UNSHIPPED`).

Builds fake game folders (only the 4 KB PE header of the real `victoria3.exe` is copied, the real
exe is only read), then checks: fresh install, re-install keeps user inis byte for byte, `v3x.ini`
merge, repair after deleting / corrupting plugins, update removes stale files, uninstall keeps
logs and changed settings, foreign `version.dll` backed up and restored byte-identical, `--purge`,
manual (build.sh) install not backed up, unsupported build refused (and `--force`), game running
(a zig-built sleeper named `victoria3.exe`) refused for install and uninstall, damaged payload,
missing game, bad usage. The fake folders are deleted at the end (`--keep` keeps them).
