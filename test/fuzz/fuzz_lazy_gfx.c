/* Fuzz harness: plugins/lazy_gfx/lg_cache.c - the DDS header parser (lg_dds_parse), the processed-
 * texture cache reader (lg_cache_read, through a temp file) and the decoded-image walkers
 * (lg_digest_img / lg_dds_build / lg_img_check_dds) on an "image" that is the fuzz input itself, with a
 * readable() that only admits ranges inside the input (pointers inside it are attacker-controlled).
 * Invariants: data_off is 128 or 148 and <= n; an accepted cache entry has dds_n == hdr.dds_size and
 * its bytes start with "DDS ". */
#include "v3x_fuzz.h"
#include "../../plugins/lazy_gfx/lg_cache.c"

static const uint8_t *g_lo, *g_hi;
static int in_input(const void *p, size_t n)
{
    const uint8_t *q = (const uint8_t *)p;
    return q >= g_lo && q <= g_hi && (size_t)(g_hi - q) >= n;
}

static void fuzz_dds(const uint8_t *d, size_t n)
{
    const uint8_t *buf = (const uint8_t *)v3x_fuzz_guarded(d, n);
    lg_dds dd;
    memset(&dd, 0, sizeof dd);
    if (lg_dds_parse(buf, n, &dd) == 1) {
        V3X_FUZZ_ASSERT((dd.data_off == 128 || dd.data_off == 148) && dd.data_off <= n);
    }
    /* the same bytes as an engine image object */
    g_lo = buf; g_hi = buf + n;
    lg_digest dg;
    if (n >= 0x40) {
        (void)lg_digest_img(buf, &dg, in_input);
        (void)lg_img_check_dds(buf, &dd, buf, n, in_input);
        static lg_fmt tab[256];
        lg_fmt_learn(tab, buf[LG_IMG_FMT], &dd);
        size_t on = 0;
        uint8_t *o = lg_dds_build(buf, tab, &on, in_input);
        if (o) {
            lg_dds back;
            V3X_FUZZ_ASSERT(on >= 128 && lg_dds_parse(o, on, &back) == 1);
            free(o);
        }
    }
}
V3X_FUZZ_TARGET(lg_dds, fuzz_dds)

static char g_tmp[MAX_PATH];
static void tmp_path(void)
{
    if (g_tmp[0]) return;
    char t[MAX_PATH];
    DWORD k = GetTempPathA(sizeof t, t);
    snprintf(g_tmp, sizeof g_tmp, "%sv3x_fuzz_lg_%lu.bin", k && k < sizeof t ? t : ".\\", GetCurrentProcessId());
}

static void fuzz_cache(const uint8_t *d, size_t n)
{
    tmp_path();
    FILE *f = fopen(g_tmp, "wb");
    if (!f) return;
    if (n) fwrite(d, 1, n, f);
    fclose(f);
    /* expected key fields: from the input's own header (deep path) or fixed (mismatch path) */
    lg_cache_hdr in;
    memset(&in, 0, sizeof in);
    if (n >= sizeof in) memcpy(&in, d, sizeof in);
    in.build[sizeof in.build - 1] = 0;
    in.src[sizeof in.src - 1] = 0;
    int own = n && (d[n - 1] & 1);
    const char *abs = own ? in.src : "C:\\game\\gfx\\a.png";
    const char *build = own ? in.build : "6af3094f-60fb000";
    uint64_t sz = own ? in.src_size : 1234, mt = own ? in.src_mtime : 5678;
    lg_cache_hdr h;
    size_t dn = 0;
    uint8_t *dds = lg_cache_read(g_tmp, abs, sz, mt, build, &h, &dn);
    if (dds) {
        V3X_FUZZ_ASSERT(dn == h.dds_size && dn + sizeof h <= n && dn >= 4 && !memcmp(dds, "DDS ", 4));
        free(dds);
    }
    DeleteFileA(g_tmp);
}
V3X_FUZZ_TARGET(lg_cache, fuzz_cache)

/* a valid 4x4 RGBA DDS, and a cache file built by lg_cache_write around it (odd last byte = "own") */
__attribute__((constructor)) static void seed_lg(void)
{
    static uint8_t dds[128 + 64];
    memset(dds, 0, sizeof dds);
    memcpy(dds, "DDS ", 4);
    uint32_t v;
    v = 124; memcpy(dds + 4, &v, 4);                  /* dwSize */
    v = 0x1007; memcpy(dds + 8, &v, 4);               /* CAPS | HEIGHT | WIDTH | PIXELFORMAT */
    v = 4; memcpy(dds + 12, &v, 4); memcpy(dds + 16, &v, 4);
    v = 32; memcpy(dds + 76, &v, 4);                  /* ddspf.dwSize */
    v = 0x41; memcpy(dds + 80, &v, 4);                /* RGB | ALPHAPIXELS */
    v = 32; memcpy(dds + 88, &v, 4);                  /* bit count */
    v = 0xff; memcpy(dds + 92, &v, 4); v = 0xff00; memcpy(dds + 96, &v, 4);
    v = 0xff0000; memcpy(dds + 100, &v, 4); v = 0xff000000u; memcpy(dds + 104, &v, 4);
    v = 0x1000; memcpy(dds + 108, &v, 4);             /* caps: TEXTURE */
    v3x_fuzz_add_seed("lg_dds", dds, sizeof dds);
    tmp_path();
    lg_cache_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, LG_CACHE_MAGIC, sizeof LG_CACHE_MAGIC);
    h.hdr_size = sizeof h; h.dds_size = sizeof dds; h.src_size = 1234; h.src_mtime = 5678;
    snprintf(h.build, sizeof h.build, "6af3094f-60fb000");
    snprintf(h.src, sizeof h.src, "C:\\game\\gfx\\a.png");
    if (lg_cache_write(g_tmp, &h, dds, sizeof dds) == 0) {
        FILE *f = fopen(g_tmp, "rb");
        static uint8_t b[sizeof(lg_cache_hdr) + sizeof dds + 1];
        size_t k = f ? fread(b, 1, sizeof b - 1, f) : 0;
        if (f) fclose(f);
        if (k) { v3x_fuzz_add_seed("lg_cache", b, k); b[k] = 1; v3x_fuzz_add_seed("lg_cache", b, k + 1); }
    }
    DeleteFileA(g_tmp);
}
