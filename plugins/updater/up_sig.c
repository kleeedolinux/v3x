/* up_sig.c - release signature check for the updater (docs/security-audit.md H1): compat.json.sig
 * must be an Ed25519 signature by a key of installer/release_keys.h over the exact compat.json
 * bytes. This unit compiles Monocypher (third_party/monocypher) through installer/release_sig.h. */
#define V3X_RELEASE_SIG_IMPL
#include "../../installer/release_sig.h"
#include "up_logic.h"

int up_sig_check(const char *json, size_t n, const char *sig, size_t sn, char key_id[17], const char **why)
{
    V3xSigResult r;
    int ok = v3x_release_verify(json, n, sig, sn, &r);
    if (key_id) memcpy(key_id, r.key_id, sizeof r.key_id);
    if (why) *why = r.why;
    return ok ? r.slot : -1;
}

int up_sig_keys(void) { return v3x_release_keys_configured(); }
