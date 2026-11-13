/* Test for include/v3x_rtti.h.
 * 1. Fake MSVC x64 RTTI built in a VirtualAlloc'd "image" (TypeDescriptors, COLs with
 *    signature 1 + pSelf, vtables with the COL pointer at [-1], plus decoys): lookup + enumeration.
 * 2. Optional: argv[1] = path to victoria3.exe, argv[2] = expected CTickTask_ count. The exe is
 *    mapped with LOAD_LIBRARY_AS_IMAGE_RESOURCE (sections at their RVAs, no relocations, so
 *    pointers are relative to the preferred ImageBase) and must match tools/rtti_dump.py.
 * Build: python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o rtti_test.exe test/rtti_test.c
 * Exit code 0 = ALL OK. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x_rtti.h"

static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static unsigned char *img;
static void put_td(uint32_t rva, const char *name) { strcpy((char *)img + rva + 16, name); }
static void put_col(uint32_t rva, uint32_t off, uint32_t td, uint32_t self)
{
    uint32_t *c = (uint32_t *)(img + rva);
    c[0] = 1; c[1] = off; c[2] = 0; c[3] = td; c[4] = 0; c[5] = self;
}
static void put_vt(uint32_t rva, uint32_t col, int nslots)
{
    uint64_t *v = (uint64_t *)(img + rva);
    v[0] = (uint64_t)(uintptr_t)img + col;
    for (int i = 1; i <= nslots; i++) v[i] = 0x1000 + i;
}

typedef struct { int n; char names[8][64]; void *vt[8]; } Seen;
static void cb(const char *name, void *vt, void *user)
{
    Seen *s = (Seen *)user;
    if (s->n < 8) { strncpy(s->names[s->n], name, 63); s->vt[s->n] = vt; }
    s->n++;
}
static int seen_has(const Seen *s, const char *n, void *vt)
{
    for (int i = 0; i < s->n && i < 8; i++) if (!strcmp(s->names[i], n) && s->vt[i] == vt) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const uint32_t SZ = 0x10000;
    img = VirtualAlloc(0, SZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    put_td(0x100, ".?AVCTickTask_Foo@@");
    put_td(0x200, ".?AVCTickTask_Bar@?A0x1234abcd@@");
    put_td(0x300, ".?AVCOther@@");
    put_td(0x400, ".?AV<lambda_1>@?1??Execute@CTickTask_Foo@@UEAAXXZ@");
    put_td(0x500, ".?AVCTickTask_Baz@@");               /* only a secondary (offset 8) vtable */
    put_td(0x600, ".?AU?$CTickTask_T@H@@");             /* template: excluded from prefix enum */
    put_td(0x700, ".?AUCTickTask_Struct@@");
    put_col(0x1000, 0, 0x100, 0x1000);
    put_col(0x1018, 0, 0x200, 0x1018);
    put_col(0x1030, 0, 0x300, 0x1030);
    put_col(0x1048, 0, 0x400, 0x1048);
    put_col(0x1060, 8, 0x500, 0x1060);
    put_col(0x1078, 0, 0x600, 0x1078);
    put_col(0x1090, 0, 0x700, 0x1090);
    put_col(0x10c0, 0, 0x100, 0x9999);                 /* decoy: wrong pSelf */
    put_col(0x10e0, 0, 0x300, 0x10e0);                 /* COL without any vtable */
    put_vt(0x2000, 0x1000, 3);
    put_vt(0x2100, 0x1018, 3);
    put_vt(0x2200, 0x1030, 3);
    put_vt(0x2300, 0x1048, 3);
    put_vt(0x2400, 0x1060, 3);
    put_vt(0x2500, 0x1078, 3);
    put_vt(0x2600, 0x1090, 3);
    put_vt(0x2700, 0x10c0, 3);                          /* points at the decoy */
    *(uint64_t *)(img + 0x2800) = (uint64_t)(uintptr_t)img + 0x1000;   /* second ref to Foo's COL: first wins */

    V3xRtti r;
    int rc = v3x_rtti_build_range(&r, (uintptr_t)img, (uintptr_t)img, SZ, 0, SZ);
    CHECK(rc == 0 && r.n == 8, "fake: build rc=%d, %u COLs (expect 8)", rc, r.n);
    CHECK(v3x_rtti_find_vtable(&r, "CTickTask_Foo") == img + 0x2008, "find CTickTask_Foo");
    CHECK(v3x_rtti_find_vtable(&r, "CTickTask_Bar") == img + 0x2108, "find CTickTask_Bar (anon namespace)");
    CHECK(v3x_rtti_find_vtable(&r, "COther") == img + 0x2208, "find COther");
    CHECK(v3x_rtti_find_vtable(&r, ".?AVCOther@@") == img + 0x2208, "find by exact mangled name");
    CHECK(v3x_rtti_find_vtable(&r, "CTickTask_Baz") == 0, "secondary-only vtable not returned");
    CHECK(v3x_rtti_find_vtable(&r, "CTickTask_Fo") == 0, "no partial-name match");
    CHECK(v3x_rtti_find_vtable(&r, "CTickTask_Struct") == img + 0x2608, "find .?AU struct");
    Seen s = {0};
    int n = v3x_rtti_enum(&r, "CTickTask_", cb, &s);
    CHECK(n == 3 && s.n == 3, "enum CTickTask_: %d (expect 3)", n);
    CHECK(seen_has(&s, "CTickTask_Foo", img + 0x2008) && seen_has(&s, "CTickTask_Bar", img + 0x2108) &&
          seen_has(&s, "CTickTask_Struct", img + 0x2608), "enum names + vtables");
    v3x_rtti_free(&r);
    VirtualFree(img, 0, MEM_RELEASE);

    if (argc >= 2) {
        HMODULE h = LoadLibraryExA(argv[1], 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        CHECK(h != 0, "map %s (err %lu)", argv[1], h ? 0 : GetLastError());
        if (h) {
            uintptr_t base = (uintptr_t)h & ~(uintptr_t)3;
            const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            rc = v3x_rtti_build_ex(&r, base, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.ImageBase);
            QueryPerformanceCounter(&t1);
            uint32_t withvt = 0;
            for (uint32_t i = 0; i < r.n; i++) withvt += r.e[i].vt != 0;
            CHECK(rc == 0 && r.n > 30000, "exe: rc=%d, %u COLs, %u with vtable, %.0f ms", rc, r.n, withvt,
                  (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
            Seen s2 = {0};
            n = v3x_rtti_enum(&r, "CTickTask_", cb, &s2);
            int want = argc >= 3 ? atoi(argv[2]) : -1;
            CHECK(want < 0 ? n >= 50 : n == want, "exe: %d CTickTask_ vtables (expect %d)", n, want);
            void *vt = v3x_rtti_find_vtable(&r, "CTickTask_UpdateEmployment");
            CHECK(vt != 0, "exe: CTickTask_UpdateEmployment vtable RVA 0x%llx, slot6 RVA 0x%llx",
                  vt ? (unsigned long long)((uintptr_t)vt - base) : 0ull,
                  vt ? (unsigned long long)(((uint64_t *)vt)[6] - nt->OptionalHeader.ImageBase) : 0ull);
            v3x_rtti_free(&r);
            FreeLibrary(h);
        }
    }
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
