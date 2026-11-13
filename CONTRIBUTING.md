# Contributing

Thanks for helping. V3X runs native code inside a commercial game, so the bar is "never crash
the game, never corrupt a save". Read [docs/safety.md](docs/safety.md) first.

## Ground rules

* **No hardcoded addresses at runtime.** Find code by anchors (strings, RTTI, unique byte shapes)
  and fail closed: if anything does not match, install nothing and log why. RVAs belong in
  `docs/re/` notes, always with the anchor that finds them.
* **Never ship game files.** Do not commit anything copied from the game (exe, shaders, `.gui`,
  textures, localization). Generate such files on the user's machine from their own install (see
  `mods/v3x_lowspec_shaders/make_mod.py`). `.gitignore` excludes the generated folders.
* **No personal paths.** No `C:\Users\<name>`, no drive letters of your machine in code, scripts or
  docs meant for others. Derive paths from `api->game_dir()` / `api->v3x_dir()`, the script's own
  location, or an environment variable.
* **API changes are append-only.** Never reorder, remove or change the type of a `V3X_API`
  field or a public struct field. New fields go at the end, `V3X_API_VERSION` goes up, and
  `docs/api-reference.md` and `CHANGELOG.md` say what was added.
* **Simulation changes are opt-in** (default off) and single-player only, and their ini says so.
* **No new dependencies** without discussion. Third-party code goes in `third_party/` with its
  license, listed in `third_party/NOTICES.md`.

## Building

```sh
pip install ziglang                 # compiler
./build.sh                          # loader + all plugins -> build/
./build.sh install                  # ... and copy into ../binaries (the game)
python sdk/build_plugin.py plugins/<name> [--test] [--install]   # one plugin
```

If your system drive is short on space, move the compiler caches and temp files:

```sh
export TEMP=<scratch> TMP=<scratch> ZIG_GLOBAL_CACHE_DIR=<scratch>/zcache ZIG_LOCAL_CACHE_DIR=<scratch>/zcache
```

## Tests

Every plugin needs an **offline test** in `test/<plugin>_test.c` (or `test_<plugin>.c` next to
it for plugins built with `sdk/build_plugin.py`): config parsing, the plugin's pure logic, and its
real anchor code against `victoria3.exe` mapped as an image (`sdk/v3x_testkit.h`; nothing in the
exe runs). Tests print one line per check and `ALL OK` at the end (exit code 0). The README
"Tests" section has the command line for each.

Before a pull request:

1. `./build.sh` builds without errors.
2. All offline tests and the end-to-end tests in the README pass (at least the ones your change
   can affect; core changes: all of them, including `test/hook_safe_test.c`,
   `test/api_v4_test.c` and `test/shutdown_test.c`).
3. In-game: back up `binaries/v3x/*.log`, `crash*.log` and the game's `logs/` from the previous
   run, then smoke test (load a save, play a few days, save, load, quit). Say in the PR how far
   it was tested: offline / loads (smoke test) / played (how long). The README and
   `docs/plugins.md` tables use exactly these words.

## Pull requests

* One topic per PR. Describe what changed, why, and how it was verified (commands + results).
* Plugins: add or update the plugin's row in `README.md` and `docs/plugins.md`, its keys in the
  configuration table, a `<plugin>.ini.example` with defaults and comments, and its console
  commands.
* RE findings: `docs/re/<topic>.md`, tag every claim **[V]** verified, **[I]** inferred,
  **[D]** from dumps or **[R]** measured at runtime, and keep a "Not verified" section.
* Update `CHANGELOG.md` under "Unreleased".

## Code style

C11 with Win32, no CRT-heavy code in hooks, 4-space indent, ~120 columns, comments that say why.
Plugin logs use the plugin name as tag; failures contain `FAILED` or `WARNING` so the console
shows them red.
