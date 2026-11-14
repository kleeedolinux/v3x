/* core_sig.c - the one translation unit of version.dll that compiles Monocypher (Ed25519, CC0 / BSD-2,
 * third_party/monocypher) through installer/release_sig.h. Kept apart from core.c so Monocypher's
 * file-scope names cannot collide with the core's. core.c includes release_sig.h without
 * V3X_RELEASE_SIG_IMPL and calls v3x_release_verify (verification of plugins.json.sig, ADR 0002). */
#define V3X_RELEASE_SIG_IMPL
#include "../installer/release_sig.h"
