/* lazy_gfx processed-texture cache: PNG/JPG/TGA -> DDS bytes that the engine's own DDS loader turns into
 * the same decoded image. Pure logic on memory (no engine calls), testable offline.
 *
 * Decoded-image struct ("img", CreateTexture rbp-0x80; written by decode 0x3c5e0d0) [V, static]:
 *   +0x00 u32 width, +0x04 u32 height, +0x08 u32 (init 1), +0x0c u32 (init 1),
 *   +0x10 u8 pdx format (FreeImage path writes 0x12/0x1f/0x21/0x25), +0x13 u8, +0x16 u8 (FreeImage: 2),
 *   +0x18 CPdxArray<subresource> {ptr +0x18, cap +0x20, size +0x24} (push = 0x169bc30),
 *   +0x40 owned pixel buffer (FreeImage path; freed by CreateTexture).
 * Subresource (40 bytes): +0x00 u64, +0x08 u32, +0x0c u32 width, +0x10 u32 height, +0x14 u32 depth,
 *   +0x18 u32 bytes, +0x20 data pointer (DDS loader: points INTO the file buffer, zero copy).
 * These offsets are re-checked at runtime against every plain 2D DDS the engine decodes (lg_img_check_dds);
 * the cache stays off until enough DDS loads matched and none failed. */
#ifndef LG_CACHE_H
#define LG_CACHE_H
#include <stdint.h>
#include <stddef.h>

#define LG_IMG_W 0x00
#define LG_IMG_H 0x04
#define LG_IMG_FMT 0x10
#define LG_IMG_ARR 0x18
#define LG_IMG_ARRN 0x24
#define LG_SUB_SIZE 40
#define LG_SUB_W 0x0c
#define LG_SUB_H 0x10
#define LG_SUB_D 0x14
#define LG_SUB_BYTES 0x18
#define LG_SUB_PTR 0x20
#define LG_DG_SUBS 16

typedef int (*lg_readable_fn)(const void *p, size_t n);

/* parsed DDS file header */
typedef struct lg_dds {
    uint32_t w, h, depth, mips, flags, caps2;
    uint8_t pf[32];             /* DDS_PIXELFORMAT verbatim */
    uint8_t dx10[20];           /* DDS_HEADER_DXT10 verbatim (valid if has_dx10) */
    int has_dx10, plain2d;      /* plain2d: no cubemap/volume/array */
    uint32_t data_off;          /* 128 or 148 */
} lg_dds;
int lg_dds_parse(const uint8_t *buf, size_t n, lg_dds *out);   /* 1 = DDS */

/* Checks the img layout against a DDS the engine just decoded from buf[0..n): dims (allowing k dropped
 * top mips), subresource count, per-mip dims, data pointers inside buf. 1 = consistent, 0 = not,
 * -1 = not checkable (not plain 2D). */
int lg_img_check_dds(const uint8_t *img, const lg_dds *d, const uint8_t *buf, size_t n, lg_readable_fn rd);

/* everything the engine's decode produced that matters for identical output */
typedef struct lg_digest {
    uint32_t w, h, n;
    uint8_t head[16];                     /* img +0x08..+0x17 */
    uint8_t sub[LG_DG_SUBS][32];          /* each subresource +0x00..+0x1f (no pointer) */
    uint64_t data_hash, data_bytes;
} lg_digest;
int lg_digest_img(const uint8_t *img, lg_digest *dg, lg_readable_fn rd);   /* 1 ok */
int lg_digest_eq(const lg_digest *a, const lg_digest *b);
uint64_t lg_hash_bytes(const void *p, size_t n, uint64_t h);

/* learned pixel formats: img format byte -> the DDS header block that produced it */
typedef struct lg_fmt { int known; uint8_t pf[32]; uint8_t dx10[20]; int has_dx10; uint32_t seen; } lg_fmt;
void lg_fmt_learn(lg_fmt tab[256], uint8_t fmt, const lg_dds *d);

/* Builds a single-mip uncompressed DDS from a decoded img (1 subresource) using the learned header for
 * its format byte. Returns malloc'd bytes (size in *out_n) or NULL (unknown format, >1 subresource,
 * pitch not whole, unreadable). */
uint8_t *lg_dds_build(const uint8_t *img, const lg_fmt tab[256], size_t *out_n, lg_readable_fn rd);

/* cache file = lg_cache_hdr + DDS bytes */
#define LG_CACHE_MAGIC "V3XTEX1"
typedef struct lg_cache_hdr {
    char magic[8];
    uint32_t hdr_size, dds_size;
    uint64_t src_size, src_mtime;
    char build[48];
    char src[520];               /* absolute source file */
    float decode_ms;             /* the engine's decode time of the source when built */
    uint32_t reserved;
    lg_digest dg;                /* the engine's decode output of the SOURCE */
} lg_cache_hdr;

uint64_t lg_cache_key(const char *abs, uint64_t size, uint64_t mtime, const char *build);
void lg_cache_file(char *out, size_t n, const char *dir, uint64_t key);
int lg_cache_write(const char *path, const lg_cache_hdr *h, const uint8_t *dds, size_t dds_n);  /* 0 ok */
/* Reads and validates (magic, sizes, key fields). Returns malloc'd DDS bytes (n in *dds_n) and fills *h,
 * or NULL when missing / stale / corrupt. */
uint8_t *lg_cache_read(const char *path, const char *abs, uint64_t size, uint64_t mtime, const char *build,
                       lg_cache_hdr *h, size_t *dds_n);

#endif
