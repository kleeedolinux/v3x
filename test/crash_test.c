/* Test for the crash_logger plugin without the game. Imports version.dll (so V3X boots and loads
 * v3x/plugins/ DLLs before main), then:
 *   1. simulates MSVC C++ throws (zig is GNU ABI, so RaiseException(0xE06D7363) with a hand-built
 *      ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor, RVAs relative to our base),
 *      5x from one site (dedup: only 3 must be logged) and once from another site;
 *   2. triggers a read access violation on a reserved page and survives it;
 *   3. runs copies of itself (child_<mode>/ layouts) with "abort" (SIGABRT, exit 3) and "overflow";
 * and checks v3x/crash.log. Our own VEH is registered last, so crash_logger (first) sees everything
 * first; ours then resumes execution. Exit code 0 = all checks passed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const void *vft; void *spare; char name[64]; } TypeDescriptor;
typedef struct { unsigned props; int pType; int mdisp, pdisp, vdisp; int size; int copyFn; } CatchableType;
typedef struct { int n; int types[2]; } CatchableTypeArray;
typedef struct { unsigned attributes; int pmfnUnwind; int pForwardCompat; int pCatchableTypeArray; } ThrowInfo;
typedef struct { const void *vptr; const char *what; char dofree; } FakeStdException;

static const void *fake_vtbl[2];
static TypeDescriptor td_rt = {fake_vtbl, 0, ".?AVruntime_error@std@@"};
static TypeDescriptor td_ex = {fake_vtbl, 0, ".?AVexception@std@@"};
static CatchableType ct_rt, ct_ex;
static CatchableTypeArray cta;
static ThrowInfo throw_info;
static FakeStdException exc_obj = {fake_vtbl, "simulated failure: out of widgets", 0};

static char *g_page;

static int rva(const void *p) { return (int)((const char *)p - (const char *)GetModuleHandleA(NULL)); }

static LONG CALLBACK test_veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == 0xE06D7363) return EXCEPTION_CONTINUE_EXECUTION;  /* "caught" */
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
        (char *)er->ExceptionInformation[1] >= g_page && (char *)er->ExceptionInformation[1] < g_page + 4096) {
        VirtualAlloc(g_page, 4096, MEM_COMMIT, PAGE_READWRITE);              /* retry succeeds */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((noinline)) static void throw_simulated(void)
{
    ULONG_PTR params[4] = {0x19930520, (ULONG_PTR)&exc_obj, (ULONG_PTR)&throw_info,
                           (ULONG_PTR)GetModuleHandleA(NULL)};
    RaiseException(0xE06D7363, 0, 4, params);
}
__attribute__((noinline)) static void throw_site_a(void) { throw_simulated(); __asm__ volatile(""); }
__attribute__((noinline)) static void throw_site_b(void) { throw_simulated(); __asm__ volatile(""); }

__attribute__((noinline)) static int read_bad(volatile char *p) { return p[16]; }

static char g_log[1 << 20];
static int read_crash_log(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { g_log[0] = 0; return 0; }
    DWORD n = 0;
    ReadFile(f, g_log, sizeof g_log - 1, &n, NULL);
    CloseHandle(f);
    g_log[n] = 0;
    return 1;
}
static int count(const char *needle)
{
    int c = 0;
    for (const char *p = g_log; (p = strstr(p, needle)); p++) c++;
    return c;
}
static int check(const char *what, int ok)
{
    printf("%-60s %s\n", what, ok ? "OK" : "FAIL");
    return !ok;
}

__attribute__((noinline)) static int recurse(int n)
{
    volatile char pad[4096];
    pad[0] = (char)n;
    return n ? recurse(n + 1) + pad[0] : 0;
}

/* Runs a copy of this exe in <dir>\child_<mode>\ (own exe + version.dll + v3x\plugins), because the
 * V3X core keeps v3x.log open deny-write, so a child in the same layout could not write its v3x.log.
 * Writes the child's v3x directory (with trailing backslash) to child_v3x. */
static DWORD run_child(const char *dir, const char *mode, char *child_v3x)
{
    char root[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], exe[MAX_PATH];
    snprintf(root, sizeof root, "%schild_%s\\", dir, mode);
    snprintf(child_v3x, MAX_PATH, "%sv3x\\", root);
    CreateDirectoryA(root, NULL);
    CreateDirectoryA(child_v3x, NULL);
    snprintf(dst, sizeof dst, "%splugins", child_v3x);
    CreateDirectoryA(dst, NULL);
    snprintf(src, sizeof src, "%scrash_test.exe", dir);
    snprintf(exe, sizeof exe, "%scrash_test.exe", root);
    CopyFileA(src, exe, FALSE);
    snprintf(src, sizeof src, "%sversion.dll", dir);
    snprintf(dst, sizeof dst, "%sversion.dll", root);
    CopyFileA(src, dst, FALSE);
    WIN32_FIND_DATAA fd;
    snprintf(src, sizeof src, "%sv3x\\plugins\\*.dll", dir);
    HANDLE h = FindFirstFileA(src, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            snprintf(src, sizeof src, "%sv3x\\plugins\\%s", dir, fd.cFileName);
            snprintf(dst, sizeof dst, "%splugins\\%s", child_v3x, fd.cFileName);
            CopyFileA(src, dst, FALSE);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, mode);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 0xFFFFFFFF;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 30000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return code;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "abort")) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        abort();
    }
    if (argc > 1 && !strcmp(argv[1], "overflow")) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        return recurse(1);
    }

    DWORD vh; /* any version.dll import keeps the DLL in the import table (that boots V3X) */
    if (!GetFileVersionInfoSizeA("C:/Windows/System32/kernel32.dll", &vh)) printf("version.dll forward failed\n");

    ct_rt.pType = rva(&td_rt); ct_rt.size = sizeof exc_obj;
    ct_ex.pType = rva(&td_ex); ct_ex.size = sizeof exc_obj;
    cta.n = 2; cta.types[0] = rva(&ct_rt); cta.types[1] = rva(&ct_ex);
    throw_info.pCatchableTypeArray = rva(&cta);

    AddVectoredExceptionHandler(0, test_veh);

    for (int i = 0; i < 5; i++) throw_site_a();
    throw_site_b();
    printf("survived 6 simulated C++ throws\n");

    g_page = VirtualAlloc(NULL, 4096, MEM_RESERVE, PAGE_NOACCESS);
    int v = read_bad(g_page);
    printf("survived read access violation (read %d)\n", v);

    char exe[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    strcpy(path, exe);
    *(strrchr(path, '\\') + 1) = 0;
    strcat(path, "v3x\\crash.log");

    int fails = 0;
    fails += check("crash.log readable", read_crash_log(path));
    fails += check("decoded type names", strstr(g_log, "std::runtime_error, std::exception") != NULL);
    fails += check("what() decoded", strstr(g_log, "what(): \"simulated failure: out of widgets\"") != NULL);
    fails += check("dedup: 3 from site A + 1 from site B logged", count("C++ EXCEPTION #") == 4);
    fails += check("suppressed counter", strstr(g_log, "repeated throws suppressed") != NULL);
    fails += check("stack has crash_test.exe+0x frames", count("crash_test.exe+0x") >= 8);
    fails += check("memory snapshot", strstr(g_log, "commit avail=") != NULL);
    char av[64];
    snprintf(av, sizeof av, "read of address 0x%llx", (unsigned long long)(uintptr_t)(g_page + 16));
    fails += check("access violation with read + address", strstr(g_log, "EXCEPTION ACCESS_VIOLATION") && strstr(g_log, av));
    fails += check("registers logged", strstr(g_log, "rip=") != NULL);

    MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);  /* drives on_frame (memory sampler) */
    read_crash_log(path);
    fails += check("memory sampler logged on first frame", strstr(g_log, "mem (first frame)") != NULL);

    /* one-line summaries mirrored to api->log (v3x.log + console) */
    char vlog[MAX_PATH];
    strcpy(vlog, path);
    strcpy(strrchr(vlog, '\\') + 1, "v3x.log");
    fails += check("v3x.log readable", read_crash_log(vlog));
    fails += check("v3x.log: C++ summary with type, what() and site",
                   strstr(g_log, "[crash_logger] EXCEPTION C++ std::runtime_error, std::exception what()=\"simulated "
                                 "failure: out of widgets\" at crash_test.exe+0x") != NULL);
    fails += check("v3x.log: C++ summaries deduplicated like crash.log", count("EXCEPTION C++ ") == 4);
    fails += check("v3x.log: summary has commit avail", strstr(g_log, "(commit avail ") != NULL);
    snprintf(av, sizeof av, "EXCEPTION ACCESS_VIOLATION read 0x%llx at crash_test.exe+0x",
             (unsigned long long)(uintptr_t)(g_page + 16));
    fails += check("v3x.log: access violation summary", strstr(g_log, av) != NULL);
    fails += check("v3x.log: sampler summary", strstr(g_log, "[crash_logger] mem (first frame)") != NULL);
    fails += check("v3x.log: no stack frames (detail stays in crash.log)", strstr(g_log, "  #00 ") == NULL);

    /* fatal paths in child processes, each in its own layout under child_<mode>\ */
    char dir[MAX_PATH], cv3x[MAX_PATH], cpath[MAX_PATH];
    strcpy(dir, exe);
    *(strrchr(dir, '\\') + 1) = 0;
    DWORD code = run_child(dir, "abort", cv3x);
    printf("abort child exit code %lu\n", code);
    fails += check("abort child exited with 3", code == 3);
    snprintf(cpath, sizeof cpath, "%scrash.log", cv3x);
    read_crash_log(cpath);
    fails += check("abort logged with stack + memory", strstr(g_log, "abort called (SIGABRT)") &&
                   strstr(g_log, "crash_test.exe+0x") && strstr(g_log, "commit avail="));
    snprintf(cpath, sizeof cpath, "%sv3x.log", cv3x);
    read_crash_log(cpath);
    fails += check("v3x.log (abort child): ERROR abort summary",
                   strstr(g_log, "[crash_logger] ERROR abort called (SIGABRT)") != NULL);

    code = run_child(dir, "overflow", cv3x);
    printf("overflow child exit code 0x%lx\n", code);
    fails += check("overflow child died with 0xC00000FD", code == 0xC00000FD);
    snprintf(cpath, sizeof cpath, "%scrash.log", cv3x);
    read_crash_log(cpath);
    fails += check("stack overflow logged with module offset",
                   strstr(g_log, "STACK_OVERFLOW (0xC00000FD)") && strstr(g_log, " module=crash_test.exe") && strstr(g_log, " offset=0x"));
    snprintf(cpath, sizeof cpath, "%sv3x.log", cv3x);
    read_crash_log(cpath);
    fails += check("v3x.log (overflow child): no api->log on overflow path",
                   strstr(g_log, "[crash_logger] logging to") && !strstr(g_log, "STACK_OVERFLOW") &&
                   !strstr(g_log, "EXCEPTION"));
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
