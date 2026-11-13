/* release_sig.h - verifies a V3X release signature (compat.json.sig) with the keys of
 * installer/release_keys.h. Header-only, no allocation, no windows.h; used by installer/setup.c
 * (V3X-Setup.exe --verify-release) and plugins/updater/up_sig.c.
 *
 * Signature file (tools/sign_release.py writes it): one ASCII line
 *     v3x-sig 1 <key id: 16 lowercase hex> <base64 of the 64-byte Ed25519 signature>[\r][\n]
 * over the exact bytes of compat.json. key id = first 8 bytes of SHA-512(public key).
 *
 * Exactly one translation unit per program defines V3X_RELEASE_SIG_IMPL before including this file:
 * it compiles Monocypher (third_party/monocypher, CC0 / BSD-2) into that unit.
 *
 * Extra trusted key for local dry runs and tests only: build with
 *   -DV3X_RELEASE_KEY_DEVTEST=\"<64 hex>\"   (installer/make_dist.py --trust-key); never in CI. */
#ifndef V3X_RELEASE_SIG_H
#define V3X_RELEASE_SIG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "release_keys.h"

#ifdef V3X_RELEASE_SIG_IMPL
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "../third_party/monocypher/monocypher.c"
/* both Monocypher files in one unit (so every user builds with one compiler call): the two names
 * they share privately are separated here; the vendored files stay unmodified */
#undef FOR
#define hash_reduce v3x_mc_ed25519_hash_reduce_
#include "../third_party/monocypher/monocypher-ed25519.c"
#undef hash_reduce
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
#include "../third_party/monocypher/monocypher-ed25519.h"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define V3X_SIG_UNUSED_ __attribute__((unused))
#else
#define V3X_SIG_UNUSED_
#endif

enum { V3X_SIG_SLOT_CURRENT = 0, V3X_SIG_SLOT_NEXT = 1, V3X_SIG_SLOT_DEVTEST = 2, V3X_SIG_NSLOTS = 3 };

typedef struct V3xSigResult {
    int slot;              /* V3X_SIG_SLOT_* of the key that verified, -1 = none */
    char key_id[17];       /* key id named in the signature file ("" if unreadable) */
    const char *why;       /* "ok" or the reason it failed (plain English, no secrets) */
} V3xSigResult;

static V3X_SIG_UNUSED_ const char *v3x_sig_key_hex_(int slot)
{
    switch (slot) {
    case V3X_SIG_SLOT_CURRENT: return V3X_RELEASE_KEY_CURRENT;
    case V3X_SIG_SLOT_NEXT: return V3X_RELEASE_KEY_NEXT;
#ifdef V3X_RELEASE_KEY_DEVTEST
    case V3X_SIG_SLOT_DEVTEST: return V3X_RELEASE_KEY_DEVTEST;
#endif
    default: return "";
    }
}

static V3X_SIG_UNUSED_ int v3x_sig_hexval_(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 1 = slot holds a well-formed public key (copied to out) */
static V3X_SIG_UNUSED_ int v3x_sig_key_(int slot, uint8_t out[32])
{
    const char *h = v3x_sig_key_hex_(slot);
    if (!h || strlen(h) != 64) return 0;
    for (int i = 0; i < 32; i++) {
        int a = v3x_sig_hexval_(h[2 * i]), b = v3x_sig_hexval_(h[2 * i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 1;
}

/* number of usable trusted keys compiled in */
static V3X_SIG_UNUSED_ int v3x_release_keys_configured(void)
{
    uint8_t k[32];
    int n = 0;
    for (int s = 0; s < V3X_SIG_NSLOTS; s++) n += v3x_sig_key_(s, k);
    return n;
}

static V3X_SIG_UNUSED_ int v3x_release_devtest_key(void)
{
    uint8_t k[32];
    return v3x_sig_key_(V3X_SIG_SLOT_DEVTEST, k);
}

static V3X_SIG_UNUSED_ void v3x_sig_key_id_(const uint8_t pub[32], char out[17])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t h[64];
    crypto_sha512(h, pub, 32);
    for (int i = 0; i < 8; i++) { out[2 * i] = hexd[h[i] >> 4]; out[2 * i + 1] = hexd[h[i] & 15]; }
    out[16] = 0;
}

static V3X_SIG_UNUSED_ int v3x_sig_b64val_(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* exactly 88 chars = 86 base64 digits + "==" -> 64 bytes; canonical (unused bits zero). 1 ok */
static V3X_SIG_UNUSED_ int v3x_sig_b64_64_(const char *s, uint8_t out[64])
{
    if (s[86] != '=' || s[87] != '=') return 0;
    uint32_t acc = 0;
    int bits = 0, k = 0;
    for (int i = 0; i < 86; i++) {
        int v = v3x_sig_b64val_(s[i]);
        if (v < 0) return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k >= 64) return 0;
            out[k++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1u;
        }
    }
    return k == 64 && acc == 0;
}

/* Parses the signature line. 1 ok. */
static V3X_SIG_UNUSED_ int v3x_sig_parse(const char *t, size_t n, char key_id[17], uint8_t sig[64])
{
    static const char pre[] = "v3x-sig 1 ";
    const size_t pn = sizeof pre - 1, need = pn + 16 + 1 + 88;
    key_id[0] = 0;
    if (!t) return 0;
    while (n && (t[n - 1] == '\n' || t[n - 1] == '\r')) n--;   /* one line, optional CRLF / LF */
    if (n != need || memcmp(t, pre, pn) || t[pn + 16] != ' ') return 0;
    for (size_t i = 0; i < 16; i++) {
        char c = t[pn + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
        key_id[i] = c;
    }
    key_id[16] = 0;
    if (!v3x_sig_b64_64_(t + pn + 17, sig)) { key_id[0] = 0; return 0; }
    return 1;
}

/* 1 = sig (the text of compat.json.sig) is a valid signature over msg by a trusted key. */
static V3X_SIG_UNUSED_ int v3x_release_verify(const void *msg, size_t n, const char *sig, size_t sn, V3xSigResult *r)
{
    V3xSigResult tmp;
    if (!r) r = &tmp;
    r->slot = -1;
    r->key_id[0] = 0;
    r->why = "no trusted release key in this build";
    uint8_t s[64], pub[32];
    char kid[17];
    if (!msg && n) { r->why = "no data"; return 0; }
    if (!v3x_sig_parse(sig, sn, kid, s)) { r->why = "malformed signature file"; return 0; }
    memcpy(r->key_id, kid, sizeof kid);
    if (!v3x_release_keys_configured()) return 0;
    for (int slot = 0; slot < V3X_SIG_NSLOTS; slot++) {
        char id[17];
        if (!v3x_sig_key_(slot, pub)) continue;
        v3x_sig_key_id_(pub, id);
        if (strcmp(id, kid)) continue;
        if (crypto_ed25519_check(s, pub, (const uint8_t *)msg, n) != 0) { r->why = "BAD signature"; return 0; }
        r->slot = slot;
        r->why = "ok";
        return 1;
    }
    r->why = "signed with an unknown key";
    return 0;
}

#endif
