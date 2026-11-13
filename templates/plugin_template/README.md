# Plugin template

A minimal plugin that follows every V3X safety rule: a manifest, an optional ini, a
fail-closed anchor, an observe-only `hook_safe` hook, a console command and an offline test.

| file | what |
|---|---|
| `my_plugin.c` | the plugin: counts and times the per-tick GameState function |
| `my_plugin.ini.example` | its config with the defaults (copy to `binaries/v3x/my_plugin.ini`) |
| `test_my_plugin.c` | offline test: callbacks, and the real anchor code against `victoria3.exe` mapped read-only |

## Build, test, install

From the repository root (needs `pip install ziglang`):

```sh
python sdk/build_plugin.py templates/plugin_template                 # build/plugins/my_plugin.dll
python sdk/build_plugin.py templates/plugin_template --test          # + offline test against the game exe
python sdk/build_plugin.py templates/plugin_template --test --install
```

The plain compiler line, if you prefer it:

```sh
python -m ziglang cc -target x86_64-windows-gnu -O2 -Wno-unused-function -shared \
    -Iinclude -Isdk -o build/plugins/my_plugin.dll templates/plugin_template/my_plugin.c
```

## Make it yours

1. Copy the folder to `plugins/<name>/` and rename `my_plugin` in the file names, the `TAG`, the
   manifest and the ini section.
2. Replace the anchor: find a string your target function references (`tools/pestrings.py`,
   `tools/pexref.py`, see `docs/reverse-engineering.md`), check the prologue bytes, and keep
   `V3X_ANCHOR_ALL_SAME` or use `V3X_ANCHOR_SHAPE` if several functions use the string.
3. Run the offline test until it passes, then smoke-test in the game (load a save, play a few
   days, save, quit) with a backup of `binaries/v3x/*.log` from the previous run.

Read `docs/safety.md` before you change game state from a callback.
