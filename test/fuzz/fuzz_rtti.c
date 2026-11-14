/* Fuzz harness: include/v3x_rtti.h (audit L7). The input is treated as an image mapped at a base: once
 * as a raw .rdata range (v3x_rtti_build_range over the whole buffer) and once as a PE file
 * (v3x_rtti_build_ex with the size given). The buffer ends at a guard page, so any read past the
 * image (a wrapped RVA, an unterminated TypeDescriptor name) crashes. Lookups and enumeration then
 * read the validated names. The seed builds a small valid image with two classes. */
#include "v3x_fuzz.h"
#include "v3x_rtti.h"

static void enum_cb(const char *name, void *vt, void *user) { (void)vt; *(size_t *)user += strlen(name); }

static void exercise(V3xRtti *r)
{
    size_t total = 0;
    (void)v3x_rtti_find_vtable(r, "CFoo");
    (void)v3x_rtti_find_vtable(r, ".?AVCFoo@@");
    (void)v3x_rtti_enum(r, "C", enum_cb, &total);
    (void)v3x_rtti_enum(r, "", enum_cb, &total);
    for (uint32_t i = 0; i < r->n; i++) {
        V3X_FUZZ_ASSERT((uint64_t)r->e[i].td + 16 < r->img_size);
        V3X_FUZZ_ASSERT(memchr(v3x_rtti_mangled(r, &r->e[i]), 0, r->img_size - r->e[i].td - 16) != NULL);
    }
    v3x_rtti_free(r);
}

static void fuzz_rtti(const uint8_t *d, size_t n)
{
    n &= ~(size_t)7;                       /* 8-aligned start AND end at the guard page */
    if (n < 8) return;
    uint8_t *img = (uint8_t *)v3x_fuzz_guarded(d, n);
    V3xRtti r;
    /* pointers inside the image are relative to the buffer address (like a loaded module) */
    if (v3x_rtti_build_range(&r, (uintptr_t)img, (uint64_t)(uintptr_t)img, (uint32_t)n, 0, (uint32_t)n) == 0) exercise(&r);
    /* and relative to a fake preferred base taken from the input */
    uint64_t pref;
    memcpy(&pref, img, 8);
    if (v3x_rtti_build_range(&r, (uintptr_t)img, pref, (uint32_t)n, 8, (uint32_t)n) == 0) exercise(&r);
    if (v3x_rtti_build_ex(&r, (uintptr_t)img, n, (uint64_t)(uintptr_t)img) == 0) exercise(&r);
}
V3X_FUZZ_TARGET(rtti, fuzz_rtti)

/* seed: TD "CFoo" at 0x40, COL at 0x80 (pSelf = 0x80), vtable qword at 0xa0 -> filled at runtime is
 * impossible (pointers are absolute), so the seed carries a COL whose vtable is found only in the
 * second (pref) pass: pref = first qword = 0, vtable qword = 0x80. */
V3X_FUZZ_SEED(rtti,
    "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" ".?AVCFoo@@\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x01\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x40\x00\x00\x00" "\x00\x00\x00\x00\x80\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x80\x00\x00\x00\x00\x00\x00\x00" "\x10\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00")
V3X_FUZZ_SEED(rtti, "MZ\x90\x00\x03\x00\x00\x00\x04\x00\x00\x00\xff\xff\x00\x00" "\xb8\x00\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00"
    "PE\x00\x00\x64\x86\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00" "\x00\x00\x00\x00\xf0\x00\x22\x00\x0b\x02\x00\x00\x00\x00\x00\x00")
