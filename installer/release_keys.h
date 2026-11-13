/* release_keys.h - the Ed25519 public keys that sign V3X releases (compat.json.sig).
 *
 * Shared by installer/setup.c (V3X-Setup.exe --verify-release) and plugins/updater (through
 * installer/release_sig.h). installer/install.ps1 carries the same two values in $ReleaseKeys;
 * installer/test_installer.py fails when they differ.
 *
 * CURRENT = the key the release workflow signs with (GitHub secret V3X_SIGNING_KEY, environment
 * "release"). NEXT = rotation slot: put the next key here one release BEFORE switching the secret,
 * so installed updaters already trust it; after the switch move it to CURRENT and empty NEXT.
 * Empty string = slot unused. 64 lowercase hex digits = 32-byte public key.
 * How to make a key: docs/updating.md "Release signing". Never commit a private key (.seed.b64).
 *
 * While CURRENT is empty no release verifies: install.ps1 refuses to install and the updater never
 * installs automatically. */
#ifndef V3X_RELEASE_KEYS_H
#define V3X_RELEASE_KEYS_H

#define V3X_RELEASE_KEY_CURRENT ""
#define V3X_RELEASE_KEY_NEXT ""

#endif
