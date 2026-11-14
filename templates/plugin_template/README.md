# Plugin template

A minimal plugin that uses only the safe V3X APIs ([docs/safe-coding.md](../../docs/safe-coding.md)):
a manifest that declares its capabilities, an optional ini with clamped values, a fail-closed
anchor with an extra shape check, an observe-only `hook_safe` hook with lock-free atomic counters,
a console command that formats into a bounded `V3xStr`, checked results of every API call, and an
offline test. It builds with `-Werror`.

| file | what |
|---|---|
| `my_plugin.c` | the plugin: counts and times the per-tick GameState function |
| `my_plugin.ini.example` | its config with the defaults (copy to `binaries/v3x/my_plugin.ini`) |
| `test_my_plugin.c` | offline test: callbacks, the bounded status line, config reload, and the real anchor code against `victoria3.exe` mapped read-only |

## Build, test, install

From the repository root (needs `pip install ziglang`):

```sh
python sdk/build_plugin.py templates/plugin_template --werror                 # build/plugins/my_plugin.dll
python sdk/build_plugin.py templates/plugin_template --werror --test ../binaries/victoria3.exe
python sdk/build_plugin.py templates/plugin_template --werror --test --install
python sdk/build_plugin.py check build/plugins/my_plugin.dll                  # will the installed loader + game accept it?
```

Expected end of the test output on 1.15.1: `9 checks, 0 failed` and `ALL OK`. Add `--sanitize` to
build the offline test with UBSan in trap mode (undefined behaviour = test crash).

`check` reads the DLL without loading it: exports, manifest, `min_api` against the installed
loader's API version, `game_builds` against the installed `victoria3.exe` (including the loader's
unknown-build safe mode after a game patch). Exit code 0 = compatible. With a v5 loader the
template also shows leveled logging (`[log] my_plugin = debug` in `v3x/v3x.ini`) and re-reads its
ini on `reload my_plugin` (docs/api-reference.md).

The plain compiler line, if you prefer it (the same hardening and warnings as `build_plugin.py`):

```sh
python -m ziglang cc -target x86_64-windows-gnu -O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong \
    -ftrivial-auto-var-init=zero -fno-delete-null-pointer-checks \
    -Wall -Wextra -Wformat=2 -Wvla -Wshadow -Wno-unused-function -Werror -shared \
    -Iinclude -Isdk -o build/plugins/my_plugin.dll templates/plugin_template/my_plugin.c
```

## Make it yours

1. Copy the folder to `plugins/<name>/` and rename `my_plugin` in the file names, the `TAG`, the
   manifest and the ini section.
2. Replace the anchor: find a string your target function references (`tools/pestrings.py`,
   `tools/pexref.py`, see `docs/reverse-engineering.md`), check the prologue bytes, and keep
   `V3X_ANCHOR_ALL_SAME` or use `V3X_ANCHOR_SHAPE` if several functions use the string. Keep a
   `check` callback that proves the shape.
3. Keep the manifest's capabilities to what you use (`"hook"` here; add `"patch"` or `"exec"` only
   if you really call `api->patch` / `api->exec_command`).
4. Read engine memory only through `sdk/v3x_mem.h`, build strings with `V3xStr`, parse files with
   `V3xSpan`, and join paths from files with `v3x_path_join_checked` (docs/safe-coding.md).
5. Keep `bash tools/check_unsafe.sh templates plugins/<name>` clean.
6. Run the offline test until it passes, then smoke-test in the game (load a save, play a few
   days, save, quit) with a backup of `binaries/v3x/*.log` from the previous run.

Read `docs/safe-coding.md` and `docs/safety.md` before you change game state from a callback.
