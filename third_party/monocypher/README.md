# Monocypher 4.0.3 (vendored, unmodified)

Used for Ed25519 release-signature verification only (installer/release_sig.h: V3X-Setup.exe
--verify-release and plugins/updater/up_sig.c).

Files copied verbatim from the official release tarball:
src/monocypher.c, src/monocypher.h, src/optional/monocypher-ed25519.c, src/optional/monocypher-ed25519.h,
LICENCE.md (dual licence: CC0-1.0 or BSD-2-Clause; V3X uses it under either).

Source: https://monocypher.org/download/monocypher-4.0.3.tar.gz (fetched 2026-11-13)
- SHA-512 (published at https://monocypher.org/download/monocypher-4.0.3.tar.gz.sha512, matched):
  40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965
- SHA-256 (GitHub release asset digest of LoupVaillant/Monocypher 4.0.3, matched):
  8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66

Both files are compiled in one translation unit by installer/release_sig.h (V3X_RELEASE_SIG_IMPL),
which renames the one private helper they share (hash_reduce) and silences -Wshadow around them.
