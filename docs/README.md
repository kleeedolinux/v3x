# V3X documentation

| document | for |
|---|---|
| [install.md](install.md) | players: install, repair, update and uninstall V3X with `V3X-Setup.exe` |
| [getting-started.md](getting-started.md) | install V3X, build, trust and run your first plugin in 10 minutes |
| [safe-coding.md](safe-coding.md) | **writing safe plugins**: the rules and the safe SDK API for each one; read before the reference |
| [api-reference.md](api-reference.md) | every field of `V3X_API` (safe usage first, unsafe functions marked), thread rules, trust, manifest, events |
| [api-stability.md](api-stability.md) | what stays the same across loader and game updates; the plugin release checklist |
| [scripting.md](scripting.md) | ship game script, localization and GUI from a plugin (`script_ext`) |
| [safety.md](safety.md) | the rules that keep the game alive (each one learned from a crash) |
| [reverse-engineering.md](reverse-engineering.md) | the RE workflow and every tool in `tools/`, devtools, MCP bridge |
| [engine-map.md](engine-map.md) | what is known about the engine: tick, threads, memory, rendering, loading, saves, console, VFS |
| [plugins.md](plugins.md) | catalog of the bundled plugins: status class, defaults, crash history, tests, installer |
| [configuration.md](configuration.md) | `v3x.ini`, plugin trust and the allowlist, and every plugin's ini keys with defaults |
| [console-commands.md](console-commands.md) | every V3X console command and the events between plugins |
| [doctor.md](doctor.md) | the `doctor` health check and its fixes |
| [updating.md](updating.md) | how updates and signed releases work |
| [testing.md](testing.md) | build, repository layout, every test and how to run it |
| [troubleshooting.md](troubleshooting.md) | reading `v3x.log` and `crash.log`, hangs, build problems |
| [faq.md](faq.md) | common questions |
| [re/](re/) | detailed reverse-engineering notes per topic (1.15.1) |

Design decisions (ADRs):

| ADR | decision |
|---|---|
| [0001](adr/0001-release-signing.md) | releases are authenticated by an Ed25519 signature |
| [0002](adr/0002-trust-model.md) | trust model: only signed or player-allowed plugins load; everything they read is untrusted |
| [0003](adr/0003-hook-lifetime.md) | hook lifetime: trampolines are never freed; W^X hook stubs |
| [0004](adr/0004-sdk-safety.md) | SDK safety: bounded helpers, checked builds, fuzzing |
| [0005](adr/0005-api-v6-containment.md) | API v6: ownership, capabilities, quotas, patch rules, removal |

SDK: `include/` (the API), `sdk/` (helper headers and `build_plugin.py`),
`templates/plugin_template/` (a complete minimal plugin with an offline test).
