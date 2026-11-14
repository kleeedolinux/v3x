# ADR 0002: Trust model: plugins are trusted code, everything they read is not

Status: accepted (2026-11-13). Audit "Threat model", findings H2, L5, L6, L10, L14.

## Context

V3X loads every DLL in `binaries/v3x/plugins/` into the game. It also reads many files: `v3x.ini`,
plugin inis, caches, indexes, markers, crash logs, mod files (localisation, DDS, descriptors), release
manifests. Some of those come from Workshop mods (anyone can publish), some from the game folder (which
Steam makes writable for `Authenticated Users`), some from the network.

## Decision

* **Trusted:** the loader (`version.dll`) and plugin DLLs. They are native code with the game's rights;
  V3X does not sandbox, sign-check or allowlist plugins. Fault isolation (SEH guards, rollback) exists
  for stability, not as a security boundary.
* **Untrusted input, always:** mod content, release assets (until the signature verifies, ADR 0001),
  and every file V3X reads, including its own caches and the files under `binaries/v3x/`. Every parser
  is bounds-checked (`sdk/v3x_safe.h`), fuzzed (`test/fuzz/`), and never takes a path from a file
  without `v3x_path_join_checked` (no `..`, no `:`/absolute, no device names, reparse points refused
  where a file is written or deleted). Caches carry magic + version + length and are rebuilt on any
  error; a path from a cache is never trusted.
* Text from mods is sanitised (`v3x_sanitize_line`) before it reaches `v3x.log`, because tools parse
  that log.
* Out of scope: local cheating and multiplayer (see `SECURITY.md`).

## Consequences

* A malicious mod can at worst be refused or produce wrong game data; it must not get code execution,
  file writes outside its place, or a loader crash. A bug that allows that is a vulnerability.
* A malicious plugin DLL is out of scope by design; documentation tells players to install plugins
  only from trusted sources.
* New parsers must come with a `test/fuzz/fuzz_<name>.c` harness and use the safe helpers.
