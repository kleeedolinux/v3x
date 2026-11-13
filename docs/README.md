# V3X documentation

| document | for |
|---|---|
| [getting-started.md](getting-started.md) | install V3X, build and run your first plugin in 10 minutes |
| [api-reference.md](api-reference.md) | every field of `V3X_API` (API v1-v4), thread rules, manifest, events |
| [safety.md](safety.md) | the rules that keep the game alive (each one learned from a crash) |
| [reverse-engineering.md](reverse-engineering.md) | the RE workflow and every tool in `tools/`, devtools, MCP bridge |
| [engine-map.md](engine-map.md) | what is known about the engine: tick, threads, memory, rendering, loading, saves, console, VFS |
| [plugins.md](plugins.md) | catalog of the 25 bundled plugins and their status |
| [configuration.md](configuration.md) | `v3x.ini` and every plugin's ini keys with defaults |
| [console-commands.md](console-commands.md) | every V3X console command and the events between plugins |
| [testing.md](testing.md) | build, repository layout, every offline and end-to-end test |
| [troubleshooting.md](troubleshooting.md) | reading `v3x.log` and `crash.log`, hangs, build problems |
| [faq.md](faq.md) | common questions |
| [re/](re/) | detailed reverse-engineering notes per topic (1.15.1) |

SDK: `include/` (the API), `sdk/` (helper headers and `build_plugin.py`),
`templates/plugin_template/` (a complete minimal plugin with an offline test).
