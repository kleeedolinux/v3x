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
 * (COL offset 0) are reported.
 *
 * Bounds (audit L7): every RVA read from the image is checked against img_size in 64-bit arithmetic
 * (no 32-bit wrap), a TypeDescriptor name must be NUL-terminated within the image (and within
 * V3X_RTTI_NAME_MAX bytes) before it is accepted, and the PE headers are checked against `size`. The
 * index then only points at validated names, so lookups never read past the image.
 *
 * NULL safety (audit action 12): a NULL V3xRtti, name or prefix is refused (build: -1, find: NULL,
 * enum: 0, free: no-op); a zeroed V3xRtti (never built, or freed) is an empty index. */
#ifndef V3X_RTTI_H
#define V3X_RTTI_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define V3X_RTTI_NAME_MAX 4096   /* longest accepted mangled name (bytes, without the NUL) */

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
    if (!r) return;
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
    if (!r) return -1;
    r->base = base; r->ptr_base = ptr_base; r->img_size = img_size;
    r->e = 0; r->n = r->cap = 0;
    if (!base) return 0;                       /* nothing mapped: an empty index */
    if (hi > img_size) hi = img_size;          /* [lo, hi) must lie inside the image */
    uint64_t lo64 = ((uint64_t)lo + 3) & ~(uint64_t)3;
    if ((uint64_t)hi < lo64 + 24) return 0;
    /* pass 1: COLs (ascending RVA, so e[] ends up sorted by col) */
    for (uint64_t rva64 = lo64; rva64 + 24 <= hi; rva64 += 4) {
        uint32_t rva = (uint32_t)rva64;
        const uint32_t *c = (const uint32_t *)(base + rva);
        if (c[0] != 1 || c[5] != rva) continue;
        uint32_t td = c[3];
        if (td == 0 || (uint64_t)td + 20 > img_size || c[1] > 0x10000000u) continue;
        const char *nm = (const char *)(base + td + 16);
        if (nm[0] != '.' || nm[1] != '?' || nm[2] != 'A') continue;
        {   /* the name must end (NUL) inside the image: lookups read it as a C string later */
            uint64_t room = (uint64_t)img_size - ((uint64_t)td + 16), k = 3;
            if (room > V3X_RTTI_NAME_MAX + 1) room = V3X_RTTI_NAME_MAX + 1;
            while (k < room && nm[k]) k++;
            if (k >= room) continue;
        }
        if (r->n == r->cap) {
            if (r->cap > 0x01000000u) { v3x_rtti_free(r); return -1; }   /* 16M entries: not an image */
            uint32_t nc = r->cap ? r->cap * 2 : 4096;
            void *p = r->e ? HeapReAlloc(GetProcessHeap(), 0, r->e, (size_t)nc * sizeof *r->e)
                           : HeapAlloc(GetProcessHeap(), 0, (size_t)nc * sizeof *r->e);
            if (!p) { v3x_rtti_free(r); return -1; }
            r->e = (V3xRttiEntry *)p; r->cap = nc;
        }
        V3xRttiEntry *e = &r->e[r->n++];
        e->td = td; e->col = rva; e->vt = 0; e->off = c[1];
    }
    if (!r->n) return 0;
    /* pass 2: vtables = qword pointing at a COL, +8 */
    uint64_t pmin = ptr_base + r->e[0].col, pmax = ptr_base + r->e[r->n - 1].col;
    for (uint64_t rva64 = ((uint64_t)lo + 7) & ~(uint64_t)7; rva64 + 16 <= hi; rva64 += 8) {
        uint64_t v;
        memcpy(&v, (const void *)(base + (uintptr_t)rva64), sizeof v);
        if (v < pmin || v > pmax) continue;
        int k = v3x_rtti__col_cmp_find(r, (uint32_t)(v - ptr_base));
        if (k >= 0 && !r->e[k].vt) r->e[k].vt = (uint32_t)rva64 + 8;
    }
    return 0;
}

/* Parse the PE headers at `base` and scan every .rdata section. Returns 0, -1 alloc, -2 bad PE. */
static int v3x_rtti_build_ex(V3xRtti *r, uintptr_t base, size_t size, uint64_t ptr_base)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!r) return -1;
    r->e = 0; r->n = r->cap = 0;
    /* size = bytes mapped at base (0 = trust SizeOfImage: a loaded module). Every header read is checked
     * and copied out with memcpy (a crafted e_lfanew may be unaligned). */
    if (!base || (size && size < sizeof dos)) return -2;
    memcpy(&dos, (const void *)base, sizeof dos);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return -2;
    uint64_t nt_off = (uint64_t)dos.e_lfanew;
    if (size && nt_off + sizeof nt > size) return -2;
    memcpy(&nt, (const void *)(base + (uintptr_t)nt_off), sizeof nt);
    if (nt.Signature != IMAGE_NT_SIGNATURE) return -2;
    if (!size) size = nt.OptionalHeader.SizeOfImage;
    if (size > 0xFFFFFFFFu) return -2;
    uint64_t sec_off = nt_off + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    if (sec_off + (uint64_t)nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > size) return -2;
    for (int i = 0; i < nt.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh;
        memcpy(&sh, (const void *)(base + (uintptr_t)sec_off + (size_t)i * sizeof sh), sizeof sh);
        if (memcmp(sh.Name, ".rdata", 7) != 0) continue;
        uint64_t lo = sh.VirtualAddress, hi = lo + sh.Misc.VirtualSize;
        if (hi > size) return -2;
        return v3x_rtti_build_range(r, base, ptr_base, (uint32_t)size, (uint32_t)lo, (uint32_t)hi);
    }
    return -2;
}

static int v3x_rtti_build(V3xRtti *r, uintptr_t base, size_t size)
{
    return v3x_rtti_build_ex(r, base, size, base);
}

static const char *v3x_rtti_mangled(const V3xRtti *r, const V3xRttiEntry *e)
{
    if (!r || !e || !r->base) return "";
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
    if (!r || !r->e || !name || !name[0]) return 0;
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
    if (!r || !r->e || !prefix) return 0;
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
