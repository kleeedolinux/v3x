/* Offline test of cpu_sched's core logic (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -o D:/v3x_tmp/threads/cst.exe test/cpu_sched_test.c
 *   D:/v3x_tmp/threads/cst.exe ["<game>/binaries/victoria3.exe" | -] [path/to/cpu_sched.dll]   -> prints ALL OK
 * With the dll path it loads the built plugin against a fake V3X_API + fake image (anchor strings and
 * signature bytes) and calls the two detours it hooked: checks the TaskThreadCount rewrite and, on a
 * hybrid machine, the CPU sets each thread ends up with.
 * Tests P/E classification on fake GetSystemCpuSetInformation buffers, policy parsing, the code-walk
 * helpers on fake code, and the classification of this machine's real CPU sets (printed, not asserted).
 * With the exe path it maps the real exe (DONT_RESOLVE_DLL_REFERENCES: no code runs) and resolves
 * both hook targets with the plugin's byte signatures. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../include/v3x.h"
#include "../plugins/cpu_sched/cs_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---------------- fake CPU-set buffers */
static unsigned char buf[CS_MAX_CPUS * 48];
static size_t blen;
static void add_cpu(uint32_t size, uint32_t type, uint32_t id, uint16_t group, uint8_t lp, uint8_t core, uint8_t eff)
{
    unsigned char *e = buf + blen;
    memset(e, 0, size);
    memcpy(e, &size, 4); memcpy(e + 4, &type, 4); memcpy(e + 8, &id, 4); memcpy(e + 12, &group, 2);
    e[14] = lp; e[15] = core; e[18] = eff;
    blen += size;
}

static void test_classify(void)
{
    static cs_map m;
    /* Intel 12900K-like: 8 P-cores with HT (eff 1, 16 logical), 8 E-cores (eff 0). Ids start at 256. */
    blen = 0;
    for (int i = 0; i < 16; i++) add_cpu(32, 0, 256 + i, 0, (uint8_t)i, (uint8_t)(i / 2), 1);
    for (int i = 0; i < 8; i++) add_cpu(32, 0, 272 + i, 0, (uint8_t)(16 + i), (uint8_t)(8 + i), 0);
    CHECK(cs_classify(buf, blen, &m) == 24);
    CHECK(m.hybrid == 1 && m.eff_min == 0 && m.eff_max == 1);
    CHECK(m.np == 16 && m.ne == 8 && m.p_phys == 8 && m.e_phys == 8);
    CHECK(m.pid[0] == 256 && m.pid[15] == 271 && m.eid[0] == 272 && m.eid[7] == 279);
    const uint32_t *ids;
    CHECK(cs_policy_ids(&m, CS_PCORES, &ids) == 16 && ids == m.pid);
    CHECK(cs_policy_ids(&m, CS_ECORES, &ids) == 8 && ids == m.eid);
    CHECK(cs_policy_ids(&m, CS_ALL, &ids) == 0 && ids == NULL);

    /* Non-hybrid (all eff 0): nothing to restrict for any policy */
    blen = 0;
    for (int i = 0; i < 12; i++) add_cpu(32, 0, 256 + i, 0, (uint8_t)i, (uint8_t)(i / 2), 0);
    CHECK(cs_classify(buf, blen, &m) == 12 && !m.hybrid && m.np == 12 && m.ne == 0 && m.p_phys == 6);
    CHECK(cs_policy_ids(&m, CS_PCORES, &ids) == 0 && cs_policy_ids(&m, CS_ECORES, &ids) == 0);

    /* Three classes (e.g. LP E-cores eff 0, E eff 1, P eff 2): P = max class only, E = the rest;
     * interleaved order, two processor groups with the same core index stay distinct physical cores. */
    blen = 0;
    add_cpu(32, 0, 1, 0, 0, 0, 2); add_cpu(32, 0, 2, 0, 1, 1, 1); add_cpu(32, 0, 3, 0, 2, 2, 0);
    add_cpu(32, 0, 4, 1, 0, 0, 2); add_cpu(32, 0, 5, 1, 1, 0, 2);
    CHECK(cs_classify(buf, blen, &m) == 5 && m.hybrid && m.eff_max == 2);
    CHECK(m.np == 3 && m.pid[0] == 1 && m.pid[1] == 4 && m.pid[2] == 5 && m.p_phys == 2);
    CHECK(m.ne == 2 && m.eid[0] == 2 && m.eid[1] == 3 && m.e_phys == 2);

    /* Variable entry sizes and unknown entry types are skipped by Size. */
    blen = 0;
    add_cpu(40, 0, 10, 0, 0, 0, 1); add_cpu(24, 7, 99, 0, 0, 0, 5); add_cpu(32, 0, 11, 0, 1, 1, 0);
    CHECK(cs_classify(buf, blen, &m) == 2 && m.np == 1 && m.pid[0] == 10 && m.ne == 1 && m.eid[0] == 11);

    /* Malformed: zero size, overrun, empty buffer */
    blen = 0; add_cpu(32, 0, 1, 0, 0, 0, 1);
    uint32_t z = 0; memcpy(buf + blen, &z, 4); memset(buf + blen + 4, 0, 28); blen += 32;
    CHECK(cs_classify(buf, blen, &m) == 1);
    blen = 0; add_cpu(32, 0, 1, 0, 0, 0, 1); add_cpu(32, 0, 2, 0, 1, 1, 0);
    CHECK(cs_classify(buf, blen - 1, &m) == 1);   /* truncated second entry is dropped */
    CHECK(cs_classify(buf, 0, &m) == 0 && m.n == 0 && !m.hybrid);

    /* Hybrid but an empty class can't happen (max != min means both exist); single P-core list stays usable */
    blen = 0; add_cpu(32, 0, 7, 0, 0, 0, 1); add_cpu(32, 0, 8, 0, 1, 1, 0);
    CHECK(cs_classify(buf, blen, &m) == 2 && cs_policy_ids(&m, CS_PCORES, &ids) == 1 && ids[0] == 7);

    /* Cap at CS_MAX_CPUS entries */
    static unsigned char big[(CS_MAX_CPUS + 8) * 32];
    for (int i = 0; i < CS_MAX_CPUS + 8; i++) {
        unsigned char *e = big + i * 32; uint32_t sz = 32, ty = 0, id = (uint32_t)i;
        memset(e, 0, 32); memcpy(e, &sz, 4); memcpy(e + 4, &ty, 4); memcpy(e + 8, &id, 4);
        e[14] = (uint8_t)i; e[15] = (uint8_t)i; e[12] = (uint8_t)(i >> 8); e[18] = (uint8_t)(i & 1);
    }
    CHECK(cs_classify(big, sizeof big, &m) == CS_MAX_CPUS && m.np + m.ne == CS_MAX_CPUS);
}

static void test_policy(void)
{
    CHECK(cs_parse_policy("pcores", -1) == CS_PCORES);
    CHECK(cs_parse_policy(" ECores \r\n", -1) == CS_ECORES);
    CHECK(cs_parse_policy("ALL", -1) == CS_ALL);
    CHECK(cs_parse_policy("pcore", -1) == -1);
    CHECK(cs_parse_policy("pcoresx", -1) == -1);
    CHECK(cs_parse_policy("", 2) == 2);
    CHECK(cs_parse_policy(NULL, 1) == 1);
}

/* ---------------- signatures and code walk on fake code */
static unsigned char code[0x2000];
static const unsigned char *peek(void *ctx, uint64_t va, size_t n)
{
    (void)ctx;
    return va >= (uint64_t)code && va + n <= (uint64_t)code + sizeof code ? (const unsigned char *)va : NULL;
}
static void put_rel(unsigned char *at, const unsigned char *target, int len)
{ int32_t r = (int32_t)(target - (at + len)); memcpy(at + len - 4, &r, 4); }

static void test_sigs(void)
{
    CHECK(sizeof CS_SIG_INIT == strlen(CS_MASK_INIT));
    CHECK(sizeof CS_SIG_ADDWORKER == strlen(CS_MASK_ADDWORKER));
    CHECK(sizeof CS_SIG_WORKER == strlen(CS_MASK_WORKER));
    unsigned char t[sizeof CS_SIG_INIT]; memcpy(t, CS_SIG_INIT, sizeof t);
    t[27] = 0x12; t[30] = 0x99;                      /* wildcard bytes (call rel32) */
    CHECK(cs_sig_match(t, CS_SIG_INIT, CS_MASK_INIT));
    t[31] = 0xbe;                                     /* mov esi,4 instead of mov edi,4 */
    CHECK(!cs_sig_match(t, CS_SIG_INIT, CS_MASK_INIT));

    memset(code, 0xcc, sizeof code);
    unsigned char *add = code + 0x800, *worker = code + 0x1000, *decoy = code + 0x1400;
    memcpy(add, CS_SIG_ADDWORKER, sizeof CS_SIG_ADDWORKER);
    memcpy(worker, CS_SIG_WORKER, sizeof CS_SIG_WORKER); worker[0x1e] = 0x70; worker[0x1f] = 0x61;
    memcpy(decoy, CS_SIG_WORKER, 20);                 /* prologue only: must not match */
    /* SpawnWorkers: a call to an unrelated fn, a stray E8 byte, then the call to AddWorker */
    unsigned char *x = code + 0x100;
    x[0x10] = 0xe8; put_rel(x + 0x10, decoy, 5);
    x[0x20] = 0xe8; x[0x21] = 0xff; x[0x22] = 0xff; x[0x23] = 0xff; x[0x24] = 0x7f;   /* target outside image */
    x[0x40] = 0xe8; put_rel(x + 0x40, add, 5);
    CHECK(cs_find_call_to(x, 0x100, (uint64_t)x, CS_SIG_ADDWORKER, CS_MASK_ADDWORKER, peek, NULL) == (uint64_t)add);
    CHECK(cs_find_call_to(x, 0x40, (uint64_t)x, CS_SIG_ADDWORKER, CS_MASK_ADDWORKER, peek, NULL) == 0);
    /* AddWorker body: lea rcx,decoy; call; then lea rcx,worker; call */
    unsigned char *b = add + 0x100;
    b[0] = 0x48; b[1] = 0x8d; b[2] = 0x0d; put_rel(b, decoy, 7); b[7] = 0xe8;
    b[0x40] = 0x48; b[0x41] = 0x8d; b[0x42] = 0x0d; put_rel(b + 0x40, worker, 7); b[0x47] = 0xe8;
    b[0x80] = 0x48; b[0x81] = 0x8d; b[0x82] = 0x0d; put_rel(b + 0x80, worker, 7); b[0x87] = 0x90;  /* lea not followed by call */
    CHECK(cs_find_lea_rcx_call(add, 0x400, (uint64_t)add, CS_SIG_WORKER, CS_MASK_WORKER, peek, NULL) == (uint64_t)worker);
    b[0x47] = 0x90;
    CHECK(cs_find_lea_rcx_call(add, 0x400, (uint64_t)add, CS_SIG_WORKER, CS_MASK_WORKER, peek, NULL) == 0);
}

/* ---------------- this machine (informational) */
typedef BOOL (WINAPI *gscsi_fn)(void *, ULONG, PULONG, HANDLE, ULONG);
static void test_this_machine(void)
{
    gscsi_fn get = (gscsi_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetSystemCpuSetInformation");
    if (!get) { printf("this machine: no CPU set API\n"); return; }
    ULONG len = 0; get(NULL, 0, &len, GetCurrentProcess(), 0);
    unsigned char *b = (unsigned char *)malloc(len ? len : 1);
    static cs_map m;
    CHECK(len && get(b, len, &len, GetCurrentProcess(), 0) && cs_classify(b, len, &m) > 0);
    SYSTEM_INFO si; GetSystemInfo(&si);
    CHECK(m.n == (int)si.dwNumberOfProcessors || si.dwNumberOfProcessors == 64);   /* one group: equal */
    printf("this machine: %d CPU sets, EfficiencyClass %d..%d, %s, P %d logical/%d phys, E %d logical/%d phys\n",
           m.n, m.eff_min, m.eff_max, m.hybrid ? "hybrid" : "not hybrid", m.np, m.p_phys, m.ne, m.e_phys);
    free(b);
}

/* ---------------- real exe */
static unsigned char *g_img; static size_t g_isz;
static const unsigned char *img_peek(void *ctx, uint64_t va, size_t n)
{ (void)ctx; return va >= (uint64_t)g_img && va + n <= (uint64_t)g_img + g_isz ? (const unsigned char *)va : NULL; }
static IMAGE_SECTION_HEADER *sect(const char *name)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_img + ((IMAGE_DOS_HEADER *)g_img)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) if (!strncmp((char *)s->Name, name, 8)) return s;
    return NULL;
}
static unsigned char *find_str(const char *str)
{
    IMAGE_SECTION_HEADER *r = sect(".rdata"); size_t n = strlen(str) + 1;
    unsigned char *p = g_img + r->VirtualAddress, *e = p + r->Misc.VirtualSize - n;
    for (; p < e; p++) if (!memcmp(p, str, n) && !p[-1]) return p;
    return NULL;
}
static unsigned char *find_xref(const unsigned char *target, int *count)
{
    IMAGE_SECTION_HEADER *t = sect(".text"); unsigned char *p = g_img + t->VirtualAddress, *e = p + t->Misc.VirtualSize - 7, *first = NULL;
    *count = 0;
    for (; p < e; p++) {
        if ((p[0] != 0x48 && p[0] != 0x4c) || (p[1] != 0x8d && p[1] != 0x8b) || (p[2] & 0xc7) != 0x05) continue;
        int32_t r; memcpy(&r, p + 3, 4);
        if (p + 7 + r == target) { if (!first) first = p; ++*count; }
    }
    return first;
}
static unsigned char *fn_start(const unsigned char *x)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_img + ((IMAGE_DOS_HEADER *)g_img)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *f = (RUNTIME_FUNCTION *)(g_img + d.VirtualAddress);
    DWORD rv = (DWORD)(x - g_img);
    for (size_t i = 0; i < d.Size / sizeof *f; i++)
        if (f[i].BeginAddress <= rv && rv < f[i].EndAddress) {
            DWORD b = f[i].BeginAddress; unsigned char *u = g_img + f[i].UnwindData;
            for (int k = 0; k < 16 && ((u[0] >> 3) & 4); k++) {          /* UNW_FLAG_CHAININFO: go to the parent */
                RUNTIME_FUNCTION *c = (RUNTIME_FUNCTION *)(u + 4 + ((u[2] + 1) & ~1) * 2);
                b = c->BeginAddress; u = g_img + c->UnwindData;
            }
            return g_img + b;
        }
    return NULL;
}

static void test_exe(const char *exe)
{
    HMODULE h = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(h != NULL); if (!h) return;
    g_img = (unsigned char *)h;
    g_isz = ((IMAGE_NT_HEADERS *)(g_img + ((IMAGE_DOS_HEADER *)g_img)->e_lfanew))->OptionalHeader.SizeOfImage;
    int n;
    unsigned char *s = find_str("Spawning {} normal worker threads"), *x = s ? find_xref(s, &n) : NULL, *f = x ? fn_start(x) : NULL;
    CHECK(f && n == 1 && cs_sig_match(f, CS_SIG_INIT, CS_MASK_INIT));
    printf("exe: scheduler init  rva 0x%llx\n", f ? (unsigned long long)(f - g_img) : 0ULL);
    s = find_str("{}_{} {}"); x = s ? find_xref(s, &n) : NULL;
    CHECK(x && n == 1);
    uint64_t add = x ? cs_find_call_to(x, 0x100, (uint64_t)x, CS_SIG_ADDWORKER, CS_MASK_ADDWORKER, img_peek, NULL) : 0;
    uint64_t w = add ? cs_find_lea_rcx_call((unsigned char *)add, 0x400, add, CS_SIG_WORKER, CS_MASK_WORKER, img_peek, NULL) : 0;
    CHECK(add && w);
    printf("exe: AddWorker       rva 0x%llx\nexe: worker entry    rva 0x%llx\n",
           add ? (unsigned long long)(add - (uint64_t)g_img) : 0ULL, w ? (unsigned long long)(w - (uint64_t)g_img) : 0ULL);
    printf("exe: %s the 1.15.1 RVAs in docs/re/threading.md (0x3df8a50 / 0x3df9180)\n",
           f && f - g_img == 0x3df8a50 && w - (uint64_t)g_img == 0x3df9180 ? "matches" : "DIFFERS from");
}

/* ---------------- plugin end-to-end with a fake API */
static unsigned char fimg[0x3000];
static char fdir[MAX_PATH];
static void *f_hooks[2][2];          /* target, detour */
static int f_nhooks, f_init_seen = 0x12345, f_cmds;
static v3x_command_fn f_cmd_fn;
#define F_INIT_STR  (fimg + 0x10)
#define F_SPAWN_STR (fimg + 0x40)
#define F_INIT_FN   (fimg + 0x400)
#define F_INIT_X    (fimg + 0x480)
#define F_SPAWN_X   (fimg + 0x800)
#define F_ADD       (fimg + 0x1000)
#define F_WORKER    (fimg + 0x2000)
static void f_log(const char *tag, const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  [%s] ", tag); vprintf(fmt, a); printf("\n"); va_end(a); }
static int f_print(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  > "); int r = vprintf(fmt, a); printf("\n"); va_end(a); return r; }
static uintptr_t f_base(void) { return (uintptr_t)fimg; }
static size_t f_size(void) { return sizeof fimg; }
static const char *f_ver(void) { return "test"; }
static void *f_find_string(const char *s)
{ return !strcmp(s, (char *)F_INIT_STR) ? F_INIT_STR : !strcmp(s, (char *)F_SPAWN_STR) ? F_SPAWN_STR : NULL; }
static void *f_find_xref(const void *t, int i) { return i ? NULL : t == F_INIT_STR ? F_INIT_X : t == F_SPAWN_STR ? F_SPAWN_X : NULL; }
static void *f_function_start(const void *a) { return a == F_INIT_X ? F_INIT_FN : NULL; }
static uintptr_t f_orig_init(uintptr_t c, uintptr_t b, uintptr_t d, uintptr_t e) { (void)b; (void)d; (void)e; f_init_seen = (int)(uint32_t)c; return 7; }
static uintptr_t f_orig_worker(void *o) { (void)o; return 0; }
static int f_hook(void *t, void *d, void **o)
{
    if (f_nhooks >= 2) return 1;
    *o = t == F_INIT_FN ? (void *)f_orig_init : t == F_WORKER ? (void *)f_orig_worker : NULL;
    if (!*o) return 1;
    f_hooks[f_nhooks][0] = t; f_hooks[f_nhooks][1] = d; f_nhooks++;
    return 0;
}
static const char *f_dir(void) { return fdir; }
static int f_reg(const char *n, const char *h, v3x_command_fn fn, void *u) { (void)h; (void)u; if (!strcmp(n, "cpusched")) { f_cmd_fn = fn; f_cmds++; } return 0; }
static void *hook_detour(void *target) { for (int i = 0; i < f_nhooks; i++) if (f_hooks[i][0] == target) return f_hooks[i][1]; return NULL; }

typedef uintptr_t (*init_fn)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef uintptr_t (*worker_fn)(void *);
typedef BOOL (WINAPI *gtscs_fn)(HANDLE, PULONG, ULONG, PULONG);
static gtscs_fn f_get_sets;
static int thread_sets(ULONG *ids, ULONG cap) { ULONG n = 0; return f_get_sets && f_get_sets(GetCurrentThread(), ids, cap, &n) ? (int)n : -1; }

typedef struct { int kind, idx, nsets; ULONG ids[64]; } wjob;
static DWORD WINAPI worker_thread(void *p)
{
    wjob *j = (wjob *)p;
    unsigned char obj[0x100]; memset(obj, 0, sizeof obj);
    memcpy(obj + CS_WORKER_OFF_INDEX, &j->idx, 4); memcpy(obj + CS_WORKER_OFF_KIND, &j->kind, 4);
    ((worker_fn)hook_detour(F_WORKER))(obj);
    j->nsets = thread_sets(j->ids, 64);
    return 0;
}

static void test_plugin(const char *dll)
{
    /* fake image: strings, init fn with its prologue, SpawnWorkers xref + call AddWorker, lea rcx,worker; call */
    memset(fimg, 0xcc, sizeof fimg);
    strcpy((char *)F_INIT_STR, "Spawning {} normal worker threads"); strcpy((char *)F_SPAWN_STR, "{}_{} {}");
    memcpy(F_INIT_FN, CS_SIG_INIT, sizeof CS_SIG_INIT);
    memcpy(F_ADD, CS_SIG_ADDWORKER, sizeof CS_SIG_ADDWORKER);
    memcpy(F_WORKER, CS_SIG_WORKER, sizeof CS_SIG_WORKER);
    F_SPAWN_X[0x30] = 0xe8; put_rel(F_SPAWN_X + 0x30, F_ADD, 5);
    unsigned char *b = F_ADD + 0x200; b[0] = 0x48; b[1] = 0x8d; b[2] = 0x0d; put_rel(b, F_WORKER, 7); b[7] = 0xe8;

    GetTempPathA(sizeof fdir, fdir); strcat(fdir, "cst_v3x\\"); CreateDirectoryA(fdir, NULL);
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%scpu_sched.ini", fdir);
    FILE *fi = fopen(ini, "w"); CHECK(fi != NULL); if (!fi) return;
    fprintf(fi, "[cpu_sched]\nnormal=pcores\ntask_threads=12\n"); fclose(fi);

    V3X_API a; memset(&a, 0, sizeof a);
    a.api_version = V3X_API_VERSION; a.struct_size = sizeof a; a.log = f_log;
    a.image_base = f_base; a.image_size = f_size; a.game_version = f_ver;
    a.find_string = f_find_string; a.find_xref = f_find_xref; a.function_start = f_function_start;
    a.hook = f_hook; a.game_dir = f_dir; a.v3x_dir = f_dir; a.register_command = f_reg; a.print = f_print;
    HMODULE h = LoadLibraryA(dll); CHECK(h != NULL); if (!h) return;
    v3x_plugin_load_fn load = (v3x_plugin_load_fn)GetProcAddress(h, "v3x_plugin_load");
    CHECK(load && load(&a) == 0);
    CHECK(f_cmds == 1);

    static cs_map m;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    gscsi_fn get = (gscsi_fn)GetProcAddress(k32, "GetSystemCpuSetInformation");
    f_get_sets = (gtscs_fn)GetProcAddress(k32, "GetThreadSelectedCpuSets");
    ULONG len = 0; get(NULL, 0, &len, GetCurrentProcess(), 0);
    unsigned char *cb = (unsigned char *)malloc(len); get(cb, len, &len, GetCurrentProcess(), 0); cs_classify(cb, len, &m); free(cb);

    if (!m.hybrid) {   /* not hybrid: only task_threads -> init hook only */
        CHECK(f_nhooks == 1 && hook_detour(F_INIT_FN) && !hook_detour(F_WORKER));
        if (hook_detour(F_INIT_FN)) CHECK(((init_fn)hook_detour(F_INIT_FN))((uint32_t)-1, 0, 0, 0) == 7 && f_init_seen == 12);
        printf("plugin: not hybrid, CPU-set assignment not exercised\n");
        return;
    }
    CHECK(f_nhooks == 2 && hook_detour(F_INIT_FN) && hook_detour(F_WORKER));
    if (f_nhooks != 2) return;
    ULONG ids[64];
    /* init on this thread: count -1 -> 12 and thread -> P-cores; later calls pass the count through */
    init_fn init = (init_fn)hook_detour(F_INIT_FN);
    CHECK(init((uint32_t)-1, 0, 0, 0) == 7 && f_init_seen == 12);
    int n = thread_sets(ids, 64);
    CHECK(n == m.np && n > 0 && ids[0] == m.pid[0] && ids[n - 1] == m.pid[m.np - 1]);
    CHECK(init(6, 0, 0, 0) == 7 && f_init_seen == 6);
    /* workers on their own threads: kind 0 -> E, kind 1 -> P (ini normal=pcores), kind 2 -> P, kind 9 -> none */
    int kinds[4] = { 0, 1, 2, 9 }, expect[4] = { m.ne, m.np, m.np, 0 };
    for (int k = 0; k < 4; k++) {
        static wjob j; memset(&j, 0, sizeof j); j.kind = kinds[k]; j.idx = k;
        HANDLE t = CreateThread(NULL, 0, worker_thread, &j, 0, NULL); WaitForSingleObject(t, INFINITE); CloseHandle(t);
        CHECK(j.nsets == expect[k]);
        if (k == 0) CHECK(j.nsets > 0 && j.ids[0] == m.eid[0]);
        if (k == 1) CHECK(j.nsets > 0 && j.ids[0] == m.pid[0]);
    }
    if (f_cmd_fn) f_cmd_fn("", NULL);
    printf("plugin: hooks + CPU-set assignment exercised (P %d, E %d)\n", m.np, m.ne);
}

int main(int argc, char **argv)
{
    test_classify();
    test_policy();
    test_sigs();
    test_this_machine();
    if (argc > 1 && strcmp(argv[1], "-")) test_exe(argv[1]);
    if (argc > 2) test_plugin(argv[2]);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
