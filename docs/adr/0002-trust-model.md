# ADR 0002: Trust model: only signed or allowed plugins load, everything they read is untrusted

Status: accepted (2026-11-13), revised for loader 0.6.0 / API v6. The first version (loader 0.5.1)
treated every DLL in `binaries/v3x/plugins/` as trusted; this revision replaces that.

## Context

V3X loads native DLLs into the game. The game folder is writable for `Authenticated Users` (Steam's
default), so any program, installer or archive that can write there could drop a DLL into
`binaries/v3x/plugins/`, and before 0.6.0 the loader would run it on the next start. V3X also reads
many files (`v3x.ini`, plugin inis, caches, markers, crash logs, mod files, release manifests); some come
from Workshop mods, some from the network.

Two questions have to be answered separately:

1. **Which plugin DLLs may run at all?** (authenticity and consent)
2. **What may a running plugin do?** (capabilities, ownership, quotas; see
   [ADR 0005](0005-api-v6-containment.md))

## Decision

### 1. A plugin DLL loads only if it is official, allowed by the player, or the player is a developer

The loader hashes every plugin file (SHA-256) and decides **before `LoadLibrary`**. A DLL is loaded if
one of these holds (checked in this order, `src/core.c` `trust_of`):

| trust | rule | who decides |
|---|---|---|
| **official** | `binaries/v3x/plugins.json` lists the DLL's file name with this SHA-256, **and** `binaries/v3x/plugins.json.sig` is a valid Ed25519 signature over the exact bytes of `plugins.json` made with the release key ([ADR 0001](0001-release-signing.md), `installer/release_sig.h`) | the V3X release; `V3X-Setup.exe` installs both files |
| **trusted (allowlist)** | the SHA-256 is a line in the per-user file `%LOCALAPPDATA%\V3X\trusted_plugins.txt` (`<sha256> <file name>`, `#` comments; only the hash counts) | the player, with the console command `trust <plugin>` |
| **dev (unchecked)** | the environment variable `V3X_TRUST=dev` is set for the game process | a developer, for building and testing plugins |

Anything else is **not loaded**: it is never mapped, so neither its `DllMain` nor its static
constructors run. The console says so at start, `plugins` shows it as
`NOT LOADED: untrusted (type 'trust <plugin>' to allow it)`, and `trust` lists every refused plugin
with its hash. `trust <plugin>` appends exactly the hash that was refused this start, so what the player
allows is the file that was checked; it takes effect at the next start. Rebuilding a plugin changes its
hash, so the player has to allow it again.

An unsigned, missing or invalid `plugins.json.sig` means the official list is ignored (logged as a
WARNING), never trusted. Development and CI builds are unsigned, which is why the developer switch
exists. There is no trust switch in `v3x.ini`: that file lives in the writable game folder, so a key
there would let any program that drops a DLL also switch the check off.

### 2. The file that was checked is the file that is mapped

The loader opens the DLL once, without write or delete sharing, reads it through that handle, hashes and
parses those bytes, and keeps the handle open until `LoadLibraryW` returned. A program that swaps the
file between the check and the load gets a sharing violation instead.

### 3. Every load decision is made on the file, before any plugin code runs

`src/v3x_pe.h` reads the PE32+ file with bounded reads (`V3xSpan`) and extracts the exports
`v3x_plugin_load`, `v3x_plugin_name` and `v3x_plugin_manifest` (with its strings) without mapping the
DLL. These checks all run before `LoadLibrary`, so a refused plugin never executes:

* `[plugins] <name> = 0` in `v3x.ini`; the crash-loop guard (`loading.marker`);
* the trust check above;
* a malformed PE file or a missing `v3x_plugin_load` export;
* the manifest: `min_api` above the loader's API, `V3X_MANIFEST_REQUIRE_BUILD` on a build not listed,
  the unknown-build gate.

Consequence: the manifest must be a compile-time constant (the `V3X_PLUGIN_MANIFEST*` macros do that).

### 4. Capabilities: enforced where the loader is the only way, advisory elsewhere

A manifest may declare `capabilities` (API v6, `V3X_PLUGIN_MANIFEST_CAPS`): a subset of
`hook patch fs net script exec`.

* **Enforced** (the loader owns the mechanism): `hook` (`hook`, `hook_safe`, `unhook`), `patch`
  (`patch`), `exec` (`exec_command`, `exec_capture`). A plugin that declares capabilities and calls one
  of these without the word gets `V3X_EPERM` (`patch`: `ERROR_ACCESS_DENIED`), logged once.
* **Advisory** (shown by `plugins`, not enforceable): `fs`, `net`, `script`. A native DLL can call the
  OS directly; the loader cannot stop it.
* Undeclared (`NULL`, or a manifest built with an older header) = everything allowed, as before.

Declaring capabilities limits what a bug or a hijacked code path inside a trusted plugin can do through
the API. It is not a promise about the plugin's own code.

### 5. Everything a plugin reads is untrusted input

Unchanged from the first version: mod content, release assets (until the signature verifies), and every
file V3X reads, including its own caches and the files under `binaries/v3x/`, are untrusted. Every
parser is bounds-checked (`sdk/v3x_safe.h`), fuzzed (`test/fuzz/`), and never takes a path from a file
without `v3x_path_join_checked`. Caches carry magic + version + length and are rebuilt on any error.
Text from mods is sanitised (`v3x_sanitize_line`) before it reaches `v3x.log`. The loader's own parsers
for `plugins.json`, the allowlist, capabilities and patterns are in `src/core_parse.h`, plain C without
state, so they can be tested and fuzzed on their own.

## Honest limits

This is **authenticity and consent, not a sandbox.**

* A trusted plugin is native code with the game's rights. It can read and write any file your Windows
  user can, open network connections and patch memory without asking the loader. Trust only plugins
  whose author you trust.
* Anything already running as your Windows user can edit `%LOCALAPPDATA%\V3X\trusted_plugins.txt` or
  start the game with `V3X_TRUST=dev`. The check protects against DLLs dropped into the game folder,
  not against malware that already runs as you.
* DLLs that a plugin loads or imports itself are not checked.
* An older, validly signed `plugins.json` lists older official DLLs; the signature does not prevent
  going back to an older official release.
* `V3X_TRUST=dev` switches the check off for **every** plugin DLL in that game process. It is for
  development machines only.
* Fault isolation (SEH guards, rollback, `plugin_disabled`) exists for stability, not as a security
  boundary.
* Out of scope: local cheating and multiplayer (see `SECURITY.md`).

## Consequences

* Players who install with `V3X-Setup.exe` see no difference: the bundled plugins are official.
* A plugin from another source needs one `trust <plugin>` per version, on purpose: the player sees
  which file they allow.
* Plugin developers set `V3X_TRUST=dev` while they iterate ([getting-started.md](../getting-started.md)
  step 4).
* A malicious mod can at worst be refused or produce wrong game data; it must not get code execution,
  file writes outside its place, or a loader crash. A bug that allows that is a vulnerability.
* New parsers must come with a `test/fuzz/fuzz_<name>.c` harness and use the safe helpers.
