/* End-to-end test for V3X without the game: imports version.dll like victoria3.exe does,
 * and contains a fake "console blocked" predicate the ironman_console plugin must find
 * (via string xref + .pdata) and override. Exit code 0 = all checks passed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((aligned(16))) static const char IRON_MSG[] = "No Console commands in ironman!";
typedef struct { union { char buf[16]; char *ptr; } u; size_t size; size_t cap; } MsvcString;
typedef struct { void *vt; MsvcString reason; } Ctx;

__attribute__((noinline)) _Bool console_blocked(void **ctx)
{
    const char *volatile m = IRON_MSG;          /* forces a rip-relative lea to IRON_MSG */
    Ctx *c = (Ctx *)*ctx;
    size_t n = strlen(m);
    c->reason.u.ptr = malloc(n + 1);
    memcpy(c->reason.u.ptr, m, n + 1);
    c->reason.size = n; c->reason.cap = n;
    return 1;
}

int main(void)
{
    int fails = 0;
    DWORD h, sz = GetFileVersionInfoSizeA("C:/Windows/System32/kernel32.dll", &h);
    printf("forwarded GetFileVersionInfoSizeA -> %lu %s\n", sz, sz ? "OK" : "FAIL"); fails += !sz;

    Ctx c = {0}; void *pc = &c;
    _Bool (*volatile f)(void **) = console_blocked;
    _Bool b = f(&pc);
    printf("console_blocked() -> %d %s\n", b, b == 0 ? "OK (plugin hooked it)" : "FAIL"); fails += b != 0;

    MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);  /* drives on_frame */
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
