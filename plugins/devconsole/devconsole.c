/* devconsole: reverse-engineering commands for the V3X console (API v2), disassembly by Zydis.
 *
 *   dis  <addr> [count]      disassemble `count` instructions (default 20, max 500)
 *   fn   <addr> [max]        whole function containing addr (bounds from .pdata), max 400 instrs
 *   xref <addr|"string"> [max]  rip-relative lea/mov in the game's .text that reference addr
 *   scan [-n max] <ida pattern>  all matches in the game's .text (default max 20), e.g.
 *                            scan 48 8B 05 ?? ?? ?? ?? 48 85 C0
 *   mem  <addr> [len]        hex + ascii dump (default 128, max 4096 bytes)
 *   mods [filter]            loaded modules: base, size, name
 *
 * Addresses: hex absolute (0x7ff6..., backticks allowed), +RVA in the game exe (+0x3e17820), or
 * module+offset exactly as printed by these commands (victoria3.exe+0x3e17820, kernel32.dll+0x10).
 * Game memory is never dereferenced: every byte is copied with ReadProcessMemory (dc_read) after a
 * VirtualQuery pre-check, so a bad or concurrently freed address prints an error, not a crash.
 * Commands run on the console input thread and only read memory.
 * Without a console window (console=0) the commands are still registered (and still reachable via
 * api->exec_command), they just can't be typed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stddef.h>

#define ZYDIS_STATIC_BUILD
#define ZYCORE_STATIC_BUILD
#include "../../third_party/zydis/Zydis.h"
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "devconsole";

static const V3X_API *api;

/* ------------------------------------------------------------------ helpers */

/* Number of bytes readable starting at a (0..n), by walking VirtualQuery regions. */
static size_t dc_readable(uintptr_t a, size_t n)
{
    size_t ok = 0;
    while (ok < n) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(a + ok), &mbi, sizeof mbi)) break;
        DWORD p = mbi.Protect & 0xFF;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) break;
        if (p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READ &&
            p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY)
            break;
        size_t left = (uintptr_t)mbi.BaseAddress + mbi.RegionSize - (a + ok);
        ok += left;
    }
    return ok < n ? ok : n;
}

/* Fault-proof copy of up to n bytes at a into buf; returns bytes copied (a readable prefix).
 * Every byte we decode or dump goes through here: memory is never dereferenced directly, so a page
 * freed or re-protected by another thread after the VirtualQuery pre-check makes ReadProcessMemory
 * fail instead of faulting (a fault on the console thread would kill the game). */
static size_t dc_read(uintptr_t a, void *buf, size_t n)
{
    size_t avail = dc_readable(a, n);
    if (!avail) return 0;
    HANDLE self = GetCurrentProcess();
    SIZE_T got = 0;
    if (ReadProcessMemory(self, (void *)a, buf, avail, &got) && got == avail) return avail;
    size_t done = 0;   /* partial failure: keep the readable prefix, page by page */
    while (done < avail) {
        size_t chunk = 0x1000 - ((a + done) & 0xFFF);
        if (chunk > avail - done) chunk = avail - done;
        got = 0;
        if (!ReadProcessMemory(self, (void *)(a + done), (uint8_t *)buf + done, chunk, &got) || got != chunk) break;
        done += chunk;
    }
    return done;
}

/* "victoria3.exe+0x1234" for addresses inside a module, otherwise "0x...". */
static void dc_where(uintptr_t a, char *buf, size_t cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m && GetModuleFileNameA(m, path, sizeof path)) {
        const char *b = strrchr(path, '\\');
        b = b ? b + 1 : path;
        snprintf(buf, cap, "%s+0x%llx", b, (unsigned long long)(a - (uintptr_t)m));
        return;
    }
    snprintf(buf, cap, "0x%llx", (unsigned long long)a);
}

/* Parses one address token from *s (advancing *s). Returns 1 on success. */
static int dc_parse_addr(const char **s, uintptr_t *out)
{
    const char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    char tok[MAX_PATH]; size_t k = 0;
    while (*p && *p != ' ' && *p != '\t' && k < sizeof tok - 1) {
        if (*p != '`') tok[k++] = *p;
        p++;
    }
    tok[k] = 0;
    if (!k) return 0;
    uintptr_t base = 0;
    const char *num = tok;
    char *plus = strrchr(tok, '+');
    if (!strncmp(tok, "rva:", 4)) { base = api->image_base(); num = tok + 4; }
    else if (plus == tok) { base = api->image_base(); num = tok + 1; }
    else if (plus) {
        *plus = 0;
        HMODULE m = GetModuleHandleA(tok);
        if (!m) return 0;
        base = (uintptr_t)m; num = plus + 1;
    }
    if (!*num) return 0;
    char *end;
    unsigned long long v = strtoull(num, &end, 16);
    if (*end) return 0;
    *out = base + (uintptr_t)v;
    *s = p;
    return 1;
}

/* Optional decimal (or 0x hex) count after the address; `def` if absent, clamped to [1, max]. */
static long dc_parse_count(const char *s, long def, long max)
{
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return def;
    long v = strtol(s, NULL, 0);
    if (v < 1) v = def;
    return v > max ? max : v;
}

/* Appends `"text"` if a points at a printable NUL-terminated ASCII string of 4+ chars. */
static void dc_string_hint(uintptr_t a, char *out, size_t cap)
{
    char s[64];
    size_t n = dc_read(a, s, sizeof s);
    size_t i = 0;
    while (i < n && ((s[i] >= 0x20 && s[i] < 0x7F) || s[i] == '\t')) i++;
    if (i >= 4 && (i == n ? n == 64 : s[i] == 0)) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, " \"%.*s%s\"", (int)(i > 60 ? 60 : i), s, i > 60 ? "..." : "");
    }
}

/* Formats one instruction at a into line. Returns its length, or 0 if a is not readable. */
static size_t dc_dis_one(uintptr_t a, char *line, size_t cap)
{
    char where[MAX_PATH];
    dc_where(a, where, sizeof where);
    uint8_t b[ZYDIS_MAX_INSTRUCTION_LENGTH];
    size_t avail = dc_read(a, b, sizeof b);
    if (!avail) { snprintf(line, cap, "%-28s <not readable>", where); return 0; }
    ZydisDisassembledInstruction ins;   /* decodes the local copy, runtime address = a */
    if (!ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, a, b, avail, &ins))) {
        snprintf(line, cap, "%-28s %02X%-28s db 0x%02X  ; invalid", where, b[0], "", b[0]);
        return 1;
    }
    size_t len = ins.info.length;
    char hex[64] = ""; size_t h = 0;
    for (size_t i = 0; i < len && i < 10; i++) h += snprintf(hex + h, sizeof hex - h, "%02X ", b[i]);
    if (len > 10) snprintf(hex + h, sizeof hex - h, "..");
    snprintf(line, cap, "%-28s %-30s %s", where, hex, ins.text);
    for (ZyanU8 i = 0; i < ins.info.operand_count_visible; i++) {
        const ZydisDecodedOperand *op = &ins.operands[i];
        int rel = (op->type == ZYDIS_OPERAND_TYPE_MEMORY && op->mem.base == ZYDIS_REGISTER_RIP) ||
                  (op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op->imm.is_relative);
        ZyanU64 abs;
        if (!rel || !ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins.info, op, a, &abs))) continue;
        char tw[MAX_PATH];
        dc_where((uintptr_t)abs, tw, sizeof tw);
        size_t l = strlen(line);
        snprintf(line + l, cap - l, "  ; %s", tw);
        if (op->type == ZYDIS_OPERAND_TYPE_MEMORY) dc_string_hint((uintptr_t)abs, line, cap);
        break;
    }
    return len;
}

/* ------------------------------------------------------------------ commands */

static void cmd_dis(const char *args, void *user)
{
    (void)user;
    uintptr_t a;
    if (!dc_parse_addr(&args, &a)) { api->print("usage: dis <addr> [count]   (addr: 0x..., +rva, module+off)"); return; }
    long n = dc_parse_count(args, 20, 500);
    char line[512];
    for (long i = 0; i < n; i++) {
        size_t len = dc_dis_one(a, line, sizeof line);
        api->print("%s", line);
        if (!len) break;
        a += len;
    }
}

static void cmd_fn(const char *args, void *user)
{
    (void)user;
    uintptr_t a;
    if (!dc_parse_addr(&args, &a)) { api->print("usage: fn <addr> [max instructions]"); return; }
    long max = dc_parse_count(args, 400, 5000);
    char where[MAX_PATH], line[512];
    dc_where(a, where, sizeof where);
    uintptr_t start = (uintptr_t)api->function_start((void *)a);
    DWORD64 img = 0;
    PRUNTIME_FUNCTION rf = start ? RtlLookupFunctionEntry((DWORD64)start, &img, NULL) : NULL;
    if (!rf) { api->print("%s: no .pdata entry (not inside a function with unwind info)", where); return; }
    uintptr_t end = (uintptr_t)img + rf->EndAddress;
    char sw[MAX_PATH];
    dc_where(start, sw, sizeof sw);
    api->print("function %s  size 0x%llx  (contains %s)", sw, (unsigned long long)(end - start), where);
    long i = 0;
    for (uintptr_t p = start; p < end && i < max; i++) {
        size_t len = dc_dis_one(p, line, sizeof line);
        api->print("%s", line);
        if (!len) return;
        p += len;
        if (p >= end) { api->print("end of function (%ld instructions)", i + 1); return; }
    }
    api->print("... truncated at %ld instructions (fn <addr> <max> for more)", max);
}

static void cmd_xref(const char *args, void *user)
{
    (void)user;
    uintptr_t target;
    while (*args == ' ') args++;
    if (*args == '"') {
        const char *q = strchr(args + 1, '"');
        if (!q || q == args + 1) { api->print("usage: xref \"exact string\" [max]"); return; }
        char s[1024];
        snprintf(s, sizeof s, "%.*s", (int)(q - args - 1), args + 1);
        void *p = api->find_string(s);
        if (!p) { api->print("string \"%s\" not found in the game's data sections", s); return; }
        char w[MAX_PATH];
        dc_where((uintptr_t)p, w, sizeof w);
        api->print("string \"%s\" at %s", s, w);
        target = (uintptr_t)p;
        args = q + 1;
    } else if (!dc_parse_addr(&args, &target)) {
        api->print("usage: xref <addr | \"string\"> [max]   (rip-relative lea/mov in .text)");
        return;
    }
    long max = dc_parse_count(args, 32, 1000);
    char line[512], fw[MAX_PATH];
    long n = 0;
    for (; n < max; n++) {
        void *site = api->find_xref((void *)target, (int)n);
        if (!site) break;
        void *fn = api->function_start(site);
        if (fn) dc_where((uintptr_t)fn, fw, sizeof fw); else snprintf(fw, sizeof fw, "?");
        dc_dis_one((uintptr_t)site, line, sizeof line);
        api->print("  [%ld] %s   (fn %s)", n, line, fw);
    }
    api->print("%ld xref(s)%s", n, n == max ? " (stopped at max)" : "");
}

/* IDA pattern -> bytes/mask. Tokens: hex bytes ("48", or runs like "488B05??77"), "?" / "??" = any byte.
 * Returns the byte count, 0 on a malformed pattern or one with no fixed byte. */
#define DC_MAX_PAT 256
static int dc_parse_pattern(const char *s, uint8_t *bytes, uint8_t *mask)
{
    int n = 0, fixed = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        const char *t = s;
        while (*t && *t != ' ' && *t != '\t') t++;
        if (t - s == 1 && *s == '?') {             /* lone "?" token */
            if (n >= DC_MAX_PAT) return 0;
            bytes[n] = 0; mask[n++] = 0;
            s = t;
            continue;
        }
        if ((t - s) % 2) return 0;
        for (; s < t; s += 2) {                    /* pairs: "48" or "??", also inside runs like 8B05??77 */
            if (n >= DC_MAX_PAT) return 0;
            if (s[0] == '?' && s[1] == '?') { bytes[n] = 0; mask[n++] = 0; continue; }
            if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return 0;
            char hx[3] = {s[0], s[1], 0};
            bytes[n] = (uint8_t)strtoul(hx, NULL, 16); mask[n++] = 1; fixed++;
        }
    }
    return fixed ? n : 0;
}

/* The game exe's .text section (from its PE headers, read via dc_read). */
static int dc_game_text(uintptr_t *start, size_t *size)
{
    uintptr_t base = api->image_base();
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (dc_read(base, &dos, sizeof dos) != sizeof dos || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (dc_read(base + dos.e_lfanew, &nt, sizeof nt) != sizeof nt || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    uintptr_t sec = base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (int i = 0; i < nt.FileHeader.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh;
        if (dc_read(sec + i * sizeof sh, &sh, sizeof sh) != sizeof sh) return 0;
        if (!memcmp(sh.Name, ".text", 6)) {
            *start = base + sh.VirtualAddress;
            *size = sh.Misc.VirtualSize ? sh.Misc.VirtualSize : sh.SizeOfRawData;
            return 1;
        }
    }
    return 0;
}

static void cmd_scan(const char *args, void *user)
{
    (void)user;
    static const char usage[] = "usage: scan [-n max] <ida pattern>   e.g. scan 48 8B 05 ?? ?? ?? ?? 48 85 C0";
    while (*args == ' ' || *args == '\t') args++;
    long max = 20;
    if (args[0] == '-' && args[1] == 'n' && (args[2] == ' ' || args[2] == '\t')) {
        char *end;
        max = strtol(args + 3, &end, 0);
        if (end == args + 3 || max < 1) { api->print("%s", usage); return; }
        if (max > 1000) max = 1000;
        args = end;
    }
    uint8_t pb[DC_MAX_PAT], pm[DC_MAX_PAT];
    int n = dc_parse_pattern(args, pb, pm);
    if (!n) { api->print("%s", usage); return; }
    uintptr_t text; size_t tsize;
    if (!dc_game_text(&text, &tsize)) { api->print("game exe has no readable .text section"); return; }

    /* Scan a local copy in 1 MB chunks overlapping by n-1 bytes (never dereferences game memory). */
    enum { CHUNK = 1 << 20 };
    static uint8_t buf[CHUNK + DC_MAX_PAT];   /* commands run one at a time on the console thread */
    long hits = 0;
    int first_fixed = 0;
    while (!pm[first_fixed]) first_fixed++;
    char line[512], fw[MAX_PATH];
    for (size_t off = 0; off < tsize && hits < max;) {
        size_t want = tsize - off < CHUNK + (size_t)n - 1 ? tsize - off : CHUNK + (size_t)n - 1;
        size_t got = dc_read(text + off, buf, want);
        if (got < (size_t)n) { if (!got) { off += 0x1000; continue; } break; }
        for (size_t i = 0; i + n <= got && hits < max; i++) {
            if (buf[i + first_fixed] != pb[first_fixed]) continue;
            int k = 0;
            while (k < n && (!pm[k] || buf[i + k] == pb[k])) k++;
            if (k < n) continue;
            uintptr_t hit = text + off + i;
            void *fn = api->function_start((void *)hit);
            if (fn) dc_where((uintptr_t)fn, fw, sizeof fw); else snprintf(fw, sizeof fw, "?");
            dc_dis_one(hit, line, sizeof line);
            api->print("  [%ld] %s   (fn %s)", hits, line, fw);
            hits++;
        }
        if (got < want) break;           /* unreadable tail of .text */
        off += got - (n - 1);
    }
    api->print("%ld match(es) in .text%s", hits, hits == max ? " (stopped at max)" : "");
}

static void cmd_mem(const char *args, void *user)
{
    (void)user;
    uintptr_t a;
    if (!dc_parse_addr(&args, &a)) { api->print("usage: mem <addr> [len]"); return; }
    size_t len = (size_t)dc_parse_count(args, 128, 4096);
    uint8_t b[4096];
    size_t avail = dc_read(a, b, len);
    char where[MAX_PATH];
    if (!avail) { dc_where(a, where, sizeof where); api->print("%s: not readable", where); return; }
    for (size_t off = 0; off < avail; off += 16) {
        char line[160]; size_t l = 0;
        dc_where(a + off, where, sizeof where);
        l += snprintf(line + l, sizeof line - l, "%-28s ", where);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < avail) l += snprintf(line + l, sizeof line - l, "%02X ", b[off + i]);
            else l += snprintf(line + l, sizeof line - l, "   ");
        }
        l += snprintf(line + l, sizeof line - l, " |");
        for (size_t i = 0; i < 16 && off + i < avail; i++) {
            uint8_t c = b[off + i];
            line[l++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        line[l++] = '|'; line[l] = 0;
        api->print("%s", line);
    }
    if (avail < len) api->print("(only %zu of %zu bytes readable)", avail, len);
}

static void cmd_mods(const char *args, void *user)
{
    (void)user;
    while (*args == ' ') args++;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { api->print("module snapshot failed (%lu)", GetLastError()); return; }
    MODULEENTRY32 me; me.dwSize = sizeof me;
    int n = 0;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        if (*args) {
            char lo[MAX_MODULE_NAME32 + 1], f[256];
            size_t i;
            for (i = 0; me.szModule[i] && i < sizeof lo - 1; i++) lo[i] = (char)tolower((unsigned char)me.szModule[i]);
            lo[i] = 0;
            for (i = 0; args[i] && i < sizeof f - 1; i++) f[i] = (char)tolower((unsigned char)args[i]);
            f[i] = 0;
            if (!strstr(lo, f)) continue;
        }
        api->print("  0x%016llx  0x%08lx  %s%s", (unsigned long long)(uintptr_t)me.modBaseAddr, me.modBaseSize,
                   me.szModule, (uintptr_t)me.modBaseAddr == api->image_base() ? "  (game)" : "");
        n++;
    }
    CloseHandle(snap);
    api->print("%d module(s)", n);
}

/* ------------------------------------------------------------------ load */

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (a->api_version < 2 || a->struct_size < offsetof(V3X_API, exec_command) + sizeof(void *)) {
        a->log("devconsole", "loader API v%u is too old (need v2 console commands), not loading", a->api_version);
        return 1;
    }
    static const struct { const char *name, *help; v3x_command_fn fn; } cmds[] = {
        {"dis", "<addr> [count]  disassemble (addr: 0x..., +rva, module+off)", cmd_dis},
        {"fn", "<addr> [max]  disassemble the whole function containing addr (.pdata)", cmd_fn},
        {"xref", "<addr|\"string\"> [max]  rip-relative lea/mov xrefs in the game's .text", cmd_xref},
        {"scan", "[-n max] <ida pattern>  all matches in the game's .text (default 20)", cmd_scan},
        {"mem", "<addr> [len]  hex dump (max 4096)", cmd_mem},
        {"mods", "[filter]  loaded modules with base and size", cmd_mods},
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        int rc = api->register_command(cmds[i].name, cmds[i].help, cmds[i].fn, NULL);
        if (rc == 0) ok++;
        else api->log("devconsole", "WARNING: command '%s' not registered (%d)", cmds[i].name, rc);
    }
    api->log("devconsole", "%d commands registered (Zydis %u.%u.%u)", ok, ZYDIS_VERSION_MAJOR(ZYDIS_VERSION),
             ZYDIS_VERSION_MINOR(ZYDIS_VERSION), ZYDIS_VERSION_PATCH(ZYDIS_VERSION));
    return 0;
}
