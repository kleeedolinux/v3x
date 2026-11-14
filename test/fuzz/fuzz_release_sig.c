/* Fuzz harness: installer/release_sig.h - the release signature line parser (v3x_sig_parse) and the
 * whole check (v3x_release_verify) used by V3X-Setup.exe --verify-release and plugins/updater.
 * The build trusts the public TEST key (tools/sign_release.py TEST_SEED; never a release key) so the
 * Ed25519 path really runs. Invariants: a parsed line is exactly "v3x-sig 1 <16 hex> <88 base64>" with
 * canonical base64 that re-encodes to the same text; only the one known (message, signature) pair
 * verifies - any mutation of either that still verifies is a forgery = crash. windows.h-free (also a
 * libFuzzer/ASan target on Linux, .github/workflows/ci.yml). */
#define V3X_RELEASE_KEY_DEVTEST "b397fa20bfc6bba89cd5e467041d6fce492345f3aa90d3fccda438bba02c738b"
#define V3X_RELEASE_SIG_IMPL
#include "v3x_fuzz.h"
#include "../../installer/release_sig.h"

/* signed with TEST_SEED by tools/sign_release.py */
static const char k_msg[] = "{\"schema\":1,\"v3x_version\":\"0.5.1\"}\n";
static const char k_sig[] = "v3x-sig 1 c91e9e3c88025a36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCQ==\n";

static void b64_64(const uint8_t in[64], char out[89])
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int k = 0;
    for (int i = 0; i < 63; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        out[k++] = a[v >> 18]; out[k++] = a[(v >> 12) & 63]; out[k++] = a[(v >> 6) & 63]; out[k++] = a[v & 63];
    }
    uint32_t v = (uint32_t)in[63] << 16;
    out[k++] = a[v >> 18]; out[k++] = a[(v >> 12) & 63]; out[k++] = '='; out[k++] = '=';
    out[k] = 0;
}

static void fuzz_parse(const uint8_t *d, size_t n)
{
    const char *t = (const char *)v3x_fuzz_guarded(d, n);   /* no NUL: the parser must respect n */
    char kid[17];
    uint8_t sig[64];
    if (!v3x_sig_parse(t, n, kid, sig)) {
        V3X_FUZZ_ASSERT(kid[0] == 0);
        return;
    }
    V3X_FUZZ_ASSERT(strlen(kid) == 16);
    for (int i = 0; i < 16; i++) V3X_FUZZ_ASSERT((kid[i] >= '0' && kid[i] <= '9') || (kid[i] >= 'a' && kid[i] <= 'f'));
    size_t m = n;
    while (m && (t[m - 1] == '\n' || t[m - 1] == '\r')) m--;
    V3X_FUZZ_ASSERT(m == 10 + 16 + 1 + 88 && !memcmp(t, "v3x-sig 1 ", 10) && !memcmp(t + 10, kid, 16));
    char re[89];
    b64_64(sig, re);
    V3X_FUZZ_ASSERT(!memcmp(t + 27, re, 88));   /* canonical: one text per signature */
}
V3X_FUZZ_TARGET(sig_parse, fuzz_parse)
V3X_FUZZ_SEED(sig_parse, "v3x-sig 1 c91e9e3c88025a36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCQ==\n")
V3X_FUZZ_SEED(sig_parse, "v3x-sig 1 c91e9e3c88025a36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCQ==\r\n")
V3X_FUZZ_SEED(sig_parse, "v3x-sig 1 C91E9E3C88025A36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCR==")
V3X_FUZZ_SEED(sig_parse, "v3x-sig 2 0000000000000000 ")

/* input = message bytes, then 0xFF, then the signature line (no 0xFF: the whole input is the line and
 * the message is the known one) */
static void fuzz_verify(const uint8_t *d, size_t n)
{
    const uint8_t *sep = (const uint8_t *)memchr(d, 0xFF, n);
    const uint8_t *msg = (const uint8_t *)k_msg;
    size_t mn = sizeof k_msg - 1;
    const char *sig;
    size_t sn;
    if (sep) {
        msg = (const uint8_t *)v3x_fuzz_guarded(d, (size_t)(sep - d));
        mn = (size_t)(sep - d);
        sn = n - mn - 1;
        sig = (const char *)v3x_fuzz_guarded(sep + 1, sn);
    } else {
        sig = (const char *)v3x_fuzz_guarded(d, n);
        sn = n;
    }
    static int self_checked;
    if (!self_checked) {   /* the known pair verifies (else the harness would only test rejections) */
        V3xSigResult k;
        V3X_FUZZ_ASSERT(v3x_release_verify(k_msg, sizeof k_msg - 1, k_sig, sizeof k_sig - 1, &k) == 1 && k.slot == V3X_SIG_SLOT_DEVTEST);
        self_checked = 1;
    }
    V3xSigResult r;
    int ok = v3x_release_verify(msg, mn, sig, sn, &r);
    V3X_FUZZ_ASSERT(r.why && (ok ? r.slot == V3X_SIG_SLOT_DEVTEST : r.slot == -1));
    if (ok) {   /* the only valid pair: anything else that verifies is a forgery */
        size_t ks = sizeof k_sig - 1, t = sn;
        while (t && (sig[t - 1] == '\n' || sig[t - 1] == '\r')) t--;
        V3X_FUZZ_ASSERT(mn == sizeof k_msg - 1 && !memcmp(msg, k_msg, mn) && t == ks - 1 && !memcmp(sig, k_sig, t));
    }
}
V3X_FUZZ_TARGET(sig_verify, fuzz_verify)
V3X_FUZZ_SEED(sig_verify, "v3x-sig 1 c91e9e3c88025a36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCQ==\n")
V3X_FUZZ_SEED(sig_verify, "{\"schema\":1,\"v3x_version\":\"0.5.1\"}\n\xFFv3x-sig 1 c91e9e3c88025a36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCQ==\n")
V3X_FUZZ_SEED(sig_verify, "{\"schema\":1,\"v3x_version\":\"0.5.2\"}\n\xFFv3x-sig 1 c91e9e3c88025a36 z2MCC1HleelRECtbiVhG6ZmJjvlWVHe9dOwmwj0ZUvi/OvMIz+dhad2ShVd5Bo5F8uTTAOh20k2w2hwqJy4qCQ==\n")
V3X_FUZZ_SEED(sig_verify, "\xFFv3x-sig 1 ffffffffffffffff AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\n")
