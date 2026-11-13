/* lazy_gfx processed-texture cache (see lg_cache.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lg_cache.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static int ok_read(lg_readable_fn rd, const void *p, size_t n) { return p && (!rd || rd(p, n)); }

int lg_dds_parse(const uint8_t *b, size_t n, lg_dds *d)
{
    memset(d, 0, sizeof *d);
    if (n < 128 || memcmp(b, "DDS ", 4) || rd32(b + 4) != 124 || rd32(b + 76) != 32) return 0;
    d->flags = rd32(b + 8);
    d->h = rd32(b + 12); d->w = rd32(b + 16);
    d->depth = rd32(b + 24);
    d->mips = rd32(b + 28); if (!d->mips) d->mips = 1;
    memcpy(d->pf, b + 76, 32);
    d->caps2 = rd32(b + 112);
    d->data_off = 128;
    int vol = (d->caps2 & 0x200000) || ((d->flags & 0x800000) && d->depth > 1);
    int cube = (d->caps2 & 0x200) != 0;
    int arr = 0;
    if ((rd32(b + 80) & 4) && !memcmp(b + 84, "DX10", 4)) {
        if (n < 148) return 0;
        memcpy(d->dx10, b + 128, 20);
        d->has_dx10 = 1;
        d->data_off = 148;
        uint32_t dim = rd32(b + 132), misc = rd32(b + 136), asz = rd32(b + 140);
        if (dim != 3 || (misc & 4) || asz > 1) arr = 1;
    }
    d->plain2d = !vol && !cube && !arr && d->w && d->h;
    return 1;
}

int lg_img_check_dds(const uint8_t *img, const lg_dds *d, const uint8_t *buf, size_t n, lg_readable_fn rd)
{
    if (!d->plain2d) return -1;
    if (!ok_read(rd, img, 0x28)) return 0;
    uint32_t w = rd32(img + LG_IMG_W), h = rd32(img + LG_IMG_H);
    int32_t cnt; memcpy(&cnt, img + LG_IMG_ARRN, 4);
    const uint8_t *arr; memcpy(&arr, img + LG_IMG_ARR, 8);
    int k = 0;                                   /* top mips dropped by load_settings */
    while (k < 16 && (d->w >> k ? d->w >> k : 1) != w) k++;
    if (k == 16 || ((d->h >> k) ? d->h >> k : 1) != h) return 0;
    if (cnt < 1 || cnt > 64) return 0;
    if ((uint32_t)cnt + (uint32_t)k != d->mips) return -1;      /* truncated / regenerated mips: not checkable */
    if (!ok_read(rd, arr, (size_t)cnt * LG_SUB_SIZE)) return 0;
    uint64_t sum = 0;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *s = arr + (size_t)i * LG_SUB_SIZE;
        uint32_t ew = w >> i ? w >> i : 1, eh = h >> i ? h >> i : 1;
        const uint8_t *p; memcpy(&p, s + LG_SUB_PTR, 8);
        uint32_t bytes = rd32(s + LG_SUB_BYTES);
        if (rd32(s + LG_SUB_W) != ew || rd32(s + LG_SUB_H) != eh || !bytes) return 0;
        if (p < buf + d->data_off || p + bytes > buf + n) return 0;
        sum += bytes;
    }
    return sum <= n ? 1 : 0;
}

uint64_t lg_hash_bytes(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, b + i, 8); h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

int lg_digest_img(const uint8_t *img, lg_digest *dg, lg_readable_fn rd)
{
    memset(dg, 0, sizeof *dg);
    if (!ok_read(rd, img, 0x28)) return 0;
    dg->w = rd32(img + LG_IMG_W); dg->h = rd32(img + LG_IMG_H);
    memcpy(dg->head, img + 0x08, 16);
    int32_t cnt; memcpy(&cnt, img + LG_IMG_ARRN, 4);
    const uint8_t *arr; memcpy(&arr, img + LG_IMG_ARR, 8);
    if (cnt < 1 || cnt > LG_DG_SUBS || !ok_read(rd, arr, (size_t)cnt * LG_SUB_SIZE)) return 0;
    dg->n = (uint32_t)cnt;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *s = arr + (size_t)i * LG_SUB_SIZE;
        memcpy(dg->sub[i], s, 32);
        const uint8_t *p; memcpy(&p, s + LG_SUB_PTR, 8);
        uint32_t bytes = rd32(s + LG_SUB_BYTES);
        if (!bytes || !ok_read(rd, p, bytes)) return 0;
        h = lg_hash_bytes(p, bytes, h);
        dg->data_bytes += bytes;
    }
    dg->data_hash = h;
    return 1;
}

int lg_digest_eq(const lg_digest *a, const lg_digest *b) { return !memcmp(a, b, sizeof *a); }

void lg_fmt_learn(lg_fmt tab[256], uint8_t fmt, const lg_dds *d)
{
    lg_fmt *f = &tab[fmt];
    if (f->known) { f->seen++; return; }
    f->known = 1; f->seen = 1;
    memcpy(f->pf, d->pf, 32);
    f->has_dx10 = d->has_dx10;
    if (d->has_dx10) memcpy(f->dx10, d->dx10, 20);
}

uint8_t *lg_dds_build(const uint8_t *img, const lg_fmt tab[256], size_t *out_n, lg_readable_fn rd)
{
    *out_n = 0;
    if (!ok_read(rd, img, 0x28)) return NULL;
    uint32_t w = rd32(img + LG_IMG_W), h = rd32(img + LG_IMG_H);
    uint8_t fmt = img[LG_IMG_FMT];
    int32_t cnt; memcpy(&cnt, img + LG_IMG_ARRN, 4);
    const uint8_t *arr; memcpy(&arr, img + LG_IMG_ARR, 8);
    const lg_fmt *f = &tab[fmt];
    if (!f->known || cnt != 1 || !w || !h || !ok_read(rd, arr, LG_SUB_SIZE)) return NULL;
    const uint8_t *p; memcpy(&p, arr + LG_SUB_PTR, 8);
    uint32_t bytes = rd32(arr + LG_SUB_BYTES);
    if (rd32(arr + LG_SUB_W) != w || rd32(arr + LG_SUB_H) != h || !bytes || bytes % h || !ok_read(rd, p, bytes)) return NULL;
    size_t hdr = 128 + (f->has_dx10 ? 20 : 0);
    uint8_t *o = calloc(1, hdr + bytes);
    if (!o) return NULL;
    memcpy(o, "DDS ", 4);
    wr32(o + 4, 124);
    wr32(o + 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8);     /* CAPS HEIGHT WIDTH PIXELFORMAT PITCH */
    wr32(o + 12, h); wr32(o + 16, w);
    wr32(o + 20, bytes / h);                         /* pitch */
    wr32(o + 28, 1);                                 /* one mip, as the FreeImage path produced */
    memcpy(o + 76, f->pf, 32);
    wr32(o + 108, 0x1000);                           /* DDSCAPS_TEXTURE */
    if (f->has_dx10) {
        memcpy(o + 128, f->dx10, 20);
        wr32(o + 132, 3); wr32(o + 136, 0); wr32(o + 140, 1);   /* TEXTURE2D, no misc, array size 1 */
    }
    memcpy(o + hdr, p, bytes);
    *out_n = hdr + bytes;
    return o;
}

uint64_t lg_cache_key(const char *abs, uint64_t size, uint64_t mtime, const char *build)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *s = abs; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 1099511628211ull;
    }
    h = lg_hash_bytes(&size, 8, h);
    h = lg_hash_bytes(&mtime, 8, h);
    for (const char *s = build ? build : ""; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h ? h : 1;
}

void lg_cache_file(char *out, size_t n, const char *dir, uint64_t key)
{
    size_t l = strlen(dir);
    snprintf(out, n, "%s%s%016llx.v3xtex", dir, l && (dir[l - 1] == '/' || dir[l - 1] == '\\') ? "" : "/", (unsigned long long)key);
}

int lg_cache_write(const char *path, const lg_cache_hdr *h, const uint8_t *dds, size_t dds_n)
{
    char tmp[MAX_PATH + 16]; snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, GetCurrentThreadId());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    lg_cache_hdr c = *h;
    memcpy(c.magic, LG_CACHE_MAGIC, 8);
    c.hdr_size = sizeof c; c.dds_size = (uint32_t)dds_n;
    int ok = fwrite(&c, sizeof c, 1, f) == 1 && fwrite(dds, 1, dds_n, f) == dds_n;
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return -1; }
    return 0;
}

uint8_t *lg_cache_read(const char *path, const char *abs, uint64_t size, uint64_t mtime, const char *build,
                       lg_cache_hdr *h, size_t *dds_n)
{
    *dds_n = 0;
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (fh == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz; DWORD got = 0;
    uint8_t *dds = NULL;
    if (!GetFileSizeEx(fh, &sz) || sz.QuadPart < (LONGLONG)sizeof *h || sz.QuadPart > (1ll << 30)) goto out;
    if (!ReadFile(fh, h, sizeof *h, &got, NULL) || got != sizeof *h) goto out;
    if (memcmp(h->magic, LG_CACHE_MAGIC, 8) || h->hdr_size != sizeof *h || (LONGLONG)sizeof *h + h->dds_size != sz.QuadPart) goto out;
    h->src[sizeof h->src - 1] = 0; h->build[sizeof h->build - 1] = 0;
    if (h->src_size != size || h->src_mtime != mtime || _stricmp(h->src, abs) || strcmp(h->build, build ? build : "")) goto out;
    dds = malloc(h->dds_size ? h->dds_size : 1);
    if (!dds || !ReadFile(fh, dds, h->dds_size, &got, NULL) || got != h->dds_size) { free(dds); dds = NULL; goto out; }
    lg_dds d;
    if (!lg_dds_parse(dds, h->dds_size, &d)) { free(dds); dds = NULL; goto out; }
    *dds_n = h->dds_size;
out:
    CloseHandle(fh);
    return dds;
}
