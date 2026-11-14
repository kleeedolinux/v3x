# ADR 0001: Releases are authenticated by an Ed25519 signature over compat.json

Status: accepted (2026-11-13). Audit finding H1, actions 1, 2 and 6 (`docs/security-audit.md`).

## Context

`install.ps1` and the updater download `V3X-Setup.exe` and run it. Before this decision the only check
was a SHA-256 from `compat.json` / `SHA256SUMS.txt` published **next to** the binary. Anyone who can
replace release assets (a leaked token, a compromised CI dependency, a hijacked pages project) replaces
the binary and its hashes together, and the updater would install it automatically. TLS only protects
the download, not who made the file.

## Decision

* The release workflow signs `compat.json` with an **Ed25519** key held only as a protected GitHub
  environment secret (`V3X_SIGNING_KEY`, environment `release`); the signature is the asset
  `compat.json.sig`. `compat.json` names the size and SHA-256 of every asset.
* The public keys are compiled in: `installer/release_keys.h` (CURRENT + NEXT rotation slot), used by
  `V3X-Setup.exe --verify-release` and the updater (`installer/release_sig.h`, Monocypher), and the
  same two values are in `install.ps1`. A test fails if they differ.
* Every consumer verifies the signature **before** trusting any hash, then the hash before running
  anything. No valid signature (including "no key configured yet") = nothing runs; the updater does
  not install automatically.
* Releases are immutable (no `--clobber` re-uploads); build and publish are separate CI jobs.

## Consequences

* A swapped asset set without the private key is refused everywhere.
* Key rotation: publish a release that carries the next key in NEXT first, then switch the secret,
  then move NEXT to CURRENT. A lost key without a NEXT key means users must reinstall manually once.
* Until the first key is configured no release verifies: that is intended (fail closed).
* The private key must never enter the repository or a workflow log; only the `release` environment
  can read it.
