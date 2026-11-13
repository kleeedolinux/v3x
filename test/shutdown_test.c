/* Process-exit test: V3X with every plugin must let the process die promptly when ExitProcess runs
 * while other threads hammer the core log lock, the CRT and the heap.
 *
 * One source, two programs:
 *   child  (default build, links build/version.dll like victoria3.exe, so V3X boots and loads
 *           every DLL in <dir>/v3x/plugins incl. test/shutdown_probe.c): pumps frames, emits
 *           "load_transition" Empty->Game and Game->Empty like the game does, starts 4 threads that
 *           call api->log/api->print in a tight loop (the core log lock is held almost all the time),
 *           one heap-churn thread and one CRT FILE writer thread, keeps pumping frames for a while
 *           (devtools samples with V3X_PROFILE=1), then calls ExitProcess(42) from the main thread.
 *   runner (-DSHUTDOWN_RUNNER, no version.dll): starts the child N times and requires each to exit
 *           within 10 s with code 42 and the probe's detach marker "... done" (proves every
 *           plugin's DLL_PROCESS_DETACH returned instead of the OS killing a deadlocked process).
 *           Mode "con": DETACHED_PROCESS + hidden window, so the core opens its own console and the
 *           console input thread (console_plus hooks it) sits in a blocking read at exit.
 *           Mode "nocon": V3X_NO_CONSOLE=1.
 * Build + run (repo root; D = scratch dir):
 *   CC="python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude"
 *   mkdir -p $D/v3x/plugins && cp build/version.dll $D/ && for f in build/plugins/?*.dll; do cp $f $D/v3x/plugins/; done
 *   $CC -shared -o $D/v3x/plugins/shutdown_probe.dll test/shutdown_probe.c
 *   $CC -o $D/shutdown_child.exe test/shutdown_test.c -Lbuild -l:version.dll -luser32
 *   $CC -DSHUTDOWN_RUNNER -o $D/shutdown_test.exe test/shutdown_test.c
 *   $D/shutdown_test.exe $D/shutdown_child.exe 20 con && $D/shutdown_test.exe $D/shutdown_child.exe 20 nocon
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"

#ifndef SHUTDOWN_RUNNER

static const V3X_API *api;
static volatile LONG g_spins;

static DWORD WINAPI log_spammer(void *u)
{
    int id = (int)(intptr_t)u;
    for (unsigned i = 0;; i++) {
        api->log("spam", "thread %d line %u: holding the core log lock as often as possible", id, i);
        if (!(i & 7)) api->print("spam %d %u", id, i);
        InterlockedIncrement(&g_spins);
    }
    return 0;
}

static DWORD WINAPI heap_churn(void *u)
{
    (void)u;
    for (unsigned i = 0;; i++) {
        void *p = malloc(64 + (i % 4096));
        if (p) { memset(p, 1, 64); free(p); }
    }
    return 0;
}

static DWORD WINAPI crt_writer(void *path)
{
    FILE *f = fopen((const char *)path, "w");
    if (!f) return 0;
    for (unsigned i = 0;; i++) {
        fprintf(f, "crt line %u\n", i);
        if (!(i & 63)) { fflush(f); rewind(f); }
    }
    return 0;
}

static void pump(int ms)
{
    DWORD end = GetTickCount() + (DWORD)ms;
    MSG m;
    while ((LONG)(end - GetTickCount()) > 0) { PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE); Sleep(2); }
}

int main(int argc, char **argv)
{
    DWORD vh;   /* a real version.dll import, so the linker keeps it and the loader boots V3X */
    if (!GetFileVersionInfoSizeA("C:/Windows/System32/kernel32.dll", &vh)) { printf("version.dll forwarding broken\n"); return 4; }
    v3x_api_fn get = (v3x_api_fn)(void *)GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api");
    if (!get || !(api = get())) { printf("no v3x_api export\n"); return 3; }
    int seed = argc > 1 ? atoi(argv[1]) : 0;

    pump(150);                                       /* plugins' on_frame callbacks run */
    V3X_LoadTransition in = { "Empty", "Game", 0.5 }, out = { "Game", "Empty", 3.8 };
    api->emit_event("load_transition_begin", &in);
    api->emit_event("load_transition", &in);
    pump(50);

    for (int i = 0; i < 4; i++) { HANDLE t = CreateThread(NULL, 0, log_spammer, (void *)(intptr_t)i, 0, NULL); if (t) CloseHandle(t); }
    { HANDLE t = CreateThread(NULL, 0, heap_churn, NULL, 0, NULL); if (t) CloseHandle(t); }
    static char crt_path[MAX_PATH];
    snprintf(crt_path, sizeof crt_path, "%sshutdown_crt.txt", api->v3x_dir());
    { HANDLE t = CreateThread(NULL, 0, crt_writer, crt_path, 0, NULL); if (t) CloseHandle(t); }

    api->emit_event("load_transition_begin", &out);  /* the game's quit: Game->Empty, then exit */
    pump(20 + (seed * 37) % 200);
    api->emit_event("load_transition", &out);
    pump(20 + (seed * 53) % 150);
    while (g_spins < 100) Sleep(1);                  /* the spammers really run */
    ExitProcess(42);
}

#else /* ------------------------------------------------------------------ runner */

static const char EXPECT[] = "detach terminating=1 print=-1 hook=1 run=-1 emit=-1 exec=-1 done\n";

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: shutdown_test <shutdown_child.exe> [runs=20] [con|nocon]\n"); return 2; }
    const char *child = argv[1];
    int runs = argc > 2 ? atoi(argv[2]) : 20;
    int con = !(argc > 3 && !strcmp(argv[3], "nocon"));
    char dir[MAX_PATH], marker[MAX_PATH];
    if (!GetFullPathNameA(child, MAX_PATH, dir, NULL)) return 2;
    char *sl = strrchr(dir, '\\'); if (sl) sl[1] = 0;
    snprintf(marker, sizeof marker, "%sv3x\\shutdown_probe.txt", dir);
    SetEnvironmentVariableA("V3X_PROFILE", "1");                       /* devtools sampler suspends threads */
    SetEnvironmentVariableA("V3X_DEVBRIDGE_PIPE", "v3x_devbridge_shutdown_test");
    SetEnvironmentVariableA("V3X_NO_CONSOLE", con ? NULL : "1");
    int fails = 0; DWORD worst = 0;
    for (int r = 0; r < runs; r++) {
        DeleteFileA(marker);
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof cmd, "\"%s\" %d", child, r);
        STARTUPINFOA si = { .cb = sizeof si };
        if (con) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
        PROCESS_INFORMATION pi;
        DWORD t0 = GetTickCount();
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, con ? DETACHED_PROCESS : 0, NULL, dir, &si, &pi)) {
            printf("FAIL: run %d: CreateProcess error %lu\n", r, GetLastError()); fails++; continue;
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 10000), dt = GetTickCount() - t0, code = 0;
        if (w != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 99); WaitForSingleObject(pi.hProcess, 5000); }
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        char got[256] = {0};
        FILE *f = fopen(marker, "rb");
        if (f) { fread(got, 1, sizeof got - 1, f); fclose(f); }
        int ok = w == WAIT_OBJECT_0 && code == 42 && !strcmp(got, EXPECT);
        if (dt > worst) worst = dt;
        printf("%s: run %2d (%s): %s in %lu ms, exit code %lu, marker: %s", ok ? "OK  " : "FAIL", r, con ? "con" : "nocon",
               w == WAIT_OBJECT_0 ? "exited" : "HUNG (killed after 10 s)", dt, code, got[0] ? got : "(missing)\n");
        fails += !ok;
    }
    printf("%d/%d runs exited cleanly, slowest %lu ms\n%s\n", runs - fails, runs, worst, fails ? "FAILED" : "ALL OK");
    return fails;
}
#endif
