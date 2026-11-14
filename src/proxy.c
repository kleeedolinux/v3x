/* version.dll export forwarding.
 * Every export is a one-instruction jump through a pointer filled by v3x_proxy_resolve()
 * from the real %SystemRoot%\System32\version.dll. No windows.h here on purpose: the
 * export names would collide with the winver.h prototypes.
 *
 * Fail closed (audit L1): every pointer starts at a fallback with the real prototype that sets
 * GetLastError = ERROR_MOD_NOT_FOUND and returns the function's documented failure value (FALSE /
 * 0 / VFF_BUFFTOOSMALL / VIF_CANNOTREADSRC). If the system DLL cannot be loaded, or one export is
 * missing, the game gets a clean error instead of a jump through NULL. */

#define V3X_PROXY_LIST(X) \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

extern void *__stdcall LoadLibraryExW(const unsigned short *, void *, unsigned);
extern void *__stdcall GetProcAddress(void *, const char *);
extern unsigned __stdcall GetSystemDirectoryW(unsigned short *, unsigned);
extern void __stdcall SetLastError(unsigned);

#define V3X_ERROR_MOD_NOT_FOUND 126u
#define V3X_VFF_BUFFTOOSMALL 0x0004u
#define V3X_VIF_CANNOTREADSRC 0x00010000u

typedef unsigned short wc;
/* Fallbacks: the documented prototypes (winver.h), failure return values. Output lengths are zeroed. */
static int __stdcall fb_GetFileVersionInfoA(const char *f, unsigned h, unsigned n, void *d)
{ (void)f; (void)h; (void)n; (void)d; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static int __stdcall fb_GetFileVersionInfoW(const wc *f, unsigned h, unsigned n, void *d)
{ (void)f; (void)h; (void)n; (void)d; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static int __stdcall fb_GetFileVersionInfoByHandle(unsigned fl, void *h, void **d, unsigned *n)
{ (void)fl; (void)h; if (d) *d = 0; if (n) *n = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static int __stdcall fb_GetFileVersionInfoExA(unsigned fl, const char *f, unsigned h, unsigned n, void *d)
{ (void)fl; (void)f; (void)h; (void)n; (void)d; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static int __stdcall fb_GetFileVersionInfoExW(unsigned fl, const wc *f, unsigned h, unsigned n, void *d)
{ (void)fl; (void)f; (void)h; (void)n; (void)d; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static unsigned __stdcall fb_GetFileVersionInfoSizeA(const char *f, unsigned *h)
{ (void)f; if (h) *h = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static unsigned __stdcall fb_GetFileVersionInfoSizeW(const wc *f, unsigned *h)
{ (void)f; if (h) *h = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static unsigned __stdcall fb_GetFileVersionInfoSizeExA(unsigned fl, const char *f, unsigned *h)
{ (void)fl; (void)f; if (h) *h = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static unsigned __stdcall fb_GetFileVersionInfoSizeExW(unsigned fl, const wc *f, unsigned *h)
{ (void)fl; (void)f; if (h) *h = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static unsigned __stdcall fb_VerFindFileA(unsigned fl, const char *f, const char *w, const char *a, char *cur, unsigned *curn,
                                          char *dst, unsigned *dstn)
{
    (void)fl; (void)f; (void)w; (void)a;
    if (cur && curn && *curn) cur[0] = 0;
    if (dst && dstn && *dstn) dst[0] = 0;
    if (curn) *curn = 0;
    if (dstn) *dstn = 0;
    SetLastError(V3X_ERROR_MOD_NOT_FOUND);
    return V3X_VFF_BUFFTOOSMALL;
}
static unsigned __stdcall fb_VerFindFileW(unsigned fl, const wc *f, const wc *w, const wc *a, wc *cur, unsigned *curn, wc *dst,
                                          unsigned *dstn)
{
    (void)fl; (void)f; (void)w; (void)a;
    if (cur && curn && *curn) cur[0] = 0;
    if (dst && dstn && *dstn) dst[0] = 0;
    if (curn) *curn = 0;
    if (dstn) *dstn = 0;
    SetLastError(V3X_ERROR_MOD_NOT_FOUND);
    return V3X_VFF_BUFFTOOSMALL;
}
static unsigned __stdcall fb_VerInstallFileA(unsigned fl, const char *s, const char *d, const char *sd, const char *dd,
                                             const char *cd, char *tmp, unsigned *tn)
{
    (void)fl; (void)s; (void)d; (void)sd; (void)dd; (void)cd;
    if (tmp && tn && *tn) tmp[0] = 0;
    if (tn) *tn = 0;
    SetLastError(V3X_ERROR_MOD_NOT_FOUND);
    return V3X_VIF_CANNOTREADSRC;
}
static unsigned __stdcall fb_VerInstallFileW(unsigned fl, const wc *s, const wc *d, const wc *sd, const wc *dd, const wc *cd,
                                             wc *tmp, unsigned *tn)
{
    (void)fl; (void)s; (void)d; (void)sd; (void)dd; (void)cd;
    if (tmp && tn && *tn) tmp[0] = 0;
    if (tn) *tn = 0;
    SetLastError(V3X_ERROR_MOD_NOT_FOUND);
    return V3X_VIF_CANNOTREADSRC;
}
static unsigned __stdcall fb_VerLanguageNameA(unsigned lang, char *buf, unsigned n)
{ (void)lang; if (buf && n) buf[0] = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static unsigned __stdcall fb_VerLanguageNameW(unsigned lang, wc *buf, unsigned n)
{ (void)lang; if (buf && n) buf[0] = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static int __stdcall fb_VerQueryValueA(const void *b, const char *sub, void **out, unsigned *n)
{ (void)b; (void)sub; if (out) *out = 0; if (n) *n = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }
static int __stdcall fb_VerQueryValueW(const void *b, const wc *sub, void **out, unsigned *n)
{ (void)b; (void)sub; if (out) *out = 0; if (n) *n = 0; SetLastError(V3X_ERROR_MOD_NOT_FOUND); return 0; }

#define X(name) void *v3x_real_##name = (void *)fb_##name;
V3X_PROXY_LIST(X)
#undef X

#define X(name)                                                       \
    __asm__(".text\n.globl " #name "\n.p2align 4\n" #name ":\n"       \
            "    jmp *v3x_real_" #name "(%rip)\n"                     \
            ".section .drectve\n.ascii \" -export:" #name "\"\n.text\n");
V3X_PROXY_LIST(X)
#undef X

/* Resolves from the DLL at `path` (wide, NUL-terminated). Returns the number of exports that could not
 * be resolved (they keep their fallback), or -1 if the DLL did not load (all keep their fallbacks). */
int v3x_proxy_resolve_from(const unsigned short *path)
{
    void *real = path ? LoadLibraryExW(path, 0, 0) : 0;
    if (!real) return -1;
    int missing = 0;
    void *p;
#define X(name) if ((p = GetProcAddress(real, #name)) != 0) v3x_real_##name = p; else missing++;
    V3X_PROXY_LIST(X)
#undef X
    return missing;
}

/* Returns the number of exports that could not be resolved (0 = ok), -1 if the system version.dll could
 * not be loaded (or the system directory is unusable). */
int v3x_proxy_resolve(void)
{
    static const char tail[] = "\\version.dll";
    unsigned short path[520];
    const unsigned cap = (unsigned)(sizeof path / sizeof path[0]);
    unsigned n = GetSystemDirectoryW(path, cap);
    /* 0 = error; >= cap = too small (the return value is then the size needed) */
    if (n == 0 || n + sizeof tail > cap) return -1;
    for (unsigned i = 0; tail[i]; i++) path[n++] = (unsigned short)tail[i];
    path[n] = 0;
    return v3x_proxy_resolve_from(path);
}
