/* v3x_rtti.h - header-only MSVC x64 RTTI lookup for V3X plugins (C, no CRT needed).
 *
 * MSVC x64 layout (all "RVA" fields are 32-bit offsets from the image base):
 *   TypeDescriptor        { void *vftable; void *spare; char name[]; }   name = ".?AVName@@"
 *   CompleteObjectLocator { u32 signature (=1 on x64); u32 offset; u32 cdOffset;
 *                           u32 pTypeDescriptor; u32 pClassDescriptor; u32 pSelf; }
 *   vtable[-1] = absolute pointer to the COL; vtable[0..] = virtual functions.
 *
 * Build: one pass over .rdata collects every COL (signature==1 and pSelf == its own RVA),
 * a second pass finds the qword that points at each COL (vtable = that qword + 8).
 * About 36k entries for victoria3.exe, 16 bytes each, HeapAlloc'd. Then lookups are linear
 * scans over the small index (names are read from the TypeDescriptors in place).
 *
 *   V3xRtti r;
 *   if (v3x_rtti_build(&r, api->image_base(), api->image_size()) == 0) {
 *       void *vt = v3x_rtti_find_vtable(&r, "CTickTask_UpdateEmployment");
 *       v3x_rtti_enum(&r, "CTickTask_", my_cb, my_user);
 *       v3x_rtti_free(&r);
 *   }
 *
 * Names: lookups take the plain class name ("CFoo") and match ".?AVCFoo@@", ".?AUCFoo@@" and
 * anonymous/named-namespace forms (".?AVCFoo@?A0x1234abcd@@", ".?AVCFoo@ns@@"). A name starting
 * with ".?A" is compared exactly against the mangled string instead. Only primary vtables
 * (COL offset 0) are reported. */
#ifndef V3X_RTTI_H
#define V3X_RTTI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct { uint32_t td, col, vt, off; } V3xRttiEntry;   /* RVAs; vt = 0 if no vtable found */

typedef struct {
    uintptr_t base;        /* where the image is mapped */
    uint64_t ptr_base;     /* base that absolute pointers inside the image are relative to */
    uint32_t img_size;
    V3xRttiEntry *e;
    uint32_t n, cap;
} V3xRtti;

/* callback for v3x_rtti_enum: name = class name without ".?AV" and namespace ("CTickTask_X") */
typedef void (*v3x_rtti_cb)(const char *name, void *vtable, void *user);

static void v3x_rtti_free(V3xRtti *r)
{
    if (r->e) HeapFree(GetProcessHeap(), 0, r->e);
    r->e = 0; r->n = r->cap = 0;
}

static int v3x_rtti__col_cmp_find(const V3xRtti *r, uint32_t col)   /* e[] sorted by col */
{
    uint32_t lo = 0, hi = r->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (r->e[mid].col < col) lo = mid + 1; else hi = mid;
    }
    return (lo < r->n && r->e[lo].col == col) ? (int)lo : -1;
}

/* Scan RVA range [lo, hi) of an image mapped at `base` (pointers relative to `ptr_base`;
 * equal to base for a loaded, relocated module). Returns 0 or -1 on allocation failure. */
static int v3x_rtti_build_range(V3xRtti *r, uintptr_t base, uint64_t ptr_base, uint32_t img_size,
                                uint32_t lo, uint32_t hi)
{
    r->base = base; r->ptr_base = ptr_base; r->img_size = img_size;
    r->e = 0; r->n = r->cap = 0;
    lo = (lo + 3) & ~3u;
    if (hi < lo + 24) return 0;
    /* pass 1: COLs (ascending RVA, so e[] ends up sorted by col) */
    for (uint32_t rva = lo; rva + 24 <= hi; rva += 4) {
        const uint32_t *c = (const uint32_t *)(base + rva);
        if (c[0] != 1 || c[5] != rva) continue;
        uint32_t td = c[3];
        if (td == 0 || td + 20 > img_size || c[1] > 0x10000000u) continue;
        const char *nm = (const char *)(base + td + 16);
        if (nm[0] != '.' || nm[1] != '?' || nm[2] != 'A') continue;
        if (r->n == r->cap) {
            uint32_t nc = r->cap ? r->cap * 2 : 4096;
            void *p = r->e ? HeapReAlloc(GetProcessHeap(), 0, r->e, nc * sizeof *r->e)
                           : HeapAlloc(GetProcessHeap(), 0, nc * sizeof *r->e);
            if (!p) { v3x_rtti_free(r); return -1; }
            r->e = (V3xRttiEntry *)p; r->cap = nc;
        }
        V3xRttiEntry *e = &r->e[r->n++];
        e->td = td; e->col = rva; e->vt = 0; e->off = c[1];
    }
    if (!r->n) return 0;
    /* pass 2: vtables = qword pointing at a COL, +8 */
    uint64_t pmin = ptr_base + r->e[0].col, pmax = ptr_base + r->e[r->n - 1].col;
    for (uint32_t rva = (lo + 7) & ~7u; rva + 16 <= hi; rva += 8) {
        uint64_t v = *(const uint64_t *)(base + rva);
        if (v < pmin || v > pmax) continue;
        int k = v3x_rtti__col_cmp_find(r, (uint32_t)(v - ptr_base));
        if (k >= 0 && !r->e[k].vt) r->e[k].vt = rva + 8;
    }
    return 0;
}

/* Parse the PE headers at `base` and scan every .rdata section. Returns 0, -1 alloc, -2 bad PE. */
static int v3x_rtti_build_ex(V3xRtti *r, uintptr_t base, size_t size, uint64_t ptr_base)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    r->e = 0; r->n = r->cap = 0;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return -2;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -2;
    if (!size) size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(s[i].Name, ".rdata", 7) != 0) continue;
        uint32_t lo = s[i].VirtualAddress, hi = lo + s[i].Misc.VirtualSize;
        if (hi > size) return -2;
        return v3x_rtti_build_range(r, base, ptr_base, (uint32_t)size, lo, hi);
    }
    return -2;
}

static int v3x_rtti_build(V3xRtti *r, uintptr_t base, size_t size)
{
    return v3x_rtti_build_ex(r, base, size, base);
}

static const char *v3x_rtti_mangled(const V3xRtti *r, const V3xRttiEntry *e)
{
    return (const char *)(r->base + e->td + 16);
}

/* Does the mangled name ".?AVxxx@..." denote plain class `name` (see header comment)? */
static int v3x_rtti__match(const char *m, const char *name, int prefix_only)
{
    if (name[0] == '.' && name[1] == '?' && name[2] == 'A') {
        size_t i = 0;
        for (; name[i]; i++) if (m[i] != name[i]) return 0;
        return prefix_only || m[i] == 0;
    }
    if (m[0] != '.' || m[1] != '?' || m[2] != 'A' || (m[3] != 'V' && m[3] != 'U')) return 0;
    m += 4;
    size_t i = 0;
    for (; name[i]; i++) if (m[i] != name[i]) return 0;
    if (prefix_only) {
        for (const char *p = m; *p != '@'; p++) if (!*p || *p == '<' || *p == '?') return 0;
    } else if (m[i] != '@') return 0;
    const char *p = m; while (*p && *p != '@') p++;
    /* must end in "@@" and the first component must not be a template/lambda */
    size_t len = 0; while (p[len]) len++;
    return len >= 2 && p[len - 1] == '@' && p[len - 2] == '@' && m[0] != '?' && m[0] != '<';
}

/* Primary vtable of class `name`, or NULL. If several (namespaces) match, the first wins. */
static void *v3x_rtti_find_vtable(const V3xRtti *r, const char *name)
{
    for (uint32_t i = 0; i < r->n; i++) {
        const V3xRttiEntry *e = &r->e[i];
        if (e->off || !e->vt) continue;
        if (v3x_rtti__match(v3x_rtti_mangled(r, e), name, 0)) return (void *)(r->base + e->vt);
    }
    return 0;
}

/* Calls cb for every primary vtable whose class name starts with `prefix`. Returns the count. */
static int v3x_rtti_enum(const V3xRtti *r, const char *prefix, v3x_rtti_cb cb, void *user)
{
    int n = 0;
    char buf[256];
    for (uint32_t i = 0; i < r->n; i++) {
        const V3xRttiEntry *e = &r->e[i];
        if (e->off || !e->vt) continue;
        const char *m = v3x_rtti_mangled(r, e);
        if (!v3x_rtti__match(m, prefix, 1)) continue;
        size_t k = 0;
        for (const char *p = m + 4; *p && *p != '@' && k < sizeof buf - 1; p++) buf[k++] = *p;
        buf[k] = 0;
        n++;
        if (cb) cb(buf, (void *)(r->base + e->vt), user);
    }
    return n;
}

#endif
