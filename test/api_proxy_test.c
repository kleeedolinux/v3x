/* Test of src/proxy.c (audit L1): the version.dll forwards fail closed.
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -o $T/proxy.exe test/api_proxy_test.c
 *   $T/proxy.exe
 * 1. Before any resolve every export is the fallback: FALSE / 0 with GetLastError ERROR_MOD_NOT_FOUND,
 *    output lengths zeroed (no jump through NULL).
 * 2. Resolving from a DLL that does not exist returns -1 and keeps the fallbacks.
 * 3. Resolving from kernel32 (has only VerLanguageNameA/W) counts 15 missing, those keep their fallbacks.
 * 4. The real resolve (System32\version.dll) forwards: GetFileVersionInfoSizeW(kernel32.dll) > 0.
 * Exit code 0 = ALL OK. */
#include "../src/proxy.c"

#include <stdio.h>
#include <string.h>

/* the proxy's own exports (asm stubs above), declared here without windows.h like proxy.c */
extern unsigned __stdcall GetLastError(void);
extern unsigned __stdcall GetFileVersionInfoSizeW(const wc *f, unsigned *h);
extern int __stdcall GetFileVersionInfoW(const wc *f, unsigned h, unsigned n, void *d);
extern int __stdcall VerQueryValueW(const void *b, const wc *sub, void **out, unsigned *n);
extern unsigned __stdcall VerFindFileW(unsigned fl, const wc *f, const wc *w, const wc *a, wc *cur, unsigned *curn, wc *dst,
                                       unsigned *dstn);
extern unsigned __stdcall VerLanguageNameA(unsigned lang, char *buf, unsigned n);
extern unsigned __stdcall GetSystemDirectoryW(unsigned short *, unsigned);

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static const wc *W(const char *s, wc *buf)
{
    size_t i = 0;
    for (; s[i]; i++) buf[i] = (wc)(unsigned char)s[i];
    buf[i] = 0;
    return buf;
}

static void check_fallbacks(const char *when, int lang_too)
{
    wc b[600], k32[600];
    unsigned n = GetSystemDirectoryW(k32, 500);
    W("\\kernel32.dll", k32 + n);
    unsigned h = 77;
    CHECK(GetFileVersionInfoSizeW(k32, &h) == 0 && h == 0 && GetLastError() == V3X_ERROR_MOD_NOT_FOUND,
          "%s: GetFileVersionInfoSizeW -> 0, ERROR_MOD_NOT_FOUND", when);
    char data[64];
    CHECK(GetFileVersionInfoW(k32, 0, sizeof data, data) == 0, "%s: GetFileVersionInfoW -> FALSE", when);
    void *out = (void *)1;
    unsigned len = 5;
    CHECK(VerQueryValueW(data, W("\\", b), &out, &len) == 0 && out == NULL && len == 0, "%s: VerQueryValueW -> FALSE, outputs zeroed", when);
    wc cur[8], dst[8];
    unsigned cn = 8, dn = 8;
    cur[0] = dst[0] = 'x';
    CHECK(VerFindFileW(0, W("a.dll", b), NULL, NULL, cur, &cn, dst, &dn) == V3X_VFF_BUFFTOOSMALL && cn == 0 && dn == 0 && cur[0] == 0 &&
              dst[0] == 0, "%s: VerFindFileW -> VFF_BUFFTOOSMALL, buffers emptied", when);
    char lang[16] = "zz";
    if (lang_too) CHECK(VerLanguageNameA(0x409, lang, sizeof lang) == 0 && lang[0] == 0, "%s: VerLanguageNameA -> 0", when);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    check_fallbacks("unresolved", 1);
    wc b[600];
    CHECK(v3x_proxy_resolve_from(W("C:\\no\\such\\dir\\version.dll", b)) == -1 && v3x_proxy_resolve_from(NULL) == -1,
          "resolve from a missing DLL -> -1");
    check_fallbacks("missing DLL", 1);
    wc k32[600];
    unsigned n = GetSystemDirectoryW(k32, 500);
    W("\\kernel32.dll", k32 + n);
    /* kernel32 has VerLanguageNameA/W only: those 2 forward, the other 15 keep their fallbacks */
    CHECK(v3x_proxy_resolve_from(k32) == 15, "resolve from kernel32 (2 of 17 exports) -> 15 missing");
    check_fallbacks("exports missing", 0);
    CHECK(v3x_proxy_resolve() == 0, "real System32\\version.dll resolves all 17");
    unsigned h = 0;
    CHECK(GetFileVersionInfoSizeW(k32, &h) > 0, "forwarded GetFileVersionInfoSizeW(kernel32.dll) > 0");
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
