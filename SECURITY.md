# Security policy

V3X is a native code loader for Victoria 3 (single player). It runs inside the game process with the
game's rights. This page says what is supported, how to report a problem, and what V3X does and does
not defend against. Decisions are in [docs/adr/](docs/adr/).

## Supported versions

| version | supported |
|---|---|
| latest release (`releases/latest`) | yes: fixes ship as a new release |
| older releases | no: update (the installer and `install.ps1` always take the latest) |
| `main` / CI dev builds (`ci-<sha>`, `dev-YYYYMMDD`) | best effort, not published as releases |

Only builds from the official GitHub releases of this repository, whose `compat.json` carries a valid
release signature ([ADR 0001](docs/adr/0001-release-signing.md)), are supported.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem.

* Use GitHub's private vulnerability reporting: repository **Security** tab, **Report a vulnerability**.
* Include: V3X version (`version` console command or `binaries/v3x/v3x.log` first line), game build,
  what an attacker needs (a mod? a file in the game folder? network position?), and steps or a
  proof of concept. A crash input from `test/fuzz/` (`crash-*.bin`) is ideal.
* You get an answer within 7 days. A fix for a confirmed issue ships as a new release; the advisory
  is published with it and credits you unless you ask otherwise.

## Threat model (summary)

| boundary | trust |
|---|---|
| `binaries/version.dll`, `binaries/v3x/plugins/*.dll` | fully trusted native code: every DLL in the plugin folder runs with the game's rights |
| files under `binaries/v3x/` (ini, caches, indexes, markers) | trusted configuration; parsers are still bounds-checked and fuzzed |
| Workshop / local mods | untrusted **content**: mod files must never be able to run code, write outside their place or crash the loader |
| network (GitHub releases) | transport only; authenticity comes from the Ed25519 release signature, not from TLS or SHA-256 sums on the same page |
| local devbridge pipe | same-user, development only, not shipped by default |

In scope:

* a malicious mod (content only) that gets V3X or a bundled plugin to crash, corrupt saves, run code,
  or read/write files outside the game folder;
* a tampered or swapped release (installer, updater, `install.ps1`);
* path traversal, reparse-point (symlink / junction) and device-name tricks in anything V3X reads
  from disk or a release;
* memory-safety bugs in the loader, the SDK headers and the bundled plugins' parsers;
* crashes and save corruption caused by V3X (stability is treated as security).

## Out of scope

* **Local cheating** in single player (console commands in ironman, editing your own saves or
  game memory): V3X is a modding tool; doing that is the point, not an attack.
* **Multiplayer**: V3X is not designed or supported for multiplayer; desyncs or advantages there are
  not security issues.
* A malicious **plugin DLL** you installed yourself: plugins are native code with full rights by
  design (there is no sandbox). Install plugins only from sources you trust.
* An attacker who already runs code as your user (they can inject into the game without V3X), unless
  V3X gives them persistence or a way across users or into elevation.
* Problems in the game itself, Steam, Windows or third-party tools.

## Hardening in place

Bounded string and buffer helpers (`sdk/v3x_safe.h`, [ADR 0004](docs/adr/0004-sdk-safety.md)), a CI
ban on the unbounded C idioms (`tools/check_unsafe.sh`), stack protector + fortify + zero-initialised
locals + `-Werror` for the core and SDK, UBSan-trap test builds, an in-tree fuzzer over every
on-disk parser (`test/fuzz/`), W^X hook stubs and never-freed trampolines
([ADR 0003](docs/adr/0003-hook-lifetime.md)), a proxy that fails closed, and signed releases verified
before anything runs ([ADR 0001](docs/adr/0001-release-signing.md), [ADR 0002](docs/adr/0002-trust-model.md)).
