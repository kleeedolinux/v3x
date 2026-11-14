/* cpu_sched: keep the sim on P-cores on hybrid (P/E) CPUs.
 * The engine never sets affinity or CPU sets (docs/re/threading.md); Windows may run the sim's
 * NORMAL workers and even the main thread on E-cores. This plugin uses CPU sets (soft affinity):
 *   - main thread (the thread calling the task-scheduler init) -> P-cores
 *   - task workers, by kind, in a detour on the worker thread entry:
 *       kind 0 BELOW_NORMAL -> E-cores, kind 1 NORMAL -> no restriction, kind 2 ABOVE_NORMAL -> P-cores
 *   - optional task_threads=N passed to the scheduler init when the game asks for auto (-1)
 * On non-hybrid CPUs (a single EfficiencyClass) it does nothing and logs "not hybrid".
 *
 * Anchors (fail closed, every step checked, mirrored in tools/re_threads_check.py):
 *   init:   "Spawning {} normal worker threads" -> xref -> function start, prologue CS_SIG_INIT
 *   worker: "{}_{} {}" -> xref (in SpawnWorkers) -> next call to a CS_SIG_ADDWORKER fn ->
 *           its `lea rcx,<entry>; call <thread start>` with entry matching CS_SIG_WORKER
 * Config: <game>/binaries/v3x/cpu_sched.ini (see cpu_sched.ini.example; all keys optional). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"
#include "v3x_safe.h"
#include "cs_core.h"

V3X_EXPORT const char *v3x_plugin_name = "cpu_sched";
V3X_PLUGIN_MANIFEST("cpu_sched", "0.5.0", "v3x", "keeps the simulation on P-cores on hybrid CPUs", "release/1.15.1");
#define TAG "cpu_sched"

static const V3X_API *api;
static unsigned char *g_base; static size_t g_size;
static char g_ini[MAX_PATH];

typedef BOOL (WINAPI *gscsi_fn)(void *info, ULONG len, PULONG ret, HANDLE proc, ULONG flags);
typedef BOOL (WINAPI *stscs_fn)(HANDLE thread, const ULONG *ids, ULONG n);
static stscs_fn g_set_cpusets;

static cs_map g_map;
static int g_pol_main = CS_PCORES, g_pol_kind[3] = { CS_ECORES, CS_ALL, CS_PCORES };
static int g_task_threads;          /* 0 = leave the game's value alone */
static int g_active;                /* hybrid + API available + enabled */

typedef uintptr_t (*init_fn)(uintptr_t count, uintptr_t b, uintptr_t c, uintptr_t d);
typedef uintptr_t (*worker_fn)(void *obj);
static init_fn g_orig_init;
static worker_fn g_orig_worker;
static void *g_init_addr, *g_worker_addr;

/* assignments, for the log and the console command */
#define CS_MAX_REC 192
typedef struct { DWORD tid; int kind, idx, policy, ok; } cs_rec;
static cs_rec g_rec[CS_MAX_REC];
static volatile LONG g_nrec, g_nworkers, g_kind_count[3];
static volatile LONG g_main_done; static DWORD g_main_tid; static int g_main_ok = -1, g_init_count_in = 0x7fffffff, g_init_count_out;

static const char *kind_name(int k) { return k == 0 ? "below_normal" : k == 1 ? "normal" : k == 2 ? "above_normal" : "?"; }
static unsigned long long rva(const void *p) { return (unsigned long long)((const unsigned char *)p - g_base); }
static int in_image(uint64_t va, size_t n) { return va >= (uint64_t)g_base && va + n <= (uint64_t)g_base + g_size; }
static const unsigned char *img_peek(void *ctx, uint64_t va, size_t n) { (void)ctx; return in_image(va, n) ? (const unsigned char *)va : NULL; }

/* Apply a policy to the current thread. Returns 1 applied, 0 nothing to do, -1 failed. */
static int apply(int policy)
{
    const uint32_t *ids; int n = cs_policy_ids(&g_map, policy, &ids);
    if (!n || !g_set_cpusets) return 0;
    return g_set_cpusets(GetCurrentThread(), (const ULONG *)ids, (ULONG)n) ? 1 : -1;
}

/* ------------------------------------------------------------------ detours */

static uintptr_t init_detour(uintptr_t count, uintptr_t b, uintptr_t c, uintptr_t d)
{
    if (!InterlockedExchange(&g_main_done, 1)) {
        g_main_tid = GetCurrentThreadId();
        g_main_ok = apply(g_pol_main);
        g_init_count_in = (int)(uint32_t)count;
        if ((int)(uint32_t)count == -1 && g_task_threads > 0) count = (uint32_t)g_task_threads;
        g_init_count_out = (int)(uint32_t)count;
        api->log(TAG, "scheduler init on thread %lu: TaskThreadCount %d -> %d; main thread -> %s: %s",
                 g_main_tid, g_init_count_in, g_init_count_out, CS_POLICY_NAMES[g_pol_main],
                 g_main_ok > 0 ? "applied" : g_main_ok == 0 ? "no restriction" : "FAILED");
    }
    return g_orig_init(count, b, c, d);
}

static uintptr_t worker_detour(void *obj)
{
    int kind = *(volatile int *)((unsigned char *)obj + CS_WORKER_OFF_KIND);
    int idx = *(volatile int *)((unsigned char *)obj + CS_WORKER_OFF_INDEX);
    int pol = kind >= 0 && kind <= 2 ? g_pol_kind[kind] : CS_ALL;
    int ok = apply(pol);
    InterlockedIncrement(&g_nworkers);
    if (kind >= 0 && kind <= 2) InterlockedIncrement(&g_kind_count[kind]);
    LONG r = InterlockedIncrement(&g_nrec) - 1;
    if (r < CS_MAX_REC) { cs_rec e = { GetCurrentThreadId(), kind, idx, pol, ok }; g_rec[r] = e; }
    api->log(TAG, "worker %s #%d (tid %lu) -> %s: %s", kind_name(kind), idx, GetCurrentThreadId(), CS_POLICY_NAMES[pol],
             ok > 0 ? "applied" : ok == 0 ? "no restriction" : "FAILED");
    return g_orig_worker(obj);
}

/* ------------------------------------------------------------------ core map */

static void ids_line(char *out, size_t n, const uint32_t *ids, int k)
{
    V3xStr ob; v3x_str_init(&ob, out, n);
    for (int i = 0; i < k; i++) v3x_str_appendf(&ob, i ? ",%u" : "%u", ids[i]);
}

static void map_text(int (*out)(const char *fmt, ...), int is_log)
{
    char p[1024], e[1024];
    ids_line(p, sizeof p, g_map.pid, g_map.np); ids_line(e, sizeof e, g_map.eid, g_map.ne);
#define CS_OUT(...) do { if (is_log) api->log(TAG, __VA_ARGS__); else out(__VA_ARGS__); } while (0)
    CS_OUT("%d CPU sets, EfficiencyClass %d..%d: %s", g_map.n, g_map.eff_min, g_map.eff_max, g_map.hybrid ? "hybrid" : "not hybrid");
    if (g_map.hybrid) {
        CS_OUT("P-cores: %d logical / %d physical, CPU set ids %s", g_map.np, g_map.p_phys, p);
        CS_OUT("E-cores: %d logical / %d physical, CPU set ids %s", g_map.ne, g_map.e_phys, e);
    }
    CS_OUT("policy: main=%s below_normal=%s normal=%s above_normal=%s task_threads=%d (%s)",
        CS_POLICY_NAMES[g_pol_main], CS_POLICY_NAMES[g_pol_kind[0]], CS_POLICY_NAMES[g_pol_kind[1]],
        CS_POLICY_NAMES[g_pol_kind[2]], g_task_threads, g_active ? "active" : "inactive");
#undef CS_OUT
}

static void cmd_cpusched(const char *args, void *u)
{
    (void)args; (void)u;
    map_text(api->print, 0);
    api->print("hooks: scheduler init %s, worker entry %s", g_orig_init ? "installed" : "off", g_orig_worker ? "installed" : "off");
    if (g_main_done)
        api->print("main thread %lu -> %s: %s (TaskThreadCount %d -> %d)", g_main_tid, CS_POLICY_NAMES[g_pol_main],
                   g_main_ok > 0 ? "applied" : g_main_ok == 0 ? "no restriction" : "FAILED", g_init_count_in, g_init_count_out);
    else api->print("main thread: scheduler init not seen yet");
    api->print("workers: %ld (below_normal %ld, normal %ld, above_normal %ld)", g_nworkers, g_kind_count[0], g_kind_count[1], g_kind_count[2]);
    LONG n = g_nrec < CS_MAX_REC ? g_nrec : CS_MAX_REC;
    for (LONG i = 0; i < n; i++)
        api->print("  tid %5lu %-12s #%-2d -> %-6s %s", g_rec[i].tid, kind_name(g_rec[i].kind), g_rec[i].idx, CS_POLICY_NAMES[g_rec[i].policy],
                   g_rec[i].ok > 0 ? "applied" : g_rec[i].ok == 0 ? "no restriction" : "FAILED");
}

static int read_map(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    gscsi_fn get = (gscsi_fn)GetProcAddress(k32, "GetSystemCpuSetInformation");
    g_set_cpusets = (stscs_fn)GetProcAddress(k32, "SetThreadSelectedCpuSets");
    if (!get || !g_set_cpusets) { api->log(TAG, "CPU set API not available (Windows < 10): inactive"); return 0; }
    ULONG len = 0;
    get(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len) { api->log(TAG, "GetSystemCpuSetInformation returned no data (error %lu): inactive", GetLastError()); return 0; }
    unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, len);
    int ok = buf && get(buf, len, &len, GetCurrentProcess(), 0) && cs_classify(buf, len, &g_map) > 0;
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (!ok) { api->log(TAG, "GetSystemCpuSetInformation failed (error %lu): inactive", GetLastError()); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ anchors */

static void *find_init(void)
{
    void *s = api->find_string("Spawning {} normal worker threads");
    void *x = s ? api->find_xref(s, 0) : NULL;
    unsigned char *f = x ? api->function_start(x) : NULL;
    if (!f || !in_image((uint64_t)f, sizeof CS_SIG_INIT) || !cs_sig_match(f, CS_SIG_INIT, CS_MASK_INIT)) {
        api->log(TAG, "scheduler init not found / signature mismatch (string %p xref %p fn %p): init hook off", s, x, (void *)f);
        return NULL;
    }
    return f;
}

static void *find_worker(void)
{
    void *s = api->find_string("{}_{} {}");
    unsigned char *x = s ? api->find_xref(s, 0) : NULL;
    if (!x || api->find_xref(s, 1)) { api->log(TAG, "worker entry: anchor \"{}_{} {}\" %s: worker hook off", x ? "ambiguous" : "not found"); return NULL; }
    size_t n = in_image((uint64_t)x, 0x100) ? 0x100 : 0;
    uint64_t add = n ? cs_find_call_to(x, n, (uint64_t)x, CS_SIG_ADDWORKER, CS_MASK_ADDWORKER, img_peek, NULL) : 0;
    if (!add || !in_image(add, 0x400)) { api->log(TAG, "worker entry: AddWorker call not found: worker hook off"); return NULL; }
    uint64_t w = cs_find_lea_rcx_call((const unsigned char *)add, 0x400, add, CS_SIG_WORKER, CS_MASK_WORKER, img_peek, NULL);
    if (!w) { api->log(TAG, "worker entry: lea rcx,<entry> not found / signature mismatch: worker hook off"); return NULL; }
    api->log(TAG, "AddWorker at exe+0x%llx", rva((void *)add));
    return (void *)w;
}

/* ------------------------------------------------------------------ entry */

static int pol_cfg(const char *key, int def)
{
    char v[32]; GetPrivateProfileStringA("cpu_sched", key, CS_POLICY_NAMES[def], v, sizeof v, g_ini);
    int p = cs_parse_policy(v, -1);
    if (p < 0) { api->log(TAG, "ini: %s=%s is not all|pcores|ecores, using %s", key, v, CS_POLICY_NAMES[def]); p = def; }
    return p;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    g_base = (unsigned char *)api->image_base(); g_size = api->image_size();
    snprintf(g_ini, sizeof g_ini, "%scpu_sched.ini", api->v3x_dir());
    if (!GetPrivateProfileIntA("cpu_sched", "enabled", 1, g_ini)) { api->log(TAG, "disabled in %s", g_ini); return 0; }
    g_pol_main = pol_cfg("main", CS_PCORES);
    g_pol_kind[0] = pol_cfg("below_normal", CS_ECORES);
    g_pol_kind[1] = pol_cfg("normal", CS_ALL);
    g_pol_kind[2] = pol_cfg("above_normal", CS_PCORES);
    g_task_threads = (int)GetPrivateProfileIntA("cpu_sched", "task_threads", 0, g_ini);
    if (g_task_threads < 0 || g_task_threads > 64) { api->log(TAG, "ini: task_threads=%d out of range 0..64, ignored", g_task_threads); g_task_threads = 0; }

    int have = read_map();
    g_active = have && g_map.hybrid;
    map_text(api->print, 1);
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *))
        api->register_command("cpusched", "cpu_sched: show the P/E core map and each worker's CPU set assignment", cmd_cpusched, NULL);
    if (!g_active && !g_task_threads) { api->log(TAG, have ? "not hybrid: nothing to do" : "inactive"); return 0; }
    if (!g_active) api->log(TAG, "%s: only task_threads will be applied", have ? "not hybrid" : "no CPU set data");

    g_init_addr = find_init();
    if (g_init_addr) {
        int rc = api->hook(g_init_addr, (void *)init_detour, (void **)&g_orig_init);
        api->log(TAG, "scheduler init at exe+0x%llx: hook %s (rc %d)", rva(g_init_addr), rc ? "FAILED" : "installed", rc);
        if (rc) g_orig_init = NULL;
    }
    if (g_active) {
        g_worker_addr = find_worker();
        if (g_worker_addr) {
            int rc = api->hook(g_worker_addr, (void *)worker_detour, (void **)&g_orig_worker);
            api->log(TAG, "worker entry at exe+0x%llx: hook %s (rc %d)", rva(g_worker_addr), rc ? "FAILED" : "installed", rc);
            if (rc) g_orig_worker = NULL;
        }
    }
    return 0;
}
