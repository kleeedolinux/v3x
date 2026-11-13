---
title: "Installing V3X (players)"
summary: "V3X-Setup.exe: download, install, what it changes, repair, uninstall."
group: players
order: 1
source: "docs/install.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/install.md"
updated: "2026-11-13"
---
V3X adds engine fixes and quality-of-life features to Victoria 3 **1.15.1**. You install it with
one file, `V3X-Setup.exe`. It needs no admin rights and changes no game file.

> **Single player only.** Do not use V3X in multiplayer. Before you play online, uninstall it, or
> rename `binaries\version.dll` (for example to `version.dll.off`) and rename it back afterwards.

## 1. Download

Get `V3X-Setup.exe` from the [Releases page](https://github.com/kleeedolinux/v3x/releases) (or from
whoever sent it to you). The release also lists its SHA-256 checksum (`SHA256SUMS.txt`) if you want
to check the file.

Windows SmartScreen or your antivirus may warn, because the file is new and not signed: choose
**More info > Run anyway** if you trust the source. The [FAQ](/docs/faq/) explains why tools like V3X
get flagged.

## 2. Run it

1. Close Victoria 3 (setup refuses to run while the game is open).
2. Double-click `V3X-Setup.exe`. It finds Victoria 3 through Steam. If it cannot, it asks you for
   the folder: in Steam, right-click Victoria 3 > **Manage** > **Browse local files**, and pick that
   `Victoria 3` folder.
3. Check that the window says `Game version: release/1.15.1 (supported)`, then click
   **Install V3X**.
4. Start the game as usual (Steam or the Paradox launcher). A small **V3X console** window opens
   next to the game and lists what loaded. Hide it with `console=0` in `binaries\v3x\v3x.ini`.

If the game version is not supported (a newer patch), setup says so and installs only if you
click **Install anyway**. Plugins switch themselves off where the game's code changed, but that
combination was never tested: waiting for a V3X update is the safe choice.

## 3. What it changes

Everything goes into the game's `binaries` folder:

| file | what it is |
|---|---|
| `binaries\version.dll` | the V3X loader. Windows loads it with the game; it loads the plugins |
| `binaries\v3x\plugins\*.dll` | the plugins |
| `binaries\v3x\*.ini` | settings. Created only if missing: setup never overwrites your settings |
| `binaries\v3x\install_manifest.txt` | the list of files setup installed (with checksums), used by repair and uninstall |
| `binaries\v3x\install.log` | what setup did |
| `binaries\v3x\backup\version.dll` | only if another tool's `version.dll` was there before: it is kept here and put back on uninstall |

While you play, V3X writes logs and reports into `binaries\v3x\` (`v3x.log`, `crash.log`, ...).
Steam's "Verify integrity of game files" does not remove V3X.

### Which plugins are on

On by default: `crash_logger`, `content_packs`, `mod_menu`, `keep_history`, `ironman_console`,
`log_filter`, `frame_pacer`, `cpu_sched`, `gfx_driver`, `mem_tuner`, `fast_load`.
Installed but off: `console_plus`, `loc_fallback`, `enhance`, `asset_stream`, `lazy_mesh`,
`sim_opt`. What each one does: [plugins.md](/docs/plugins/).

Turn plugins on or off with the **V3X** button on the main menu, or in `binaries\v3x\v3x.ini`:

```ini
[plugins]
ironman_console=0
loc_fallback=1
```

Changes apply at the next start. If achievements matter to you, set `ironman_console=0` (it lets
the console work in ironman).

## 4. Repair, update, uninstall

Run `V3X-Setup.exe` again:

* **Repair / update V3X** copies back any V3X file that is missing or damaged (or replaces them
  with the newer version). Your settings are kept.
* **Uninstall V3X** removes exactly the files setup installed and puts back a `version.dll` that
  was there before. Logs, reports and settings you changed stay in `binaries\v3x\`; tick
  *Also delete logs, reports and settings* to remove that folder too.

Lost the setup file? Delete `binaries\version.dll` and the `binaries\v3x` folder by hand. That is
the whole uninstall.

## 5. Troubleshooting

| problem | what to do |
|---|---|
| "Victoria 3 is running" | close the game (check the task manager for `victoria3.exe`), then run setup again |
| "Victoria 3 was not found automatically" | pick the `Victoria 3` folder (the one with the `binaries` folder) |
| "This setup file is damaged" | download it again; the file is checked before anything is written |
| "could not write ... is it in use?" | close the game and any tool that may hold the file, then run **Repair** |
| the game does not start or crashes right away | rename `binaries\version.dll` to start without V3X. If that helps, turn plugins off one by one in `v3x.ini`, or uninstall |
| something else | send `binaries\v3x\v3x.log`, `crash.log`, `crash.prev.log` and `install.log`. Copy them before you restart: the next start overwrites the logs |

More: [troubleshooting.md](/docs/troubleshooting/), [faq.md](/docs/faq/).

## For scripts and testing

`V3X-Setup.exe --silent [--game-dir "<...>\Victoria 3"] [--repair | --uninstall [--purge]] [--force] [--status]`
prints to stdout and returns 0 ok, 1 error, 2 unsupported game build, 3 game running, 4 game not
found, 5 not installed, 6 damaged setup file, 7 bad arguments. Details: [installer/README.md](https://github.com/kleeedolinux/v3x/blob/main/installer/README.md).
