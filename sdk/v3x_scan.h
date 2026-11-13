/* v3x_scan.h - code scanning helpers for V3X plugins (header-only, C).
 *
 * api->pattern_scan returns the FIRST match only and api->find_xref one xref at a time. Anchors
 * must be unique to be trustworthy, so these helpers count and enumerate:
 *
 *   V3xSection text;
 *   v3x_section(api->image_base(), ".text", &text);
 *   int n = v3x_scan_count(&text, "48 8B 05 ?? ?? ?? ?? 48 85 C0", &first);   // want n == 1
 *   void *callee = v3x_rel32_target(call_insn);                                 // E8/E9 xx xx xx xx
 *   void *global = v3x_rip_target(mov_insn, 3, 7);                              // disp at +3, len 7
 *
 * All functions work on any mapped PE image (the running game, or an exe mapped by
 * sdk/v3x_testkit.h in an offline test). Nothing here writes memory. */
#ifndef V3X_SCAN_H
#define V3X_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct V3xSection { uint8_t *ptr; size_t size; } V3xSection;

/* Finds section `name` (".text", ".rdata", ".data", ".pdata") of the PE image at base. 1 = found. */
static int v3x_section(uintptr_t base, const char *name, V3xSection *out)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(name);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (len <= 8 && !memcmp(s->Name, name, len) && (len == 8 || s->Name[len] == 0)) {
            out->ptr = (uint8_t *)base + s->VirtualAddress;
            out->size = s->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

/* Compiled IDA-style pattern ("48 8B ?? ?? 05", "?" or "??" = any byte). */
typedef struct V3xPattern { uint8_t bytes[256], mask[256]; int n; } V3xPattern;

/* Returns the number of bytes, 0 on an empty or malformed pattern (a non-hex token). */
static int v3x_pattern_compile(const char *pat, V3xPattern *p)
{
    p->n = 0;
    for (const char *s = pat; s && *s;) {
        if (*s == ' ') { s++; continue; }
        if (p->n >= 256) return p->n = 0;
        if (*s == '?') { p->bytes[p->n] = 0; p->mask[p->n++] = 0; s++; if (*s == '?') s++; continue; }
        char h[3] = {s[0], s[1], 0};
        char *end = NULL;
        unsigned long v = strtoul(h, &end, 16);
        if (end != h + 2) return p->n = 0;
        p->bytes[p->n] = (uint8_t)v; p->mask[p->n++] = 1;
        s += 2;
    }
    return p->n;
}

/* 1 if the compiled pattern matches at addr (addr must have p->n readable bytes). */
static int v3x_pattern_match_at(const V3xPattern *p, const uint8_t *addr)
{
    for (int k = 0; k < p->n; k++)
        if (p->mask[k] && addr[k] != p->bytes[k]) return 0;
    return p->n > 0;
}

/* Calls cb(match, user) for every match in sec (stops when cb returns non-zero). Returns the
 * number of matches visited, -1 on a bad pattern. cb may be NULL (just count). */
static int v3x_scan_each(const V3xSection *sec, const char *pattern, int (*cb)(uint8_t *match, void *user), void *user)
{
    V3xPattern p;
    if (!sec || !sec->ptr || !v3x_pattern_compile(pattern, &p)) return -1;
    int hits = 0;
    for (size_t i = 0; i + (size_t)p.n <= sec->size; i++) {
        if (p.mask[0] && sec->ptr[i] != p.bytes[0]) continue;
        if (!v3x_pattern_match_at(&p, sec->ptr + i)) continue;
        hits++;
        if (cb && cb(sec->ptr + i, user)) break;
    }
    return hits;
}

static int v3x_scan__first(uint8_t *m, void *u) { if (!*(uint8_t **)u) *(uint8_t **)u = m; return 0; }

/* Number of matches of pattern in sec (-1 bad pattern); *first = first match or NULL. Use it to
 * insist on a unique match: if (v3x_scan_count(&text, PAT, &hit) != 1) fail closed. */
static int v3x_scan_count(const V3xSection *sec, const char *pattern, void **first)
{
    uint8_t *f = NULL;
    int n = v3x_scan_each(sec, pattern, v3x_scan__first, &f);
    if (first) *first = f;
    return n;
}

/* Target of a rel32 call/jmp at insn (E8 / E9 rel32, or 0F 8x rel32 jcc). NULL if it is none of them. */
static void *v3x_rel32_target(const void *insn)
{
    const uint8_t *p = (const uint8_t *)insn;
    int32_t d;
    if (p[0] == 0xE8 || p[0] == 0xE9) { memcpy(&d, p + 1, 4); return (void *)(p + 5 + d); }
    if (p[0] == 0x0F && (p[1] & 0xF0) == 0x80) { memcpy(&d, p + 2, 4); return (void *)(p + 6 + d); }
    return NULL;
}

/* Target of a RIP-relative operand: disp32 at insn+disp_off, instruction length insn_len.
 * `mov rax, [rip+X]` = 48 8B 05 X: v3x_rip_target(p, 3, 7). `lea rcx, [rip+X]` = 48 8D 0D X: same. */
static void *v3x_rip_target(const void *insn, int disp_off, int insn_len)
{
    int32_t d;
    memcpy(&d, (const uint8_t *)insn + disp_off, 4);
    return (void *)((const uint8_t *)insn + insn_len + d);
}

/* Every RIP-relative `lea/mov r64, [rip+X]` (REX 48/4C, 8D/8B, modrm rm=101 mod=00; the same
 * encoding api->find_xref recognises) in sec whose target is `target`. Writes up to max of them
 * to out (may be NULL) and returns the total count. */
static int v3x_xrefs(const V3xSection *sec, const void *target, void **out, int max)
{
    int n = 0;
    for (size_t i = 0; sec && sec->ptr && i + 7 <= sec->size; i++) {
        const uint8_t *p = sec->ptr + i;
        if ((p[0] != 0x48 && p[0] != 0x4C) || (p[1] != 0x8D && p[1] != 0x8B) || (p[2] & 0xC7) != 0x05) continue;
        if (v3x_rip_target(p, 3, 7) != target) continue;
        if (out && n < max) out[n] = (void *)p;
        n++;
    }
    return n;
}

/* Every direct `call target` (E8 rel32) in sec. Same out/max/return convention as v3x_xrefs. */
static int v3x_callers(const V3xSection *sec, const void *target, void **out, int max)
{
    int n = 0;
    for (size_t i = 0; sec && sec->ptr && i + 5 <= sec->size; i++) {
        if (sec->ptr[i] != 0xE8 || v3x_rel32_target(sec->ptr + i) != target) continue;
        if (out && n < max) out[n] = sec->ptr + i;
        n++;
    }
    return n;
}

/* Searches forward from `from` for up to `max_bytes` for the pattern (e.g. inside one function).
 * Returns the match or NULL. The caller guarantees [from, from+max_bytes) is mapped. */
static void *v3x_scan_near(const void *from, size_t max_bytes, const char *pattern)
{
    V3xPattern p;
    if (!from || !v3x_pattern_compile(pattern, &p)) return NULL;
    const uint8_t *b = (const uint8_t *)from;
    for (size_t i = 0; i + (size_t)p.n <= max_bytes; i++)
        if (v3x_pattern_match_at(&p, b + i)) return (void *)(b + i);
    return NULL;
}

#endif
