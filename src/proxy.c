/* version.dll export forwarding.
 * Every export is a one-instruction jump through a pointer filled by v3x_proxy_resolve()
 * from the real %SystemRoot%\System32\version.dll. No windows.h here on purpose: the
 * export names would collide with the winver.h prototypes. */

#define V3X_PROXY_LIST(X) \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA) \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA) \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW) \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW) \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define X(name) void *v3x_real_##name;
V3X_PROXY_LIST(X)
#undef X

#define X(name)                                                       \
    __asm__(".text\n.globl " #name "\n.p2align 4\n" #name ":\n"       \
            "    jmp *v3x_real_" #name "(%rip)\n"                     \
            ".section .drectve\n.ascii \" -export:" #name "\"\n.text\n");
V3X_PROXY_LIST(X)
#undef X

extern void *__stdcall LoadLibraryW(const unsigned short *);
extern void *__stdcall GetProcAddress(void *, const char *);
extern unsigned __stdcall GetSystemDirectoryW(unsigned short *, unsigned);

/* Returns number of exports that could not be resolved (0 = ok). */
int v3x_proxy_resolve(void)
{
    unsigned short path[520];
    unsigned n = GetSystemDirectoryW(path, 500);
    const char *tail = "\\version.dll";
    for (int i = 0; tail[i]; i++) path[n++] = (unsigned short)tail[i];
    path[n] = 0;
    void *real = LoadLibraryW(path);
    if (!real) return -1;
    int missing = 0;
#define X(name) if (!(v3x_real_##name = GetProcAddress(real, #name))) missing++;
    V3X_PROXY_LIST(X)
#undef X
    return missing;
}
