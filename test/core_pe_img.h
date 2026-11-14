/* core_pe_img.h - builds a tiny but valid x64 plugin DLL IMAGE in memory (headers, one section, an export
 * directory with v3x_plugin_load / v3x_plugin_manifest / v3x_plugin_name and a manifest) for
 * test/core_parse_test.c (unit tests of src/v3x_pe.h) and test/fuzz/fuzz_core_parse.c (seed). The image is
 * a plain file layout: NOT loadable, only parseable. Fields can be bent through PeOpt.
 *
 *   file offset 0x000 DOS header (e_lfanew = 0x40), 0x040 PE signature + file header, 0x058 optional header
 *   (240 bytes), 0x148 one section header; section ".rdata" RVA 0x1000, raw data at 0x200..0x600.
 *   section offsets: +0x000 export dir, +0x040 functions, +0x050 names, +0x060 ordinals, +0x070 names text,
 *   +0x100 manifest (64 bytes), +0x180 v3x_plugin_name pointer, +0x200 strings. */
#ifndef CORE_PE_IMG_H
#define CORE_PE_IMG_H

#include <stdint.h>
#include <string.h>

#define PEI_SIZE 0x600
#define PEI_SEC_RAW 0x200
#define PEI_SEC_RVA 0x1000
#define PEI_BASE 0x180000000ull

typedef struct PeOpt {
    uint16_t machine, chars, magic;     /* 0 = the default (0x8664, 0x2022, 0x20b) */
    uint16_t nsec;                      /* 0 = 1 */
    uint32_t mf_size;                   /* manifest struct_size, 0 = 64 */
    uint32_t mf_flags, mf_min_api;
    int no_exports;                     /* export directory entry = 0 */
    int no_load, no_manifest, no_name;  /* leave that export out */
    int load_forwarder;                 /* v3x_plugin_load RVA inside the export directory */
    int caps_null, builds_null;         /* NULL pointers */
    int bad_str;                        /* version pointer outside the image */
    int gap_str;                        /* version pointer into an RVA that has no file data */
    int unterminated;                   /* description string runs to the end of the section data */
    uint32_t nnames;                    /* 0 = 3 (the number of names in the export dir) */
    const char *name, *version, *author, *desc, *builds, *caps;   /* NULL = defaults */
} PeOpt;

static void pei_w16(uint8_t *b, size_t o, uint16_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void pei_w32(uint8_t *b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + (size_t)i] = (uint8_t)(v >> (8 * i)); }
static void pei_w64(uint8_t *b, size_t o, uint64_t v) { for (int i = 0; i < 8; i++) b[o + (size_t)i] = (uint8_t)(v >> (8 * i)); }

/* writes the image to b[PEI_SIZE] (zeroed first); returns PEI_SIZE */
static size_t pei_build(uint8_t *b, const PeOpt *o)
{
    const size_t S = PEI_SEC_RAW;                       /* file offset of the section data */
    memset(b, 0, PEI_SIZE);
    b[0] = 'M'; b[1] = 'Z';
    pei_w32(b, 0x3c, 0x40);
    memcpy(b + 0x40, "PE\0\0", 4);
    pei_w16(b, 0x44, o->machine ? o->machine : 0x8664);
    pei_w16(b, 0x46, o->nsec ? o->nsec : 1);
    pei_w16(b, 0x54, 0xF0);
    pei_w16(b, 0x56, o->chars ? o->chars : 0x2022);
    pei_w16(b, 0x58, o->magic ? o->magic : 0x20b);
    pei_w64(b, 0x58 + 24, PEI_BASE);
    pei_w32(b, 0x58 + 56, 0x3000);                      /* SizeOfImage */
    pei_w32(b, 0x58 + 108, 16);
    if (!o->no_exports) { pei_w32(b, 0x58 + 112, PEI_SEC_RVA); pei_w32(b, 0x58 + 116, 0xb0); }
    memcpy(b + 0x148, ".rdata", 6);
    pei_w32(b, 0x148 + 8, 0x600);                       /* VirtualSize */
    pei_w32(b, 0x148 + 12, PEI_SEC_RVA);
    pei_w32(b, 0x148 + 16, PEI_SIZE - PEI_SEC_RAW);     /* SizeOfRawData */
    pei_w32(b, 0x148 + 20, PEI_SEC_RAW);

    uint32_t nn = o->nnames ? o->nnames : 3;
    pei_w32(b, S + 20, 3);                              /* NumberOfFunctions */
    pei_w32(b, S + 24, nn);                             /* NumberOfNames */
    pei_w32(b, S + 28, PEI_SEC_RVA + 0x40);
    pei_w32(b, S + 32, PEI_SEC_RVA + 0x50);
    pei_w32(b, S + 36, PEI_SEC_RVA + 0x60);
    /* function table: load, manifest, name (ordinal 0, 1, 2) */
    pei_w32(b, S + 0x40, o->load_forwarder ? PEI_SEC_RVA + 0x10 : PEI_SEC_RVA + 0x3f0);
    pei_w32(b, S + 0x44, PEI_SEC_RVA + 0x100);
    pei_w32(b, S + 0x48, PEI_SEC_RVA + 0x180);
    /* names: a name the loader does not look for takes the slot of an export that is left out */
    pei_w32(b, S + 0x50, PEI_SEC_RVA + (o->no_load ? 0x0a8u : 0x70u));
    pei_w32(b, S + 0x54, PEI_SEC_RVA + (o->no_manifest ? 0x0a8u : 0x80u));
    pei_w32(b, S + 0x58, PEI_SEC_RVA + (o->no_name ? 0x0a8u : 0x94u));
    pei_w16(b, S + 0x60, 0); pei_w16(b, S + 0x62, 1); pei_w16(b, S + 0x64, 2);
    memcpy(b + S + 0x70, "v3x_plugin_load", 16);
    memcpy(b + S + 0x80, "v3x_plugin_manifest", 20);
    memcpy(b + S + 0x94, "v3x_plugin_name", 16);
    memcpy(b + S + 0xa8, "other_export", 13);

    /* strings at +0x200: name, version, author, desc, builds, caps, plugin_name */
    const char *def[7] = {"pei_plugin", "1.2.3", "tester", "a test plugin", "release/1.15.1;unkn*", "hook, patch", "pei_stem"};
    const char *use[7] = {o->name, o->version, o->author, o->desc, o->builds, o->caps, NULL};
    uint32_t va_rva[7];
    size_t at = 0x200;
    for (int i = 0; i < 7; i++) {
        const char *s = use[i] ? use[i] : def[i];
        size_t n = strlen(s) + 1;
        if (i == 3 && o->unterminated) {
            /* fill to the end of the file's section data with non-NUL bytes */
            va_rva[i] = PEI_SEC_RVA + (uint32_t)at;
            memset(b + S + at, 'x', PEI_SIZE - S - at);
            at = PEI_SIZE - S;
            continue;
        }
        if (S + at + n > PEI_SIZE) n = PEI_SIZE - S - at;
        memcpy(b + S + at, s, n);
        va_rva[i] = PEI_SEC_RVA + (uint32_t)at;
        at += n;
    }
    /* manifest at +0x100 */
    uint32_t msz = o->mf_size ? o->mf_size : 64;
    pei_w32(b, S + 0x100, msz);
    pei_w32(b, S + 0x104, o->mf_flags);
    pei_w32(b, S + 0x108, o->mf_min_api);
    for (int i = 0; i < 6; i++) {
        uint64_t va = PEI_BASE + va_rva[i];
        if (i == 1 && o->bad_str) va = PEI_BASE + 0x9000;       /* beyond SizeOfImage */
        if (i == 1 && o->gap_str) va = PEI_BASE + 0x2800;       /* inside the image, no file data */
        if (i == 4 && o->builds_null) va = 0;
        if (i == 5 && o->caps_null) va = 0;
        pei_w64(b, S + 0x110 + (size_t)i * 8, va);
    }
    pei_w64(b, S + 0x180, PEI_BASE + va_rva[6]);        /* v3x_plugin_name data = pointer to the string */
    return PEI_SIZE;
}

#endif
