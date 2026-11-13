/* v3x_mem.h - fault-proof memory reads for V3X plugins (header-only, C, no CRT needed).
 *
 * Game memory changes under you: objects are freed by other threads, pages are decommitted by
 * mimalloc, a pointer chain you reverse-engineered on one build is garbage on the next. A plain
 * dereference that faults on the console thread or an engine worker kills the game. These
 * helpers never dereference: they check the range with VirtualQuery and copy it with
 * ReadProcessMemory on the own process, which fails (returns FALSE) instead of faulting, even if
 * the page disappears between the check and the copy.
 *
 *   uint64_t p;
 *   if (v3x_mem_read_u64(obj + 0x120, &p) && v3x_mem_readable(p, 0x40)) ...
 *   char name[64]; v3x_mem_read_cstr(str_ptr, name, sizeof name);
 *
 * Cost: one VirtualQuery per region plus a syscall per read (about 1 us). Do not use it in a hot
 * path that runs millions of times; resolve and validate pointers once, then use them directly
 * where you can prove they live long enough (e.g. .data globals of the exe). */
#ifndef V3X_MEM_H
#define V3X_MEM_H

#include <stddef.h>
#include <stdint.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* 1 if protection p (low byte of MEMORY_BASIC_INFORMATION.Protect) allows reading */
static int v3x_mem__prot_readable(DWORD prot)
{
    DWORD p = prot & 0xFF;
    if (prot & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

/* Number of readable bytes starting at addr, up to n (0 = the first byte is not readable). */
static size_t v3x_mem_readable_prefix(uintptr_t addr, size_t n)
{
    size_t ok = 0;
    if (addr < 0x10000) return 0;                     /* NULL page and small integers */
    while (ok < n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((const void *)(addr + ok), &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_COMMIT || !v3x_mem__prot_readable(mbi.Protect)) break;
        ok += (uintptr_t)mbi.BaseAddress + mbi.RegionSize - (addr + ok);
    }
    return ok < n ? ok : n;
}

/* 1 if all n bytes at addr are committed and readable right now. */
static int v3x_mem_readable(uintptr_t addr, size_t n) { return n && v3x_mem_readable_prefix(addr, n) == n; }

/* 1 if addr is committed executable memory (a plausible code pointer / vtable slot target). */
static int v3x_mem_is_code(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (addr < 0x10000 || !VirtualQuery((const void *)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    DWORD p = mbi.Protect & 0xFF;
    return !(mbi.Protect & PAGE_GUARD) &&
           (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY);
}

/* Copies exactly n bytes from addr into out. Returns 1 on success, 0 if any byte is unreadable
 * (out is then unspecified). Never faults. */
static int v3x_mem_read(uintptr_t addr, void *out, size_t n)
{
    SIZE_T got = 0;
    if (!n) return 1;
    if (!out || !v3x_mem_readable(addr, n)) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)addr, out, n, &got) && got == n;
}

static int v3x_mem_read_u64(uintptr_t addr, uint64_t *out) { return v3x_mem_read(addr, out, 8); }
static int v3x_mem_read_u32(uintptr_t addr, uint32_t *out) { return v3x_mem_read(addr, out, 4); }
static int v3x_mem_read_u8(uintptr_t addr, uint8_t *out) { return v3x_mem_read(addr, out, 1); }
static int v3x_mem_read_ptr(uintptr_t addr, uintptr_t *out) { return v3x_mem_read(addr, out, sizeof *out); }

/* Follows a pointer chain: p = base; for each offset: p = *(p + off). Returns the final value in
 * *out (1) or 0 if any step is unreadable or NULL. Example: [[gs+0x120]+0x1c00] =
 * v3x_mem_chain(gs, (const uint32_t[]){0x120, 0x1c00}, 2, &mgr). */
static int v3x_mem_chain(uintptr_t base, const uint32_t *offsets, int n, uintptr_t *out)
{
    uintptr_t p = base;
    for (int i = 0; i < n; i++) {
        if (!p || !v3x_mem_read_ptr(p + offsets[i], &p)) return 0;
    }
    if (!p) return 0;
    *out = p;
    return 1;
}

/* Copies a NUL-terminated string of at most cap-1 chars into out (always terminated; cap >= 1).
 * Returns the length copied, or -1 if the first byte is unreadable. Stops at the end of the
 * readable range, so a string running into an unmapped page is cut, not a crash. */
static int v3x_mem_read_cstr(uintptr_t addr, char *out, size_t cap)
{
    if (!out || !cap) return -1;
    out[0] = 0;
    size_t avail = v3x_mem_readable_prefix(addr, cap - 1);
    if (!avail) return -1;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (const void *)addr, out, avail, &got)) got = 0;
    size_t i = 0;
    while (i < got && out[i]) i++;
    out[i] = 0;
    return got ? (int)i : -1;
}

/* 1 if [addr, addr+n) lies inside [module_base, module_base+module_size), e.g.
 * v3x_mem_in_module(api->image_base(), api->image_size(), p, 8). */
static int v3x_mem_in_module(uintptr_t module_base, size_t module_size, uintptr_t addr, size_t n)
{
    return addr >= module_base && n <= module_size && addr - module_base <= module_size - n;
}

#endif
