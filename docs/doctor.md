# V3X doctor

The doctor checks your V3X setup while the game runs and tells you, in one line per item, what is
fine, what is worth a look and what is broken, plus one line on what to do about it.

There is also an **offline doctor** for when the game does not start: `V3X-Setup.exe --doctor`
(see [install.md](install.md)). Both use the same check IDs, so a bug report can say "D013".

## Using it

Open the V3X console window (it opens next to the game) and type:

| command | what it does |
|---|---|
| `doctor` | runs every check and prints the results |
| `doctor fix <ID>` | applies the safe fix of one check, e.g. `doctor fix D007.alloc_tracker`. `doctor fix D007` applies every D007 fix |
| `doctor report` | writes `binaries/v3x/doctor_report.txt` for a bug report |

The doctor also runs **once by itself** when you load a save or start a game (or about a minute
after the game window opens when the `fast_load` plugin is off). It writes one line to `v3x.log` and
the console, for example:

    [doctor] doctor: 0 problems, 2 warnings (type doctor)

It never slows the game: the checks run on a background thread.

### What the words mean

- **OK**: nothing to do.
- **WARN**: works, but costs speed, may hide a feature, or might be involved in a crash.
- **PROBLEM**: something is broken (a plugin that crashed, a file that cannot load, memory almost full).

## What `doctor fix` changes

Only settings in the `.ini` files in `binaries/v3x/`, and only when the result offers a fix:

- It changes the one value and leaves everything else in the file exactly as it was (your comments,
  spacing and the other settings).
- The first time it changes a file, it keeps the original file next to it as `<file>.doctor-bak`.
  It never deletes anything.
- The new file is written as a whole (temp file + rename), so a crash in the middle cannot leave a
  broken half file.
- It prints what changed and how to undo it, e.g.
  `changed alloc_tracker.ini [alloc_tracker] enabled: 1 -> 0. Undo: set enabled = 1 again`.
- **Changes take effect at the next game start.**
- Running the same fix twice does nothing the second time.

Turning a plugin off means `[plugins] <name> = 0` in `v3x.ini`, which is the same as switching it off
in the main menu (V3X button). To turn it on again, set it to `1` or use the menu.

## The checks

| ID | name | what it looks at |
|---|---|---|
| D001 | game_build | Is this game version one V3X was tested on? After a game update V3X runs in **safe mode**: plugins that do not declare the new version are skipped. Shows which ones. Fix: back to safe mode when `unknown_build = load` is set. |
| D002 | loader_files | V3X loader version and API. |
| D003 | foreign_proxy | Other DLL loaders next to `victoria3.exe` (dinput8.dll, dxgi.dll, winmm.dll, ...) and overlays (Steam, Discord, RivaTuner, ReShade, OBS, ...). For your information. |
| D004 | plugin_manifests | Plugins that were not loaded because of the game version or a too old V3X. |
| D005 | plugin_status | Every plugin: loaded, turned off by you, did not start (its game code was not found: it stays off safely), could not load (broken file). |
| D006 | plugin_faults | Plugins that crashed during this session. V3X switches such a plugin off and the game keeps running. |
| D007 | perf_settings | Settings that cost game time and are only meant for measuring: `alloc_tracker` (a short freeze every 30 s), devtools `profile = 1`, `tick_profiler`, lazy_gfx `csv = 1`, plugins in `mode = measure`, log_filter `enabled = 0`. Fix: the recommended value. |
| D008 | memory_commit | Memory the game uses, Windows commit charge against its limit, RAM. WARN above 85 %, PROBLEM above 95 %. |
| D009 | pagefile | A small pagefile (commit limit below 1.5x RAM), or Windows having to grow the pagefile during play (stutters). |
| D010 | disk_free | Free space on the game drive and on C:. WARN below 5 GB, PROBLEM below 1 GB. |
| D011 | mod_overrides | Enabled mods that replace a file V3X uses. Example: a mod with its own `gui/frontend/frontend_main.gui` hides the V3X main-menu button (use the console: `modmenu`). |
| D012 | crash_loop | A plugin that crashed the game while loading at the last start (V3X skipped it this time). Fix: turn it off. |
| D013 | last_crash | How the last session ended: the crashing module, a V3X plugin on the crash stack, low memory at that moment. Fix: turn that plugin off. |
| D014 | ini_syntax | Lines a plugin ignored in its `.ini`, `[plugins]` names without a DLL. |
| D015 | game_running | offline doctor only |

Per-plugin rows add the plugin name: `D005.par_load`, `D007.alloc_tracker`.

## Bug reports

Type `doctor report` and attach `binaries/v3x/doctor_report.txt` together with `v3x.log`,
`crash.log` and `crash.prev.log`. The report contains the check results, the plugin list, the
warning/error lines of `v3x.log` and the last crash summary. Your Windows user name and your user
folder are replaced by `<user>` / `%USERPROFILE%`; the only full path left is the game folder.

---

## Português (Brasil)

O doctor verifica a instalação do V3X com o jogo aberto. No console do V3X digite:

- `doctor` - roda todas as verificações (OK / WARN = atenção / PROBLEM = problema) com uma linha "what to do" (o que fazer).
- `doctor fix <ID>` - aplica a correção segura daquele item (ex.: `doctor fix D007.alloc_tracker`). Só altera o valor no `.ini`, mantém comentários, guarda o original como `<arquivo>.doctor-bak`, nunca apaga nada e mostra como desfazer. **Vale a partir do próximo início do jogo.**
- `doctor report` - grava `binaries/v3x/doctor_report.txt` para anexar a um relato de bug (seu nome de usuário do Windows é mascarado).

Ele também roda sozinho uma vez ao carregar um jogo e escreve uma linha no `v3x.log`
(`doctor: 0 problems, 2 warnings (type doctor)`), sem travar o jogo. Se o jogo nem abre, use
`V3X-Setup.exe --doctor`.
