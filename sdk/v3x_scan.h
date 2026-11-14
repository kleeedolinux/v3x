/* v3x_scan.h - code scanning helpers for V3X plugins (header-only, C).
 *
 * api->pattern_scan returns the FIRST match only and api->find_xref one xref at a time. Anchors
 * must be unique to be trustworthy, so these helpers count and enumerate:
 *
 *   V3xSection text, image;
 *   v3x_section(api->image_base(), ".text", &text);
 *   v3x_image_range(api->image_base(), &image);
 *   int n = v3x_scan_count(&text, "48 8B 05 ?? ?? ?? ?? 48 85 C0", &first);   // want n == 1
 *   void *callee = v3x_rel32_target_in(&text, call_insn, NULL);                 // E8/E9 xx xx xx xx
 *   void *global = v3x_rip_target_in(&text, mov_insn, 3, 7, &image);            // disp at +3, len 7
 *   void *hit = v3x_scan_near_in(&text, fn, 0x40, "48 83 C1 40 E8");             // within fn's first 0x40 bytes
 *
 * Bounded decoding (audit action 12): the *_in functions read instruction bytes only inside `code`
 * and return a target only if it lies inside `dest` (NULL = code), else NULL; target arithmetic is
 * done on integers (no pointer overflow). The old unbounded names v3x_rel32_target, v3x_rip_target
 * and v3x_scan_near are DEPRECATED wrappers: they look up the mapped image (or, outside an image,
 * the committed memory region) that contains the address with VirtualQuery and call the bounded
 * version with that range - one system call per use, so loops over a whole section should call the
 * *_in functions. Define V3X_SAFE_STRICT (or V3X_WARN_DEPRECATED) to get deprecation warnings.
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
#include "v3x_safe.h"   /* V3X_MUST_USE, V3X_NN */

V3X_NN_BEGIN

#if defined(V3X_SAFE_STRICT) || defined(V3X_WARN_DEPRECATED)
#define V3X_SCAN_DEPRECATED_(msg) __attribute__((deprecated(msg)))
#else
#define V3X_SCAN_DEPRECATED_(msg)
#endif

typedef struct V3xSection { uint8_t *ptr; size_t size; } V3xSection;

/* Finds section `name` (".text", ".rdata", ".data", ".pdata") of the PE image at base. 1 = found. */
static int v3x_section(uintptr_t base, const char *V3X_NN name, V3xSection *V3X_NN out)
{
    if (!base || !name || !out) return 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000) return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(name), img = nt->OptionalHeader.SizeOfImage;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (len <= 8 && !memcmp(s->Name, name, len) && (len == 8 || s->Name[len] == 0)) {
            if ((uint64_t)s->VirtualAddress + s->Misc.VirtualSize > img) return 0;   /* malformed */
            out->ptr = (uint8_t *)base + s->VirtualAddress;
            out->size = s->Misc.VirtualSize;
            return 1;
        }
    }
    return 0;
}

/* The whole image at base (SizeOfImage bytes). 1 = ok. */
static int v3x_image_range(uintptr_t base, V3xSection *V3X_NN out)
{
    if (!base || !out) return 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000) return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + (uintptr_t)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    out->ptr = (uint8_t *)base;
    out->size = nt->OptionalHeader.SizeOfImage;
    return 1;
}

/* 1 if [p, p+n) lies inside sec (integer arithmetic, no wrap) */
static int v3x_sec_has_(const V3xSection *sec, const void *p, size_t n)
{
    if (!sec || !sec->ptr || !p) return 0;
    uintptr_t lo = (uintptr_t)sec->ptr, a = (uintptr_t)p;
    return a >= lo && n <= sec->size && a - lo <= sec->size - n;
}

/* Compiled IDA-style pattern ("48 8B ?? ?? 05", "?" or "??" = any byte). */
typedef struct V3xPattern { uint8_t bytes[256], mask[256]; int n; } V3xPattern;

static int v3x_hex_digit_(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Returns the number of bytes; 0 on an empty or malformed pattern (a byte that is not two hex digits,
 * e.g. "+F", "-1", "4", "0x"), or more than 256 bytes (never cut). */
static int v3x_pattern_compile(const char *pat, V3xPattern *V3X_NN p)
{
    if (!p) return 0;
    p->n = 0;
    for (const char *s = pat; s && *s;) {
        if (*s == ' ') { s++; continue; }
        if (p->n >= 256) return p->n = 0;
        if (*s == '?') { p->bytes[p->n] = 0; p->mask[p->n++] = 0; s++; if (*s == '?') s++; continue; }
        int hi = v3x_hex_digit_(s[0]), lo = hi < 0 ? -1 : v3x_hex_digit_(s[1]);
        if (hi < 0 || lo < 0) return p->n = 0;
        p->bytes[p->n] = (uint8_t)(hi << 4 | lo); p->mask[p->n++] = 1;
        s += 2;
    }
    return p->n;
}

/* 1 if the compiled pattern matches at addr (addr must have p->n readable bytes). */
static int v3x_pattern_match_at(const V3xPattern *V3X_NN p, const uint8_t *V3X_NN addr)
{
    if (!p || !addr) return 0;
    for (int k = 0; k < p->n; k++)
        if (p->mask[k] && addr[k] != p->bytes[k]) return 0;
    return p->n > 0;
}

/* Calls cb(match, user) for every match in sec (stops when cb returns non-zero). Returns the
 * number of matches visited, -1 on a bad pattern. cb may be NULL (just count). */
static int v3x_scan_each(const V3xSection *V3X_NN sec, const char *V3X_NN pattern, int (*cb)(uint8_t *match, void *user),
                         void *user)
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
static int v3x_scan_count(const V3xSection *V3X_NN sec, const char *V3X_NN pattern, void **first)
{
    uint8_t *f = NULL;
    int n = v3x_scan_each(sec, pattern, v3x_scan__first, &f);
    if (first) *first = f;
    return n;
}

/* ---- bounded decoding */

/* insn + len + disp32 (read at insn + disp_off, inside code) as an integer address; 0 if out of code */
static uintptr_t v3x_rel_calc_(const V3xSection *code, const uint8_t *insn, size_t disp_off, size_t insn_len)
{
    if (insn_len < disp_off + 4 || !v3x_sec_has_(code, insn, insn_len)) return 0;
    int32_t d;
    memcpy(&d, insn + disp_off, 4);
    return (uintptr_t)insn + insn_len + (uintptr_t)(intptr_t)d;   /* unsigned wrap is defined */
}

static void *v3x_in_dest_(const V3xSection *code, const V3xSection *dest, uintptr_t t)
{
    const V3xSection *d = dest ? dest : code;
    if (!t || !d || !d->ptr) return NULL;
    uintptr_t lo = (uintptr_t)d->ptr;
    return t >= lo && t - lo < d->size ? (void *)t : NULL;
}

/* Target of a rel32 call/jmp at insn (E8 / E9 rel32, or 0F 8x rel32 jcc) whose bytes lie inside code.
 * NULL if it is none of them, the instruction is not inside code, or the target is not inside dest
 * (NULL = code). */
static V3X_MUST_USE void *v3x_rel32_target_in(const V3xSection *V3X_NN code, const void *V3X_NN insn, const V3xSection *dest)
{
    const uint8_t *p = (const uint8_t *)insn;
    if (!v3x_sec_has_(code, p, 1)) return NULL;
    if (p[0] == 0xE8 || p[0] == 0xE9) return v3x_in_dest_(code, dest, v3x_rel_calc_(code, p, 1, 5));
    if (p[0] == 0x0F && v3x_sec_has_(code, p, 2) && (p[1] & 0xF0) == 0x80)
        return v3x_in_dest_(code, dest, v3x_rel_calc_(code, p, 2, 6));
    return NULL;
}

/* Target of a RIP-relative operand: disp32 at insn+disp_off, instruction length insn_len (both
 * inside code; disp_off >= 0, insn_len >= disp_off + 4). NULL if out of code or the target is not
 * inside dest (NULL = code; pass the image range for globals in .data / .rdata).
 * `mov rax, [rip+X]` = 48 8B 05 X: (p, 3, 7). `lea rcx, [rip+X]` = 48 8D 0D X: same. */
static V3X_MUST_USE void *v3x_rip_target_in(const V3xSection *V3X_NN code, const void *V3X_NN insn, int disp_off, int insn_len,
                                            const V3xSection *dest)
{
    if (disp_off < 0 || insn_len < 0) return NULL;
    return v3x_in_dest_(code, dest, v3x_rel_calc_(code, (const uint8_t *)insn, (size_t)disp_off, (size_t)insn_len));
}

/* Searches forward from `from` (inside sec) for up to max_bytes, never past the end of sec, for the
 * pattern (e.g. inside one function). Returns the match or NULL. */
static V3X_MUST_USE void *v3x_scan_near_in(const V3xSection *V3X_NN sec, const void *V3X_NN from, size_t max_bytes,
                                           const char *V3X_NN pattern)
{
    V3xPattern p;
    if (!v3x_sec_has_(sec, from, 0) || !v3x_pattern_compile(pattern, &p)) return NULL;
    const uint8_t *b = (const uint8_t *)from;
    size_t room = sec->size - (size_t)((uintptr_t)b - (uintptr_t)sec->ptr);
    if (max_bytes > room) max_bytes = room;
    for (size_t i = 0; i + (size_t)p.n <= max_bytes; i++)
        if (v3x_pattern_match_at(&p, b + i)) return (void *)(b + i);
    return NULL;
}

/* Every RIP-relative `lea/mov r64, [rip+X]` (REX 48/4C, 8D/8B, modrm rm=101 mod=00; the same
 * encoding api->find_xref recognises) in sec whose target is `target`. Writes up to max of them
 * to out (may be NULL) and returns the total count. */
static int v3x_xrefs(const V3xSection *V3X_NN sec, const void *target, void **out, int max)
{
    int n = 0;
    for (size_t i = 0; sec && sec->ptr && i + 7 <= sec->size; i++) {
        const uint8_t *p = sec->ptr + i;
        if ((p[0] != 0x48 && p[0] != 0x4C) || (p[1] != 0x8D && p[1] != 0x8B) || (p[2] & 0xC7) != 0x05) continue;
        if (v3x_rel_calc_(sec, p, 3, 7) != (uintptr_t)target) continue;
        if (out && n < max) out[n] = (void *)p;
        n++;
    }
    return n;
}

/* Every direct `call target` (E8 rel32) in sec. Same out/max/return convention as v3x_xrefs. */
static int v3x_callers(const V3xSection *V3X_NN sec, const void *target, void **out, int max)
{
    int n = 0;
    for (size_t i = 0; sec && sec->ptr && i + 5 <= sec->size; i++) {
        if (sec->ptr[i] != 0xE8 || v3x_rel_calc_(sec, sec->ptr + i, 1, 5) != (uintptr_t)target) continue;
        if (out && n < max) out[n] = sec->ptr + i;
        n++;
    }
    return n;
}

/* ---- deprecated unbounded names (kept for source compatibility) */

/* The readable range around addr: the whole PE image that contains it (any mapped image, also an exe
 * mapped by the testkit), else the committed region. 1 = found. */
static int v3x_range_of_(const void *addr, V3xSection *out)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!addr || !VirtualQuery(addr, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mi.Protect == PAGE_EXECUTE || !mi.Protect)
        return 0;
    if (mi.Type == MEM_IMAGE && mi.AllocationBase) {
        MEMORY_BASIC_INFORMATION hi;
        if (VirtualQuery(mi.AllocationBase, &hi, sizeof hi) && hi.State == MEM_COMMIT &&
            !(hi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && hi.Protect && hi.RegionSize >= 0x400 &&
            v3x_image_range((uintptr_t)mi.AllocationBase, out) && v3x_sec_has_(out, addr, 1))
            return 1;
    }
    out->ptr = (uint8_t *)mi.BaseAddress;
    out->size = mi.RegionSize;
    return 1;
}

/* DEPRECATED: v3x_rel32_target_in(&code, insn, NULL). Target must lie in the same image / region. */
V3X_SCAN_DEPRECATED_("use v3x_rel32_target_in(&text, insn, NULL)")
static void *v3x_rel32_target(const void *V3X_NN insn)
{
    V3xSection r;
    return v3x_range_of_(insn, &r) ? v3x_rel32_target_in(&r, insn, NULL) : NULL;
}

/* DEPRECATED: v3x_rip_target_in(&code, insn, disp_off, insn_len, &image). */
V3X_SCAN_DEPRECATED_("use v3x_rip_target_in(&text, insn, disp_off, insn_len, &image)")
static void *v3x_rip_target(const void *V3X_NN insn, int disp_off, int insn_len)
{
    V3xSection r;
    return v3x_range_of_(insn, &r) ? v3x_rip_target_in(&r, insn, disp_off, insn_len, NULL) : NULL;
}

/* DEPRECATED: v3x_scan_near_in(&text, from, max_bytes, pattern). Never reads past the end of the
 * image / region that contains from. */
V3X_SCAN_DEPRECATED_("use v3x_scan_near_in(&text, from, max_bytes, pattern)")
static void *v3x_scan_near(const void *V3X_NN from, size_t max_bytes, const char *V3X_NN pattern)
{
    V3xSection r;
    return v3x_range_of_(from, &r) ? v3x_scan_near_in(&r, from, max_bytes, pattern) : NULL;
}

V3X_NN_END

#endif
