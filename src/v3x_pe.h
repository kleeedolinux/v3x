/* v3x_pe.h - what the loader (src/core.c) reads from a plugin DLL FILE before it calls LoadLibrary
 * (audit N4): SHA-256 of the bytes and a bounded PE32+ reader for the exports v3x_plugin_load,
 * v3x_plugin_name and v3x_plugin_manifest (with the manifest's strings). Every read goes through
 * V3xSpan (sdk/v3x_safe.h); a malformed file is refused, never trusted. No windows.h, no state, so it is
 * unit-tested and fuzzed on its own (test/api_trust_test.c, test/fuzz/fuzz_core_pe.c).
 *
 * Why from the file: a mapped DLL has already run DllMain and its static constructors. A plugin the
 * loader refuses (min_api, REQUIRE_BUILD, unknown-build gate, trust check, no v3x_plugin_load) must
 * never execute, so the decision is made on the bytes. The loader holds the file open without write /
 * delete sharing from the read until LoadLibrary returned, so the bytes that were hashed and parsed are
 * the bytes that get mapped. */
#ifndef V3X_PE_H
#define V3X_PE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../sdk/v3x_safe.h"

/* ------------------------------------------------------------------ SHA-256 (FIPS 180-4) */
typedef struct V3xSha256 {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t fill;
} V3xSha256;

static const uint32_t v3x_sha256_k_[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define V3X_ROR32_(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void v3x_sha256_block_(V3xSha256 *c, const uint8_t *p)
{
    uint32_t w[64], a, b, d, e, f, g, h, cc;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = V3X_ROR32_(w[i - 15], 7) ^ V3X_ROR32_(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = V3X_ROR32_(w[i - 2], 17) ^ V3X_ROR32_(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (V3X_ROR32_(e, 6) ^ V3X_ROR32_(e, 11) ^ V3X_ROR32_(e, 25)) + ((e & f) ^ (~e & g)) + v3x_sha256_k_[i] + w[i];
        uint32_t t2 = (V3X_ROR32_(a, 2) ^ V3X_ROR32_(a, 13) ^ V3X_ROR32_(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void v3x_sha256_init(V3xSha256 *c)
{
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->fill = 0;
}

static void v3x_sha256_update(V3xSha256 *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += (uint64_t)n;
    while (n) {
        size_t k = 64 - c->fill;
        if (k > n) k = n;
        memcpy(c->buf + c->fill, p, k);
        c->fill += k; p += k; n -= k;
        if (c->fill == 64) { v3x_sha256_block_(c, c->buf); c->fill = 0; }
    }
}

static void v3x_sha256_final(V3xSha256 *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8u;
    uint8_t pad = 0x80, z = 0, len[8];
    v3x_sha256_update(c, &pad, 1);
    while (c->fill != 56) v3x_sha256_update(c, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    v3x_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

/* 64 lowercase hex digits + NUL */
static void v3x_sha256_hex(const void *data, size_t n, char out[65])
{
    static const char hx[] = "0123456789abcdef";
    V3xSha256 c;
    uint8_t d[32];
    v3x_sha256_init(&c);
    v3x_sha256_update(&c, data, n);
    v3x_sha256_final(&c, d);
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[64] = 0;
}

/* ------------------------------------------------------------------ PE32+ reader */

#define V3X_PE_STR_MAX 4096u   /* longest manifest / name string read (without NUL) */

typedef struct V3xPePlugin {
    int has_load;              /* exports v3x_plugin_load (a function RVA, not forwarded) */
    int has_manifest;          /* exports v3x_plugin_manifest */
    int has_name;              /* exports v3x_plugin_name and it points to a string */
    uint32_t mf_size, mf_flags, mf_min_api;
    int mf_has_caps;           /* struct_size covers capabilities */
    int mf_builds_null;        /* game_builds pointer is NULL (or not covered) */
    char plugin_name[64];      /* *v3x_plugin_name */
    char name[64], version[24], author[48], desc[112], builds[512], caps[128];   /* "" when NULL */
    const char *why;           /* reason when v3x_pe_read_plugin returns -1 */
} V3xPePlugin;

typedef struct V3xPeImg_ {
    const uint8_t *f;
    size_t n;
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t nsec;
    size_t sec_off;            /* file offset of the first section header */
} V3xPeImg_;

/* file offset of [rva, rva+len) if it lies inside one section's raw data, else (size_t)-1 */
static size_t v3x_pe_rva_off_(const V3xPeImg_ *im, uint32_t rva, size_t len)
{
    for (uint32_t i = 0; i < im->nsec; i++) {
        V3xSpan r;
        size_t o = im->sec_off + (size_t)i * 40u;
        if (o > im->n || im->n - o < 40) return (size_t)-1;
        v3x_span_init(&r, im->f + o, 40);
        uint32_t vsz, va, rawsz, rawp;
        (void)v3x_span_skip(&r, 8);
        (void)v3x_span_u32le(&r, &vsz); (void)v3x_span_u32le(&r, &va);
        (void)v3x_span_u32le(&r, &rawsz); (void)v3x_span_u32le(&r, &rawp);
        if (r.err) return (size_t)-1;
        if (rva < va) continue;
        uint64_t d = (uint64_t)rva - va;
        if (d >= rawsz) continue;                      /* not in this section's file data */
        if ((uint64_t)len > (uint64_t)rawsz - d) return (size_t)-1;
        uint64_t off = (uint64_t)rawp + d;
        if (off > im->n || (uint64_t)len > (uint64_t)im->n - off) return (size_t)-1;
        return (size_t)off;
    }
    return (size_t)-1;
}

/* NUL-terminated string at rva, copied (cut) into out. 1 ok, 0 not inside the file / no NUL within
 * V3X_PE_STR_MAX bytes. */
static int v3x_pe_str_rva_(const V3xPeImg_ *im, uint32_t rva, char *out, size_t cap)
{
    if (cap) out[0] = 0;
    size_t off = v3x_pe_rva_off_(im, rva, 1);
    if (off == (size_t)-1) return 0;
    size_t lim = im->n - off;
    if (lim > V3X_PE_STR_MAX + 1u) lim = V3X_PE_STR_MAX + 1u;
    const uint8_t *s = im->f + off;
    const uint8_t *z = (const uint8_t *)memchr(s, 0, lim);
    if (!z) return 0;
    size_t len = (size_t)(z - s);
    /* the whole string must lie in one section's file data */
    if (v3x_pe_rva_off_(im, rva, len + 1) == (size_t)-1) return 0;
    if (cap) {
        size_t k = len < cap - 1 ? len : cap - 1;
        memcpy(out, s, k);
        if (k < len) k = v3x_utf8_trim_tail(out, k, 0);   /* cut: never in the middle of a UTF-8 char */
        out[k] = 0;
    }
    return 1;
}

/* A pointer stored in the file (a VA relative to the preferred ImageBase) -> string. NULL pointer = "",
 * returns 1 and sets *was_null. 0 = points outside the image / unreadable. */
static int v3x_pe_str_va_(const V3xPeImg_ *im, uint64_t va, char *out, size_t cap, int *was_null)
{
    if (was_null) *was_null = 0;
    if (cap) out[0] = 0;
    if (!va) { if (was_null) *was_null = 1; return 1; }
    if (va < im->image_base || va - im->image_base >= im->size_of_image) return 0;
    return v3x_pe_str_rva_(im, (uint32_t)(va - im->image_base), out, cap);
}

static int v3x_pe_u64_rva_(const V3xPeImg_ *im, uint32_t rva, uint64_t *v)
{
    size_t off = v3x_pe_rva_off_(im, rva, 8);
    V3xSpan r;
    if (off == (size_t)-1) return 0;
    v3x_span_init(&r, im->f + off, 8);
    return v3x_span_u64le(&r, v) == V3X_R_OK;
}

/* Parses the plugin DLL in f[0..n). 0 = it is a PE32+ x64 DLL (fields filled), -1 = refuse (why set). */
static int v3x_pe_read_plugin(const uint8_t *f, size_t n, V3xPePlugin *out)
{
    V3xPeImg_ im;
    V3xSpan r;
    uint16_t mz = 0, machine = 0, nsec = 0, optsz = 0, chars = 0, magic = 0;
    uint32_t lfanew = 0, sig = 0, nrva = 0, exp_rva = 0, exp_size = 0;
    memset(out, 0, sizeof *out);
    memset(&im, 0, sizeof im);
    out->why = "not a PE file";
    if (!f) return -1;
    v3x_span_init(&r, f, n);
    (void)v3x_span_u16le(&r, &mz);
    if (r.err || mz != 0x5A4D) return -1;
    v3x_span_init(&r, f, n);
    (void)v3x_span_skip(&r, 0x3c);
    (void)v3x_span_u32le(&r, &lfanew);
    if (r.err) return -1;
    v3x_span_init(&r, f, n);
    (void)v3x_span_skip(&r, lfanew);
    (void)v3x_span_u32le(&r, &sig);
    (void)v3x_span_u16le(&r, &machine);
    (void)v3x_span_u16le(&r, &nsec);
    (void)v3x_span_skip(&r, 12);
    (void)v3x_span_u16le(&r, &optsz);
    (void)v3x_span_u16le(&r, &chars);
    if (r.err || sig != 0x00004550u) return -1;
    out->why = "not an x64 DLL";
    if (machine != 0x8664 || !(chars & 0x2000)) return -1;
    out->why = "malformed PE optional header";
    V3xSpan opt = v3x_span_sub(&r, optsz);
    if (r.err) return -1;
    (void)v3x_span_u16le(&opt, &magic);
    if (opt.err || magic != 0x20b || optsz < 112) return -1;
    (void)v3x_span_skip(&opt, 22);                   /* to ImageBase at +24 */
    (void)v3x_span_u64le(&opt, &im.image_base);
    (void)v3x_span_skip(&opt, 24);                   /* to SizeOfImage at +56 */
    (void)v3x_span_u32le(&opt, &im.size_of_image);
    (void)v3x_span_skip(&opt, 108 - 60);             /* to NumberOfRvaAndSizes at +108 */
    (void)v3x_span_u32le(&opt, &nrva);
    if (opt.err) return -1;
    if (nrva >= 1) { (void)v3x_span_u32le(&opt, &exp_rva); (void)v3x_span_u32le(&opt, &exp_size); }
    if (opt.err) return -1;
    if (nsec == 0 || nsec > 96) { out->why = "malformed PE section table"; return -1; }
    im.f = f; im.n = n; im.nsec = nsec;
    im.sec_off = r.pos;
    if (im.sec_off > n || (n - im.sec_off) / 40u < nsec) { out->why = "malformed PE section table"; return -1; }
    out->why = "ok";
    if (!exp_rva || !exp_size) return 0;             /* no exports: has_load = 0 */

    out->why = "malformed export directory";
    size_t eo = v3x_pe_rva_off_(&im, exp_rva, 40);
    if (eo == (size_t)-1) return -1;
    uint32_t nfun = 0, nnames = 0, a_fun = 0, a_names = 0, a_ord = 0;
    v3x_span_init(&r, f + eo, 40);
    (void)v3x_span_skip(&r, 20);
    (void)v3x_span_u32le(&r, &nfun); (void)v3x_span_u32le(&r, &nnames);
    (void)v3x_span_u32le(&r, &a_fun); (void)v3x_span_u32le(&r, &a_names); (void)v3x_span_u32le(&r, &a_ord);
    if (r.err || nnames > 65536 || nfun > 65536) return -1;
    size_t fo = nfun ? v3x_pe_rva_off_(&im, a_fun, (size_t)nfun * 4u) : 0;
    size_t no = nnames ? v3x_pe_rva_off_(&im, a_names, (size_t)nnames * 4u) : 0;
    size_t oo = nnames ? v3x_pe_rva_off_(&im, a_ord, (size_t)nnames * 2u) : 0;
    if (fo == (size_t)-1 || no == (size_t)-1 || oo == (size_t)-1) return -1;
    uint32_t rva_load = 0, rva_mf = 0, rva_name = 0;
    for (uint32_t i = 0; i < nnames; i++) {
        V3xSpan a;
        uint32_t nrv = 0, frv = 0;
        uint16_t ord = 0;
        char nm[32];
        v3x_span_init(&a, f + no + (size_t)i * 4u, 4); (void)v3x_span_u32le(&a, &nrv);
        v3x_span_init(&a, f + oo + (size_t)i * 2u, 2); (void)v3x_span_u16le(&a, &ord);
        if (ord >= nfun) continue;
        if (!v3x_pe_str_rva_(&im, nrv, nm, sizeof nm)) continue;
        v3x_span_init(&a, f + fo + (size_t)ord * 4u, 4); (void)v3x_span_u32le(&a, &frv);
        if (frv >= exp_rva && frv - exp_rva < exp_size) continue;   /* forwarder string, not code/data */
        if (!strcmp(nm, "v3x_plugin_load")) rva_load = frv;
        else if (!strcmp(nm, "v3x_plugin_manifest")) rva_mf = frv;
        else if (!strcmp(nm, "v3x_plugin_name")) rva_name = frv;
    }
    out->has_load = rva_load != 0 && rva_load < im.size_of_image;
    if (rva_name) {
        uint64_t va = 0;
        int nul = 0;
        if (v3x_pe_u64_rva_(&im, rva_name, &va) && v3x_pe_str_va_(&im, va, out->plugin_name, sizeof out->plugin_name, &nul) && !nul)
            out->has_name = 1;
    }
    if (rva_mf) {
        size_t mo = v3x_pe_rva_off_(&im, rva_mf, 16);
        out->why = "bad v3x_plugin_manifest";
        if (mo == (size_t)-1) return -1;
        v3x_span_init(&r, f + mo, 16);
        uint32_t resv = 0;
        (void)v3x_span_u32le(&r, &out->mf_size); (void)v3x_span_u32le(&r, &out->mf_flags);
        (void)v3x_span_u32le(&r, &out->mf_min_api); (void)v3x_span_u32le(&r, &resv);
        out->has_manifest = 1;
        if (out->mf_size < 16 + 8) { out->why = "bad v3x_plugin_manifest (size)"; return -1; }   /* below `name` */
        uint32_t sz = out->mf_size > 4096 ? 4096 : out->mf_size;
        struct { size_t at; char *dst; size_t cap; } fl[6] = {
            {16, out->name, sizeof out->name}, {24, out->version, sizeof out->version},
            {32, out->author, sizeof out->author}, {40, out->desc, sizeof out->desc},
            {48, out->builds, sizeof out->builds}, {56, out->caps, sizeof out->caps}};
        out->mf_builds_null = 1;
        for (int k = 0; k < 6; k++) {
            uint64_t va = 0;
            int nul = 0;
            if (fl[k].at + 8 > sz) break;
            if (!v3x_pe_u64_rva_(&im, rva_mf + (uint32_t)fl[k].at, &va)) { out->why = "bad v3x_plugin_manifest"; return -1; }
            if (!v3x_pe_str_va_(&im, va, fl[k].dst, fl[k].cap, &nul)) {
                out->why = "bad v3x_plugin_manifest (a string points outside the DLL)";
                return -1;
            }
            if (k == 4) out->mf_builds_null = nul;
            if (k == 5) out->mf_has_caps = !nul;
        }
    }
    out->why = "ok";
    return 0;
}

#endif
