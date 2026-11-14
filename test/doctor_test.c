/* Offline test of the in-game doctor (plugins/doctor/dr_core.c): every check on fake inputs, the
 * comment-preserving ini fixer (idempotence, atomic write, backups), the report masking.
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o D:/v3x_tmp/doctor/drt.exe test/doctor_test.c
 *   D:/v3x_tmp/doctor/drt.exe D:/v3x_tmp/doctor/t [<game>/binaries/v3x]
 * argv[1] = scratch folder (created; fake ini / content_packs / mod folders go there).
 * argv[2] (optional) = a real binaries/v3x folder: its v3x.log, crash logs and inis are only READ and
 * the results printed (smoke test on real data). Prints "N checks, M failed".
 *
 * -DDOCTOR_E2E: end-to-end mode. The exe links build/version.dll like victoria3.exe and loads the real
 * doctor.dll from <dir>/v3x/plugins (the e2e dir is a fake game folder, never the real one):
 *   <dir>/doctor_e2e.exe, <dir>/version.dll, <dir>/v3x/plugins/doctor.dll
 *   cd <dir> && V3X_NO_CONSOLE=1 ./doctor_e2e.exe
 * It drives `doctor`, `doctor fix D001`, `doctor report` through exec_capture and emits load_transition
 * Empty->Game on the main thread (handler time must stay < 5 ms; the summary line arrives via run_on_main). */
#include "../plugins/doctor/dr_core.c"
#include <stdio.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const dr_result *find(const dr_results *r, const char *id)
{
    for (int i = 0; i < r->n; i++)
        if (!strcmp(r->r[i].id, id)) return &r->r[i];
    return NULL;
}
static int sev_of(const dr_results *r, const char *id)
{
    const dr_result *x = find(r, id);
    return x ? x->sev : -1;
}
static void dump(const dr_results *r)
{
    char line[1200];
    for (int i = 0; i < r->n; i++) { dr_format_result(&r->r[i], line, sizeof line); printf("  %s\n", line); }
}

static char *slurp(const char *path)
{
    int ex;
    return read_all(path, 64u << 20, &ex);
}
static void spit(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}
static void mkdirs(const char *path)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 3; *s; s++)
        if (*s == '\\' || *s == '/') { char c = *s; *s = 0; CreateDirectoryA(p, NULL); *s = c; }
    CreateDirectoryA(p, NULL);
}
static int count_tmp(const char *dir)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int n = 0;
    snprintf(pat, sizeof pat, "%s\\*.tmp", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do n++; while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

/* ---- a fake session -------------------------------------------------------------------------- */

static const char *FAKE_LOG =
    "[16:26:00.916][v3x] V3X api v5, exe=D:\\g\\binaries\\victoria3.exe base=00007FF6A3280000 size=0x60fb000 game=release/1.15.1\n"
    "[16:26:01.416][alloc_tracker] attribution skips 6981 allocator wrapper function(s)\n"
    "[16:26:01.563][v3x] loaded plugin 'alloc_tracker' v0.5.0 (alloc_tracker.dll)\n"
    "[16:26:01.600][v3x] plugin hello disabled (v3x.ini)\n"
    "[16:26:01.827][console_plus] console_plus.ini: 1 lines ignored\n"
    "[16:26:04.640][v3x] loaded plugin 'console_plus' (console_plus.dll)\n"
    "[16:26:05.000][v3x] plugin 'gated_one' (gated_one.dll) skipped: unknown game build 1234-5678 not declared (unknown_build = load to force)\n"
    "[16:26:05.100][v3x] plugin 'newer' (newer.dll) refused: needs loader API v9 (this is v5)\n"
    "[16:26:05.200][v3x] FAILED to load broken.dll (error 193)\n"
    "[16:26:05.300][tick_profiler] RTTI: 4000 COLs, 20 CTickTask_ vtables\n"
    "[16:26:05.310][tick_profiler] only 20 tick tasks resolved (<50), not hooking\n"
    "[16:26:05.320][v3x] plugin 'tick_profiler' returned 2 from v3x_plugin_load\n"
    "[16:26:05.330][v3x] plugin 'tick_profiler' rolled back: 0 hook target(s) released, 0 patch(es) restored, callbacks disabled\n"
    "[16:26:05.330][tick_profiler] a line after the failure that must not become the reason\n"
    "[16:26:06.000][par_load] anchor realdir FAILED: need 1 shared getRealDir callee (not hooked)\n"
    "[16:26:06.010][v3x] loaded plugin 'par_load' (par_load.dll)\n"
    "[16:26:06.020][v3x] FAULT in plugin badload.dll during v3x_plugin_load: exception 0xc0000005 at badload.dll+0x10\n"
    "[16:26:06.030][v3x] ERROR: the game crashed while loading plugin crashy last start: skipping it this start (the next start tries again; disable it with [plugins] crashy = 0 in v3x.ini)\n"
    "[16:26:06.100][v3x] WARNING: plugin 'oldman' lists game builds \"release/1.14*\"; running release/1.15.1 (build 6af3094f-60fb000). Loading anyway.\n"
    "[16:26:06.110][v3x] loaded plugin 'oldman' (oldman.dll)\n"
    "[16:26:06.200][v3x] loaded plugin 'lazy_gfx' (lazy_gfx.dll)\n"
    "[16:26:06.300][v3x] loaded plugin 'devtools' (devtools.dll)\n"
    "[16:26:06.400][v3x] loaded plugin 'fast_lookup' (fast_lookup.dll)\n"
    "[16:26:06.410][v3x] loaded plugin 'mod_loader' (mod_loader.dll)\n"
    "[16:26:06.420][tick_par] inactive (enabled=1 mode=off)\n"
    "[16:26:06.430][v3x] loaded plugin 'tick_par' (tick_par.dll)\n"
    "[16:26:06.440][v3x] loaded plugin 'log_filter' (log_filter.dll)\n"
    "[16:26:06.450][v3x] loaded plugin 'ai_par' (ai_par.dll)\n"
    "[16:26:06.460][v3x] loaded plugin 'crash_logger' (crash_logger.dll)\n"
    "[16:26:06.470][v3x] loaded plugin 'doctor' (doctor.dll)\n"
    "[16:26:14.000][v3x] 13/20 plugins loaded, 1 disabled in v3x.ini\n"
    "[16:40:00.000][v3x] FAULT in plugin console_plus.dll during console command cplus: exception 0xc0000005 (read of 0x0) at console_plus.dll+0x7349\n"
    "[16:40:00.001][v3x] ERROR: plugin console_plus.dll disabled: its frame/event/command/run_on_main callbacks are skipped from now on (its hooks stay). The game continues; restart to load it again.\n";

static const char *const FAKE_STEMS[] = {"alloc_tracker", "hello", "console_plus", "gated_one", "newer", "broken", "tick_profiler",
                                         "par_load", "badload", "crashy", "oldman", "lazy_gfx", "devtools", "fast_lookup",
                                         "mod_loader", "tick_par", "log_filter", "ai_par", "crash_logger", "doctor", "added_later"};

/* crash.prev.log in the real crash_logger format (2026-11-13 session, shortened) */
static const char *CRASH_PREV =
    "15:31:41.296 [14952] crash_logger started 2026-11-13, pid 7584, exe D:\\g\\binaries\\victoria3.exe, game release/1.15.1\n"
    "15:31:41.296 [14952]   memory: private=35 MB working_set=128 MB | phys avail=9148/16108 MB | commit avail=26869 MB of limit 38194 MB (load 43%)\n"
    "15:51:49.997 [14952] mem (changed): private=18115 MB | commit avail=9635 MB of limit 38192 MB | phys avail=2543 MB\n"
    "16:01:17.170 [14952] EXCEPTION ACCESS_VIOLATION (0xc0000005) read of address 0x7ffd8c000100 at victoria3.exe+0x43d94d6\n"
    "16:01:17.170 [14952]   rax=00000000000000de rbx=00007ffd8cde15e0 rcx=00007ffd8cde1190 rdx=000000e13e51c000\n"
    "16:01:17.170 [14952]   rip=00007ff6a76594d6 eflags=00010206\n"
    "16:01:17.170 [14952]   memory: private=18822 MB working_set=6419 MB | phys avail=3356/16108 MB | commit avail=1949 MB of limit 38192 MB (load 79%)\n"
    "16:01:17.174 [14952]   #00 victoria3.exe+0x43d94d6\n"
    "16:01:17.174 [14952]   #01 victoria3.exe+0x67b8cf\n"
    "16:01:17.174 [14952]   #02 console_plus.dll+0x7349\n"
    "16:01:17.174 [14952]   #03 VERSION.dll+0x3dc6\n"
    "16:01:17.174 [14952]   #04 ntdll.dll+0x4cc91\n";

static const char *CRASH_CUR =   /* this session (pid 4242): pagefile grew 38192 -> 40264 */
    "16:26:05.075 [10480] crash_logger started 2026-11-13, pid 4242, exe D:\\g\\binaries\\victoria3.exe, game release/1.15.1\n"
    "16:26:05.075 [10480]   memory: private=35 MB working_set=128 MB | phys avail=6023/16108 MB | commit avail=22819 MB of limit 38192 MB (load 62%)\n"
    "17:29:56.000 [10480] mem (changed): private=20100 MB | commit avail=4300 MB of limit 39222 MB | phys avail=900 MB\n"
    "18:01:34.000 [10480] mem (changed): private=20500 MB | commit avail=4500 MB of limit 40264 MB | phys avail=800 MB\n";

static void base_inputs(dr_inputs *in)
{
    memset(in, 0, sizeof *in);
    in->game_version = "release/1.15.1";
    in->build_id = "6af3094f-60fb000";
    in->known_builds = "6af3094f-60fb000";
    in->loader_version = "0.5.0";
    in->api_version = 5;
    in->v3x_log = FAKE_LOG;
    in->plugin_files = FAKE_STEMS;
    in->nplugin_files = (int)(sizeof FAKE_STEMS / sizeof FAKE_STEMS[0]);
    in->pid = 4242;
    in->exe_dir = "D:\\g\\binaries\\";
    in->loader_path = "D:\\g\\binaries\\version.dll";
}
static void set_ini(dr_inputs *in, const char *name, const char *text)
{
    for (int i = 0; i < in->ninis; i++)
        if (!strcmp(in->inis[i].name, name)) { in->inis[i].text = text; return; }
    in->inis[in->ninis].name = name;
    in->inis[in->ninis++].text = text;
}

static dr_results R;
static dr_plugins P;

/* ---- tests ---------------------------------------------------------------------------------- */

static void test_ini(void)
{
    char *o = NULL, old[64], v[64];
    /* value replaced, everything else byte-identical (comments, spacing, inline comment) */
    const char *a = "; alloc_tracker config\n[alloc_tracker]\n; 0 = off\nenabled = 1   ; note\nbig_kb=64\n";
    CHECK(dr_ini_set(a, "alloc_tracker", "enabled", "0", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "; alloc_tracker config\n[alloc_tracker]\n; 0 = off\nenabled = 0   ; note\nbig_kb=64\n"));
    CHECK(!strcmp(old, "1"));
    char *o2 = NULL;
    CHECK(dr_ini_set(o, "alloc_tracker", "enabled", "0", 0, &o2, old, sizeof old) == 0);   /* idempotent */
    CHECK(o2 == NULL && !strcmp(old, "0"));
    CHECK(dr_ini_set(o, "alloc_tracker", "ENABLED", "false", 0, &o2, old, sizeof old) == 0);  /* 0 == false */
    free(o);

    /* CRLF and BOM kept */
    const char *b = "\xEF\xBB\xBF[alloc_tracker]\r\nenabled=1\r\nx=2\r\n";
    CHECK(dr_ini_set(b, "alloc_tracker", "enabled", "0", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "\xEF\xBB\xBF[alloc_tracker]\r\nenabled=0\r\nx=2\r\n"));
    free(o);

    /* key missing: added after the section's last key, before the next section's comment */
    const char *c = "[a]\nx=1\n\n# about b\n[b]\ny=2\n";
    CHECK(dr_ini_set(c, "a", "k", "5", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "[a]\nx=1\nk = 5\n\n# about b\n[b]\ny=2\n"));
    CHECK(old[0] == 0);
    free(o);
    /* section missing: appended */
    CHECK(dr_ini_set("console=1\n", "plugins", "tick_profiler", "0", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "console=1\n\n[plugins]\ntick_profiler = 0\n"));
    free(o);
    /* top-level key in a v3x.ini with [plugins]: goes after console=1, not into [plugins] */
    const char *d = "# V3X settings\nconsole=1\n\n[plugins]\nfast_lookup=1\n";
    CHECK(dr_ini_set(d, "", "unknown_build", "safe", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "# V3X settings\nconsole=1\nunknown_build = safe\n\n[plugins]\nfast_lookup=1\n"));
    free(o);
    /* top region only comments: right before the first header */
    CHECK(dr_ini_set("# c\n[plugins]\nx=1\n", "", "unknown_build", "safe", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "# c\nunknown_build = safe\n[plugins]\nx=1\n"));
    free(o);
    /* [plugins] key present with other case: replaced in place */
    CHECK(dr_ini_set("[Plugins]\nTick_Profiler = 1 # dev\n", "plugins", "tick_profiler", "0", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "[Plugins]\nTick_Profiler = 0 # dev\n"));
    free(o);
    /* any_section: every duplicate replaced (first-wins and last-wins parsers then agree) */
    const char *e = "mode = measure\n[x]\nmode=on\nmode = measure ; again\n";
    CHECK(dr_ini_set(e, "", "mode", "off", 1, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "mode = off\n[x]\nmode=off\nmode = off ; again\n"));
    CHECK(!strcmp(old, "measure"));
    free(o);
    /* any_section with a plugin-named section and no key: into that section */
    CHECK(dr_ini_set("# lf\n[log_filter]\npreset = default\n", "log_filter", "enabled", "1", 1, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "# lf\n[log_filter]\npreset = default\nenabled = 1\n"));
    free(o);
    /* empty file, file without trailing newline, flat comments-only file */
    CHECK(dr_ini_set("", "alloc_tracker", "enabled", "0", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "[alloc_tracker]\nenabled = 0\n"));
    free(o);
    CHECK(dr_ini_set("profile=1", "", "profile", "0", 1, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "profile=0"));
    free(o);
    CHECK(dr_ini_set("csv = 1", "", "interval", "3", 1, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "csv = 1\ninterval = 3\n"));
    free(o);
    CHECK(dr_ini_set("; only comments\n", "", "csv", "0", 1, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "; only comments\ncsv = 0\n"));
    free(o);
    /* a commented-out key is not the key */
    CHECK(dr_ini_set("[a]\n; enabled = 1\n", "a", "enabled", "0", 0, &o, old, sizeof old) == 1);
    CHECK(o && !strcmp(o, "[a]\nenabled = 0\n; enabled = 1\n"));
    free(o);

    /* get */
    CHECK(dr_ini_get("[a]\nk=1\n[b]\nk=2\n", "b", "k", 0, v, sizeof v) && !strcmp(v, "2"));
    CHECK(dr_ini_get("[a]\nk=1\n[b]\nk=2\n", "a", "k", 0, v, sizeof v) && !strcmp(v, "1"));
    CHECK(!dr_ini_get("[a]\nk=1\n", "", "k", 0, v, sizeof v));
    CHECK(dr_ini_get("k=1\n[x]\nk = 3 ; c\n", "", "k", 1, v, sizeof v) && !strcmp(v, "3"));
    CHECK(dr_ini_get("k = a#b\n", "", "k", 1, v, sizeof v) && !strcmp(v, "a#b"));   /* '#' without space is data */
    CHECK(!dr_ini_get(NULL, "", "k", 1, v, sizeof v));
    CHECK(dr_truthy("1") && dr_truthy("yes") && dr_truthy("on") && !dr_truthy("0") && !dr_truthy("off") && !dr_truthy(""));
    CHECK(dr_list_has("a;6af3094f-60fb000; b", "6AF3094F-60FB000") && dr_list_has("a; b", "b") && !dr_list_has("ab", "a"));
}

static DWORD WINAPI hold_ini_lock(LPVOID ready)
{
    HANDLE m = CreateMutexW(NULL, TRUE, L"Local\\v3x_ini");
    SetEvent((HANDLE)ready);
    Sleep(300);
    ReleaseMutex(m); CloseHandle(m);
    return 0;
}

static void test_apply_fix(const char *dir)
{
    char path[MAX_PATH], bak[MAX_PATH], msg[700], v3x[MAX_PATH];
    snprintf(v3x, sizeof v3x, "%s\\fix\\", dir);
    mkdirs(v3x);
    snprintf(path, sizeof path, "%salloc_tracker.ini", v3x);
    snprintf(bak, sizeof bak, "%s.doctor-bak", path);
    DeleteFileA(path); DeleteFileA(bak);
    const char *orig = "; comment kept\r\n[alloc_tracker]\r\nenabled = 1 ; on for one load\r\nbig_kb=64\r\n";
    spit(path, orig);
    dr_fix f = {"alloc_tracker.ini", "alloc_tracker", "enabled", "0", 0};
    CHECK(dr_apply_fix(v3x, &f, msg, sizeof msg) == 1);
    CHECK(strstr(msg, "1 -> 0") && strstr(msg, "next game start") && strstr(msg, "Undo"));
    char *t = slurp(path);
    CHECK(t && !strcmp(t, "; comment kept\r\n[alloc_tracker]\r\nenabled = 0 ; on for one load\r\nbig_kb=64\r\n"));
    free(t);
    t = slurp(bak);
    CHECK(t && !strcmp(t, orig));   /* the original is kept */
    free(t);
    CHECK(dr_apply_fix(v3x, &f, msg, sizeof msg) == 0);   /* idempotent */
    CHECK(strstr(msg, "already"));
    /* a second, different change keeps the FIRST original as backup */
    dr_fix f2 = {"alloc_tracker.ini", "alloc_tracker", "big_kb", "128", 0};
    CHECK(dr_apply_fix(v3x, &f2, msg, sizeof msg) == 1);
    t = slurp(bak);
    CHECK(t && !strcmp(t, orig));
    free(t);
    CHECK(count_tmp(v3x) == 0);
    /* atomic: while another program holds the file open (no delete sharing) the replace fails and the
     * file stays exactly as it was - never half written; no temp file is left */
    t = slurp(path);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    dr_fix f3 = {"alloc_tracker.ini", "alloc_tracker", "enabled", "1", 0};
    CHECK(dr_apply_fix(v3x, &f3, msg, sizeof msg) == -1);
    CHECK(strstr(msg, "nothing changed") != NULL);
    CloseHandle(h);
    char *t2 = slurp(path);
    CHECK(t && t2 && !strcmp(t, t2));
    free(t); free(t2);
    CHECK(count_tmp(v3x) == 0);
    /* missing file: created, no backup */
    snprintf(path, sizeof path, "%sdevtools.ini", v3x);
    DeleteFileA(path);
    snprintf(bak, sizeof bak, "%s.doctor-bak", path);
    DeleteFileA(bak);
    dr_fix f4 = {"devtools.ini", "", "profile", "0", 1};
    CHECK(dr_apply_fix(v3x, &f4, msg, sizeof msg) == 1);
    CHECK(strstr(msg, "created") != NULL);
    t = slurp(path);
    CHECK(t && !strcmp(t, "profile = 0\n"));
    free(t);
    CHECK(GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES);
    /* L11: another writer (installer / mod_menu) holds Local\v3x_ini -> the fix waits for it */
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, hold_ini_lock, ready, 0, NULL);
    WaitForSingleObject(ready, 5000);
    DWORD t0 = GetTickCount();
    dr_fix f5 = {"devtools.ini", "", "profile", "1", 1};
    CHECK(dr_apply_fix(v3x, &f5, msg, sizeof msg) == 1);
    DWORD waited = GetTickCount() - t0;
    CHECK(waited >= 250 && waited < 4000);
    WaitForSingleObject(th, 5000); CloseHandle(th); CloseHandle(ready);
}

static void test_log_parse(void)
{
    dr_plugins ps;
    memset(&ps, 0, sizeof ps);
    dr_parse_log(FAKE_LOG, &ps);
    CHECK(ps.log_summary == 1);
    dr_plugin *p;
    CHECK((p = plug_find(&ps, "alloc_tracker")) && p->state == DR_PS_LOADED && !strcmp(p->version, "0.5.0"));
    CHECK((p = plug_find(&ps, "hello")) && p->state == DR_PS_DISABLED_INI);
    CHECK((p = plug_find(&ps, "gated_one")) && p->state == DR_PS_GATED);
    CHECK((p = plug_find(&ps, "newer")) && p->state == DR_PS_REFUSED_API);
    CHECK((p = plug_find(&ps, "broken")) && p->state == DR_PS_LOAD_ERROR && strstr(p->why, "193"));
    CHECK((p = plug_find(&ps, "tick_profiler")) && p->state == DR_PS_FAILED_RC);
    CHECK(p && !strcmp(p->why, "only 20 tick tasks resolved (<50), not hooking"));   /* its last line BEFORE the failure */
    CHECK((p = plug_find(&ps, "par_load")) && p->state == DR_PS_LOADED && p->anchor_fail && strstr(p->anchor, "realdir"));
    CHECK((p = plug_find(&ps, "badload")) && p->state == DR_PS_FAULT_LOAD);
    CHECK((p = plug_find(&ps, "crashy")) && p->state == DR_PS_CRASH_SKIP);
    CHECK((p = plug_find(&ps, "oldman")) && p->manifest_warn && p->state == DR_PS_LOADED);
    CHECK((p = plug_find(&ps, "console_plus")) && p->faulted && strstr(p->fault, "console command cplus"));
    CHECK((p = plug_find(&ps, "tick_par")) && p->inactive);
    CHECK(ps.nignored == 1 && strstr(ps.ignored[0], "console_plus.ini"));
    CHECK(!plug_find(&ps, "v3x") && !plug_find(&ps, "console"));

    /* gate lines */
    memset(&ps, 0, sizeof ps);
    dr_parse_log("[00:00:00.000][v3x] WARNING: game build 1-2 (release/1.16) is not one this loader was tested on (6af3094f-60fb000). "
                 "Safe mode: only plugins whose manifest declares this build are loaded.\n", &ps);
    CHECK(ps.gate_active && !ps.gate_forced);
    memset(&ps, 0, sizeof ps);
    dr_parse_log("[00:00:00.000][v3x] WARNING: game build 1-2 (release/1.16) is not one this loader was tested on (x); "
                 "unknown_build = load: loading every plugin anyway\n", &ps);
    CHECK(ps.gate_forced && !ps.gate_active);

    /* `plugins` command output (fallback when the log level hides the load lines) */
    const char *cmd =
        "5 plugin file(s):\n"
        "  alloc_tracker        0.5.0      alloc_tracker.dll            loaded at 00007FFD12340000\n"
        "                       allocation tracker  -- v3x\n"
        "  hello                -          hello.dll                    disabled (v3x.ini [plugins])\n"
        "  par_load             0.5.0      par_load.dll                 FAULTED in on_frame callback (exception 0xc0000005), callbacks disabled\n"
        "  ?                    -          junk.dll                     FAILED LoadLibrary (error 126)\n"
        "  gated_one            1.0        gated_one.dll                skipped: unknown game build x not declared (unknown_build = load to force)\n";
    memset(&ps, 0, sizeof ps);
    dr_parse_plugins_cmd(cmd, &ps);
    CHECK(ps.n == 5);
    CHECK((p = plug_find(&ps, "alloc_tracker")) && p->state == DR_PS_LOADED && !strcmp(p->version, "0.5.0"));
    CHECK((p = plug_find(&ps, "hello")) && p->state == DR_PS_DISABLED_INI);
    CHECK((p = plug_find(&ps, "par_load")) && p->state == DR_PS_LOADED && p->faulted);
    CHECK((p = plug_find(&ps, "junk")) && p->state == DR_PS_LOAD_ERROR && strstr(p->why, "126"));
    CHECK((p = plug_find(&ps, "gated_one")) && p->state == DR_PS_GATED);
}

static void test_checks(const char *dir)
{
    dr_inputs in;
    /* ---- the full fake session */
    base_inputs(&in);
    set_ini(&in, "v3x.ini", "console=1\n[plugins]\nhello=0\nghost=1\nalloc_tracker=1\n");
    set_ini(&in, "alloc_tracker.ini", "[alloc_tracker]\nenabled = 1\n");
    set_ini(&in, "devtools.ini", "profile=1\ninterval_sec=10\n");
    set_ini(&in, "lazy_gfx.ini", "mode = on\n");                 /* csv defaults to 1 */
    set_ini(&in, "fast_lookup.ini", "; default mode = measure\n");
    set_ini(&in, "mod_loader.ini", "mode = on\n");
    set_ini(&in, "tick_par.ini", "[tick_par]\nmode=measure\n");   /* but it logged inactive */
    set_ini(&in, "log_filter.ini", "[log_filter]\nenabled = 0\n");
    set_ini(&in, "ai_par.ini", "[ai_par]\nfine_chunks = 1\n");    /* measure needed for the lever */
    in.have_mem = 1;
    in.phys_total = 16ull << 30; in.phys_avail = 2ull << 30;
    in.commit_limit = 38ull << 30; in.commit_total = (uint64_t)(0.9 * (double)(38ull << 30)); in.private_bytes = 20ull << 30;
    snprintf(in.game_drive, sizeof in.game_drive, "D:");
    in.have_game_disk = 1; in.game_free = 100ull << 30; in.game_total = 1000ull << 30;
    in.have_c_disk = 1; in.c_free = 3ull << 30; in.c_total = 200ull << 30;
    in.crash_log = CRASH_CUR; in.crash_prev = CRASH_PREV;
    const char *mods[] = {"C:\\Windows\\System32\\ntdll.dll", "D:\\g\\binaries\\version.dll", "D:\\g\\binaries\\dinput8.dll",
                          "C:\\Windows\\System32\\dinput8.dll", "C:\\Program Files\\RivaTuner\\RTSSHooks64.dll"};
    in.modules = mods; in.nmodules = 5;
    /* mods: a content pack with frontend_main.gui and an enabled mod that has its own copy */
    char packs[MAX_PATH], mod1[MAX_PATH], mod2[MAX_PATH], p[MAX_PATH * 2], json[2048];
    snprintf(packs, sizeof packs, "%s\\v3x\\content_packs", dir);
    snprintf(p, sizeof p, "%s\\v3x_menu\\gui\\frontend", packs); mkdirs(p);
    snprintf(p, sizeof p, "%s\\v3x_menu\\gui\\frontend\\frontend_main.gui", packs); spit(p, "x");
    snprintf(p, sizeof p, "%s\\v3x_menu\\descriptor.txt", packs); spit(p, "pack root file: not owned");
    snprintf(mod1, sizeof mod1, "%s\\workshop\\111", dir);
    snprintf(p, sizeof p, "%s\\gui\\frontend", mod1); mkdirs(p);
    snprintf(p, sizeof p, "%s\\gui\\frontend\\frontend_main.gui", mod1); spit(p, "mod menu");
    snprintf(p, sizeof p, "%s\\.metadata", mod1); mkdirs(p);
    snprintf(p, sizeof p, "%s\\.metadata\\metadata.json", mod1); spit(p, "{ \"name\" : \"Better UI\", \"id\": \"x\" }");
    snprintf(mod2, sizeof mod2, "%s\\workshop\\222", dir);
    mkdirs(mod2);
    snprintf(p, sizeof p, "%s\\descriptor.txt", mod2); spit(p, "same name as a pack ROOT file: no hit");
    char e1[MAX_PATH * 2], e2[MAX_PATH * 2];
    size_t k = 0;
    for (const char *s = mod1; *s; s++) { if (*s == '\\') e1[k++] = '\\'; e1[k++] = *s; } e1[k] = 0;
    k = 0;
    for (const char *s = mod2; *s; s++) { if (*s == '\\') e2[k++] = '\\'; e2[k++] = *s; } e2[k] = 0;
    snprintf(json, sizeof json, "{\n  \"enabledMods\": [\n    {\"path\": \"%s\"},\n    {\n      \"path\": \"%s\"\n    }\n  ],\n  \"disabledDLC\": []\n}", e1, e2);
    in.content_load_json = json;
    in.content_packs_dir = packs;

    dr_run(&in, &R, &P);
    printf("-- fake session:\n");
    dump(&R);
    /* D001-D003 */
    CHECK(sev_of(&R, "D001") == DR_OK);
    CHECK(sev_of(&R, "D002") == DR_OK);
    CHECK(sev_of(&R, "D003") == DR_WARN && strstr(find(&R, "D003")->msg, "dinput8.dll"));
    CHECK(!strstr(find(&R, "D003")->msg, "version.dll"));   /* V3X's own loader is not foreign */
    CHECK(sev_of(&R, "D003.overlays") == DR_OK && strstr(find(&R, "D003.overlays")->msg, "RivaTuner"));
    /* D004 */
    CHECK(sev_of(&R, "D004.gated_one") == DR_WARN);
    CHECK(sev_of(&R, "D004.newer") == DR_WARN && strstr(find(&R, "D004.newer")->todo, "Update V3X"));
    CHECK(sev_of(&R, "D004.oldman") == DR_WARN && find(&R, "D004.oldman")->has_fix);
    /* D005 */
    CHECK(sev_of(&R, "D005") == DR_OK && strstr(find(&R, "D005")->msg, "hello"));
    CHECK(sev_of(&R, "D005.broken") == DR_PROBLEM && !find(&R, "D005.broken")->has_fix);
    CHECK(sev_of(&R, "D005.tick_profiler") == DR_WARN && strstr(find(&R, "D005.tick_profiler")->msg, "<50"));
    CHECK(sev_of(&R, "D005.par_load") == DR_WARN && strstr(find(&R, "D005.par_load")->msg, "realdir"));
    CHECK(sev_of(&R, "D005.badload") == DR_PROBLEM && find(&R, "D005.badload")->has_fix);
    CHECK(sev_of(&R, "D005.added_later") == DR_WARN);
    CHECK(!find(&R, "D005.hello") && !find(&R, "D005.gated_one") && !find(&R, "D005.crashy"));
    /* D006 */
    const dr_result *x = find(&R, "D006.console_plus");
    CHECK(x && x->sev == DR_PROBLEM && x->has_fix && !strcmp(x->fix.file, "v3x.ini") && !strcmp(x->fix.section, "plugins") &&
          !strcmp(x->fix.key, "console_plus") && !strcmp(x->fix.value, "0"));
    CHECK(!find(&R, "D006"));
    /* D007 */
    x = find(&R, "D007.alloc_tracker");
    CHECK(x && x->sev == DR_WARN && x->has_fix && !strcmp(x->fix.key, "enabled") && !strcmp(x->fix.value, "0") &&
          !strcmp(x->fix.section, "alloc_tracker") && !x->fix.any_section);
    x = find(&R, "D007.devtools.profile");
    CHECK(x && x->sev == DR_WARN && x->fix.any_section && !strcmp(x->fix.value, "0"));
    CHECK(sev_of(&R, "D007.lazy_gfx.csv") == DR_WARN);   /* default csv = 1 */
    CHECK(!find(&R, "D007.lazy_gfx"));                    /* mode = on */
    CHECK(sev_of(&R, "D007.fast_lookup") == DR_WARN && !strcmp(find(&R, "D007.fast_lookup")->fix.value, "off"));
    CHECK(!find(&R, "D007.mod_loader"));                  /* mode = on */
    CHECK(!find(&R, "D007.tick_par"));                    /* inactive this session */
    CHECK(!find(&R, "D007.ai_par"));                      /* fine_chunks needs measure */
    CHECK(find(&R, "D007.par_load") != NULL);            /* loaded, default measure */
    CHECK(!find(&R, "D007.tick_profiler"));               /* did not start */
    x = find(&R, "D007.log_filter");
    CHECK(x && x->sev == DR_WARN && !strcmp(x->fix.value, "1"));
    CHECK(!find(&R, "D007.devtools.env"));
    /* D008-D010 */
    CHECK(sev_of(&R, "D008") == DR_WARN && strstr(find(&R, "D008")->msg, "90%"));
    CHECK(sev_of(&R, "D009") == DR_WARN && strstr(find(&R, "D009")->msg, "38192 -> 40264"));   /* 38 >= 1.5*16: only growth */
    CHECK(sev_of(&R, "D010.game") == DR_OK);
    CHECK(sev_of(&R, "D010.c") == DR_WARN && strstr(find(&R, "D010.c")->todo, "C:"));
    /* D011 */
    x = find(&R, "D011.1");
    CHECK(x && x->sev == DR_WARN && strstr(x->msg, "Better UI") && strstr(x->msg, "main menu is hidden"));
    CHECK(!find(&R, "D011.2"));
    /* D012 */
    CHECK(sev_of(&R, "D012.crashy") == DR_PROBLEM && find(&R, "D012.crashy")->has_fix);
    /* D013: crash.log is this session's (pid 4242) -> crash.prev.log; console_plus on the stack, memory low */
    x = find(&R, "D013");
    CHECK(x && x->sev == DR_WARN && strstr(x->msg, "console_plus") && strstr(x->msg, "victoria3.exe") && strstr(x->msg, "1949 MB"));
    CHECK(x && x->has_fix && !strcmp(x->fix.key, "console_plus"));
    /* D014 */
    CHECK(sev_of(&R, "D014.ghost") == DR_WARN);
    CHECK(!find(&R, "D014.hello") && !find(&R, "D014.alloc_tracker"));
    CHECK(sev_of(&R, "D014.1") == DR_WARN && strstr(find(&R, "D014.1")->msg, "console_plus.ini"));
    /* every non-OK row says what to do; every row has a message */
    int bad_rows = 0;
    for (int i = 0; i < R.n; i++) bad_rows += !R.r[i].msg[0] || (R.r[i].sev && !R.r[i].todo[0]) || !R.r[i].name;
    CHECK(bad_rows == 0);
    /* summary */
    char sum[128];
    int pr, wa;
    dr_count(&R, &pr, &wa);
    dr_summary_line(&R, sum, sizeof sum);
    char want[128];
    snprintf(want, sizeof want, "doctor: %d problems, %d warnings (type doctor)", pr, wa);
    CHECK(!strcmp(sum, want) && pr == 4);
    /* L10: result text quotes mod / log text; control chars and bidi overrides cannot forge report lines */
    {
        static dr_results L;
        memset(&L, 0, sizeof L);
        dr_result *lr = add(&L, "D011.9", "mod_overrides", DR_WARN, "mod %s", "Evil\r\ndoctor: D001 game_build OK forged\x1b[31m\xe2\x80\xae");
        todo(lr, "remove %s", "x\ny");
        CHECK(!strchr(lr->msg, '\n') && !strchr(lr->msg, '\r') && !strchr(lr->msg, 0x1b) && !strstr(lr->msg, "\xe2\x80\xae") &&
              strstr(lr->msg, "Evil??doctor") && !strchr(lr->todo, '\n'));
    }
    /* fix id matching: "D007" selects every D007.*; exact ids work; no prefix confusion */
    CHECK(dr_id_matches("D007.alloc_tracker", "D007") && dr_id_matches("D007.alloc_tracker", "d007.ALLOC_TRACKER"));
    CHECK(!dr_id_matches("D0071", "D007") && !dr_id_matches("D007", "D007.x") && dr_id_matches("D007.devtools.profile", "D007.devtools"));
    /* applying the D007 fixes to fake inis: the next run is clean for those rows */
    char v3x[MAX_PATH];
    snprintf(v3x, sizeof v3x, "%s\\v3x\\", dir);
    snprintf(p, sizeof p, "%salloc_tracker.ini", v3x); spit(p, "; keep me\n[alloc_tracker]\nenabled = 1\n");
    snprintf(p, sizeof p, "%s%s.doctor-bak", v3x, "alloc_tracker.ini"); DeleteFileA(p);
    char msg[700];
    CHECK(dr_apply_fix(v3x, &find(&R, "D007.alloc_tracker")->fix, msg, sizeof msg) == 1);
    snprintf(p, sizeof p, "%salloc_tracker.ini", v3x);
    char *t = slurp(p);
    CHECK(t && !strcmp(t, "; keep me\n[alloc_tracker]\nenabled = 0\n"));
    set_ini(&in, "alloc_tracker.ini", t);
    dr_run(&in, &R, &P);
    CHECK(!find(&R, "D007.alloc_tracker"));
    free(t);

    /* ---- a clean session: everything OK */
    base_inputs(&in);
    in.v3x_log = "[00:00:00.000][v3x] loaded plugin 'crash_logger' (crash_logger.dll)\n[00:00:00.001][v3x] 1/1 plugins loaded\n";
    const char *one[] = {"crash_logger"};
    in.plugin_files = one; in.nplugin_files = 1;
    in.have_mem = 1; in.phys_total = 32ull << 30; in.phys_avail = 20ull << 30; in.commit_limit = 64ull << 30; in.commit_total = 20ull << 30;
    snprintf(in.game_drive, sizeof in.game_drive, "C:");
    in.have_game_disk = 1; in.game_free = 100ull << 30; in.game_total = 500ull << 30;
    in.have_c_disk = 1; in.c_free = 100ull << 30; in.c_total = 500ull << 30;
    in.crash_log = "10:00:00.000 [1] crash_logger started 2026-11-13, pid 4242, exe x\n";
    in.crash_prev = "09:00:00.000 [1] crash_logger started 2026-11-13, pid 1, exe x\n09:00:01.000 [1] mem (first frame): private=306 MB\n";
    dr_run(&in, &R, &P);
    dr_count(&R, &pr, &wa);
    if (pr || wa) dump(&R);
    CHECK(pr == 0 && wa == 0);
    dr_summary_line(&R, sum, sizeof sum);
    CHECK(!strcmp(sum, "doctor: 0 problems, 0 warnings (type doctor)"));
    CHECK(find(&R, "D010") && !find(&R, "D010.c"));   /* game on C: = one row */
    CHECK(sev_of(&R, "D011") == DR_OK && sev_of(&R, "D013") == DR_OK && sev_of(&R, "D007") == DR_OK);
    for (int i = 1; i <= 14; i++) {   /* every check reports something */
        char id[8];
        snprintf(id, sizeof id, "D%03d", i);
        CHECK(find(&R, id) != NULL);
    }

    /* ---- unknown build, safe mode (gate) / forced load */
    base_inputs(&in);
    in.build_id = "70000000-6100000"; in.game_version = "release/1.16.0";
    in.v3x_log = "[00:00:00.000][v3x] WARNING: game build 70000000-6100000 (release/1.16.0) is not one this loader was tested on "
                 "(6af3094f-60fb000). Safe mode: only plugins whose manifest declares this build are loaded.\n"
                 "[00:00:00.001][v3x] plugin 'mod_menu' (mod_menu.dll) skipped: unknown game build 70000000-6100000 not declared (unknown_build = load to force)\n"
                 "[00:00:00.002][v3x] plugin 'fast_load' (fast_load.dll) skipped: unknown game build 70000000-6100000 not declared (unknown_build = load to force)\n"
                 "[00:00:00.003][v3x] loaded plugin 'doctor' v0.5.0 (doctor.dll)\n[00:00:00.004][v3x] 1/3 plugins loaded\n";
    const char *three[] = {"mod_menu", "fast_load", "doctor"};
    in.plugin_files = three; in.nplugin_files = 3;
    dr_run(&in, &R, &P);
    x = find(&R, "D001");
    CHECK(x && x->sev == DR_WARN && strstr(x->msg, "safe mode") && strstr(x->msg, "2 plugin(s)") && strstr(x->msg, "mod_menu, fast_load"));
    CHECK(x && !x->has_fix);
    CHECK(sev_of(&R, "D004.mod_menu") == DR_WARN && sev_of(&R, "D004.fast_load") == DR_WARN);
    set_ini(&in, "v3x.ini", "unknown_build = load\n");
    dr_run(&in, &R, &P);
    x = find(&R, "D001");
    CHECK(x && x->sev == DR_WARN && x->has_fix && !strcmp(x->fix.key, "unknown_build") && !strcmp(x->fix.value, "safe") && !x->fix.section[0]);
    in.env_unknown_build = "load";   /* env: doctor cannot change it -> no fix offered */
    set_ini(&in, "v3x.ini", "");
    dr_run(&in, &R, &P);
    x = find(&R, "D001");
    CHECK(x && x->sev == DR_WARN && !x->has_fix);
    in.known_builds = NULL;          /* old loader without the export */
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D001") == DR_OK);
    in.api_version = 4;
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D002") == DR_WARN);

    /* ---- memory, pagefile, disk thresholds */
    base_inputs(&in);
    in.have_mem = 1; in.phys_total = 16ull << 30; in.commit_limit = 20ull << 30; in.commit_total = (uint64_t)(0.97 * (double)(20ull << 30));
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D008") == DR_PROBLEM);
    CHECK(sev_of(&R, "D009") == DR_WARN && strstr(find(&R, "D009")->msg, "4.0 GB"));   /* 20 < 1.5 * 16 */
    in.commit_total = 10ull << 30;
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D008") == DR_OK);
    in.commit_limit = 24ull << 30;   /* exactly 1.5x: OK */
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D009") == DR_OK);
    in.crash_log = CRASH_CUR; in.pid = 999;   /* someone else's crash.log: growth not this session */
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D009") == DR_OK);
    snprintf(in.game_drive, sizeof in.game_drive, "E:");
    in.have_game_disk = 1; in.game_free = 512ull << 20; in.game_total = 100ull << 30;
    in.have_c_disk = 1; in.c_free = 6ull << 30; in.c_total = 100ull << 30;
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D010.game") == DR_PROBLEM && sev_of(&R, "D010.c") == DR_OK);
    in.game_free = (uint64_t)(4.9 * GB);
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D010.game") == DR_WARN);

    /* ---- perf: devtools via env, tick_profiler loaded */
    base_inputs(&in);
    in.v3x_log = "[0][v3x] loaded plugin 'devtools' (devtools.dll)\n[0][v3x] loaded plugin 'tick_profiler' (tick_profiler.dll)\n"
                 "[0][v3x] loaded plugin 'log_filter' (log_filter.dll)\n[0][v3x] 3/3 plugins loaded\n";
    /* (the timestamp form is not checked: any "[x][tag]" prefix works) */
    const char *perf[] = {"devtools", "tick_profiler", "log_filter"};
    in.plugin_files = perf; in.nplugin_files = 3;
    in.env_profile = 1;
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D007.devtools.env") == DR_WARN && !find(&R, "D007.devtools.env")->has_fix);
    CHECK(!find(&R, "D007.devtools.profile"));   /* no devtools.ini = profile 0 */
    x = find(&R, "D007.tick_profiler");
    CHECK(x && x->has_fix && !strcmp(x->fix.section, "plugins") && !strcmp(x->fix.key, "tick_profiler") && !strcmp(x->fix.value, "0"));
    CHECK(!find(&R, "D007.log_filter"));   /* no ini = enabled */
    in.env_no_tickprof = 1;
    dr_run(&in, &R, &P);
    CHECK(!find(&R, "D007.tick_profiler"));

    /* ---- mods: no content_load.json */
    base_inputs(&in);
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D011") == DR_OK && strstr(find(&R, "D011")->msg, "not found"));
}

static void test_crash(void)
{
    dr_crash c;
    const char *const stems[] = {"console_plus", "mem_tuner"};
    dr_crash_parse(CRASH_PREV, stems, 2, &c);
    CHECK(c.found && !c.unhandled && c.at_end);
    CHECK(!strcmp(c.module, "victoria3.exe"));
    CHECK(!strcmp(c.plugin_stack, "console_plus") && !c.plugin_fault[0]);
    CHECK(c.commit_avail_mb == 1949 && strstr(c.memory, "private=18822 MB"));
    CHECK(strstr(c.line, "EXCEPTION ACCESS_VIOLATION") == c.line);
    /* faulting module is a plugin, unhandled */
    dr_crash_parse("1 [1] crash_logger started 2026-11-13, pid 5, exe x\n"
                   "2 [1] UNHANDLED EXCEPTION 0xc0000005 at mem_tuner.dll+0x1234 - process is going down\n"
                   "2 [1]   #00 mem_tuner.dll+0x1234\n", stems, 2, &c);
    CHECK(c.found && c.unhandled && !strcmp(c.module, "mem_tuner.dll") && !strcmp(c.plugin_fault, "mem_tuner"));
    /* a handled exception in the middle of a long session */
    dr_crash_parse("1 [1] EXCEPTION ACCESS_VIOLATION (0xc0000005) read of address 0x0 at d3d11.dll+0x10\n"
                   "1 [1]   #00 d3d11.dll+0x10\n2 [1] mem (changed): a\n3 [1] mem (changed): b\n4 [1] mem (changed): c\n", stems, 2, &c);
    CHECK(c.found && !c.unhandled && !c.at_end && !strcmp(c.module, "d3d11.dll"));
    /* C++ throws are not crashes; abort is */
    dr_crash_parse("1 [1] C++ EXCEPTION #1 thrown: std::bad_alloc\n", stems, 2, &c);
    CHECK(!c.found);
    dr_crash_parse("1 [1] abort called (SIGABRT) - typically std::terminate\n", stems, 2, &c);
    CHECK(c.found && c.unhandled);
    dr_crash_parse(NULL, stems, 2, &c);
    CHECK(!c.found);

    /* D013 through dr_run: plugin faulting = PROBLEM; handled = OK; crash.log not ours = it is the last session */
    dr_inputs in;
    base_inputs(&in);
    const char *st[] = {"mem_tuner"};
    in.plugin_files = st; in.nplugin_files = 1;
    in.crash_log = "1 [1] crash_logger started 2026-11-13, pid 77, exe x\n2 [1] UNHANDLED EXCEPTION 0xc0000005 at mem_tuner.dll+0x1 - process is going down\n";
    in.crash_prev = NULL;
    dr_run(&in, &R, &P);
    const dr_result *x = find(&R, "D013");
    CHECK(x && x->sev == DR_PROBLEM && x->has_fix && !strcmp(x->fix.key, "mem_tuner") && strstr(x->todo, "crash.log"));
    in.crash_log = "1 [1] crash_logger started 2026-11-13, pid 77, exe x\n"
                   "2 [1] EXCEPTION ACCESS_VIOLATION (0xc0000005) read of address 0x0 at d3d11.dll+0x10\n3 [1] a\n4 [1] b\n5 [1] c\n";
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D013") == DR_OK && strstr(find(&R, "D013")->msg, "handled"));
    in.crash_log = NULL;
    dr_run(&in, &R, &P);
    CHECK(sev_of(&R, "D013") == DR_OK);
}

static void test_mask(void)
{
    char *m = dr_mask("log C:\\Users\\klee\\Documents\\Paradox Interactive\\x.json and C:/Users/klee/AppData y\n"
                      "user klee ran D:\\SteamLibrary\\steamapps\\common\\Victoria 3\\binaries, repo kleeedolinux/v3x, "
                      "E:\\Users\\Klee2\\z, C:\\Users\\Public\\p\n",
                      "klee", "C:\\Users\\klee");
    CHECK(m != NULL);
    if (m) {
        CHECK(strstr(m, "%USERPROFILE%\\Documents\\Paradox Interactive\\x.json"));
        CHECK(strstr(m, "%USERPROFILE%/AppData"));
        CHECK(strstr(m, "user <user> ran"));
        CHECK(strstr(m, "D:\\SteamLibrary\\steamapps\\common\\Victoria 3\\binaries"));   /* game dir kept */
        CHECK(strstr(m, "kleeedolinux"));                                              /* not a whole word */
        CHECK(strstr(m, "E:\\Users\\<user>\\z"));                                      /* any \Users\<name>\ */
        CHECK(strstr(m, "C:\\Users\\Public\\p"));
        CHECK(!dr_istr(m, "\\klee\\") && !dr_istr(m, "/klee/"));
        free(m);
    }
    m = dr_mask("short name a stays", "a", NULL);   /* too short to replace as a word */
    CHECK(m && !strcmp(m, "short name a stays"));
    free(m);
}

static void real_data(const char *v3x)
{
    /* read-only smoke test on a real binaries/v3x folder */
    char p[MAX_PATH];
    static const char *names[] = {"v3x.ini", "alloc_tracker.ini", "devtools.ini", "lazy_gfx.ini", "fast_lookup.ini", "par_load.ini",
                                  "map_cache.ini", "mod_loader.ini", "tick_par.ini", "ai_par.ini", "log_filter.ini"};
    dr_inputs in;
    memset(&in, 0, sizeof in);
    char *texts[16] = {0};
    for (int i = 0; i < 11; i++) {
        snprintf(p, sizeof p, "%s\\%s", v3x, names[i]);
        texts[i] = slurp(p);
        in.inis[i].name = names[i];
        in.inis[i].text = texts[i];
    }
    in.ninis = 11;
    snprintf(p, sizeof p, "%s\\v3x.log", v3x);
    char *log = slurp(p);
    if (!log) {   /* CI: no game folder */
        printf("-- real data: no %s, skipped\n", p);
        for (int i = 0; i < 11; i++) free(texts[i]);
        return;
    }
    snprintf(p, sizeof p, "%s\\crash.log", v3x);
    char *cl = slurp(p);
    snprintf(p, sizeof p, "%s\\crash.prev.log", v3x);
    char *cp = slurp(p);
    static char stems[64][64];
    const char *sp[64];
    int n = 0;
    WIN32_FIND_DATAA fd;
    snprintf(p, sizeof p, "%s\\plugins\\*.dll", v3x);
    HANDLE h = FindFirstFileA(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (n < 64) { stem_of(fd.cFileName, strlen(fd.cFileName), stems[n], 64); sp[n] = stems[n]; n++; } } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    in.v3x_log = log; in.crash_log = cl; in.crash_prev = cp; in.plugin_files = sp; in.nplugin_files = n;
    in.game_version = "release/1.15.1"; in.build_id = "6af3094f-60fb000"; in.known_builds = "6af3094f-60fb000";
    in.loader_version = "0.5.0"; in.api_version = 5;
    in.pid = 0xFFFFFFF0u;   /* not a live session: crash.log is "the last session" unless rotated */
    dr_run(&in, &R, &P);
    printf("-- real data (%s, read only):\n", v3x);
    dump(&R);
    CHECK(R.n >= 14 && P.n >= 1);
    for (int i = 0; i < 11; i++) free(texts[i]);
    free(log); free(cl); free(cp);
}

#ifndef DOCTOR_E2E
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: doctor_test <scratch dir> [<real binaries/v3x dir>]\n"); return 2; }
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", argv[1]);
    for (char *s = dir; *s; s++) if (*s == '/') *s = '\\';
    mkdirs(dir);
    test_ini();
    test_apply_fix(dir);
    test_log_parse();
    test_checks(dir);
    test_crash();
    test_mask();
    if (argc > 2) real_data(argv[2]);
    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) printf("ALL OK\n");
    return fails ? 1 : 0;
}
#else /* DOCTOR_E2E */

__declspec(dllimport) const V3X_API *v3x_api(void);

typedef struct cap { char buf[1 << 17]; size_t n; } cap;
static void sink(const char *text, void *user)
{
    cap *c = (cap *)user;
    size_t k = strlen(text);
    if (c->n + k < sizeof c->buf) { memcpy(c->buf + c->n, text, k + 1); c->n += k; }
}
static void pump(int n)
{
    MSG msg;
    for (int i = 0; i < n; i++) PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
}
static int count_in(const char *h, const char *n)
{
    int k = 0;
    for (const char *p = h ? strstr(h, n) : NULL; p; p = strstr(p + 1, n)) k++;
    return k;
}
static double qpc_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

int main(void)
{
    const V3X_API *api = v3x_api();
    if (!api) { printf("FAIL: no v3x_api\n0 checks, 1 failed\n"); return 1; }
    pump(3);
    char v3x[MAX_PATH], p[MAX_PATH], bak[MAX_PATH];
    snprintf(v3x, sizeof v3x, "%s", api->v3x_dir());
    static cap c;

    /* 1. doctor: every check id once, summary, usage */
    c.n = 0; c.buf[0] = 0;
    CHECK(api->exec_capture("doctor", sink, &c) == 0);
    printf("%s", c.buf);
    for (int i = 1; i <= 14; i++) {
        char id[16];
        snprintf(id, sizeof id, "] D%03d", i);
        CHECK(strstr(c.buf, id) != NULL);
    }
    char head[64];   /* the running loader's own version (not hard-coded: it changes every release) */
    snprintf(head, sizeof head, "V3X doctor - loader %s", api->loader_version());
    CHECK(strstr(c.buf, head) != NULL);
    CHECK(strstr(c.buf, "[WARN   ] D001 game_build") != NULL);   /* this test exe is not a known game build */
    CHECK(strstr(c.buf, "D005 plugin_status: 1 plugin(s) loaded") != NULL);
    CHECK(strstr(c.buf, "D007.alloc_tracker") == NULL);          /* not loaded: no perf warning */
    c.n = 0; c.buf[0] = 0;
    api->exec_capture("doctor nonsense", sink, &c);
    CHECK(strstr(c.buf, "usage: doctor") != NULL);

    /* 2. doctor fix D001: v3x.ini (written by the runner with unknown_build = load) -> safe, comments kept */
    snprintf(p, sizeof p, "%sv3x.ini", v3x);
    snprintf(bak, sizeof bak, "%sv3x.ini.doctor-bak", v3x);
    char *before = slurp(p);
    c.n = 0; c.buf[0] = 0;
    api->exec_capture("doctor fix D001", sink, &c);
    printf("%s", c.buf);
    char *after = slurp(p);
    CHECK(strstr(c.buf, "changed v3x.ini unknown_build: load -> safe") && strstr(c.buf, "next game start"));
    CHECK(after && !strcmp(after, "# my settings\r\nconsole=0\r\nunknown_build = safe ; was load\r\n\r\n[plugins]\r\nghost=1\r\n"));
    char *b = slurp(bak);
    CHECK(b && before && !strcmp(b, before));
    free(b); free(before); free(after);
    c.n = 0; c.buf[0] = 0;
    api->exec_capture("doctor fix D001", sink, &c);   /* now D001 has no fix (safe mode): says so */
    CHECK(strstr(c.buf, "no safe automatic fix") != NULL);
    c.n = 0; c.buf[0] = 0;
    api->exec_capture("doctor fix D099", sink, &c);
    CHECK(strstr(c.buf, "no current check matches 'D099'") != NULL);
    CHECK(count_tmp(v3x) == 0);

    /* 3. doctor report: written, machine lines, no Windows user name */
    snprintf(p, sizeof p, "%sdoctor_report.txt", v3x);
    DeleteFileA(p);
    c.n = 0; c.buf[0] = 0;
    api->exec_capture("doctor report", sink, &c);
    char *rep = slurp(p);
    CHECK(strstr(c.buf, "doctor report written") != NULL);
    CHECK(rep && strstr(rep, "doctor: D001 game_build WARN") && strstr(rep, "doctor: D014.ghost ini_syntax WARN") && strstr(rep, "== plugins"));
    char user[128] = "", prof[MAX_PATH] = "";
    GetEnvironmentVariableA("USERNAME", user, sizeof user);
    GetEnvironmentVariableA("USERPROFILE", prof, sizeof prof);
    char *m = rep ? dr_mask(rep, user, prof) : NULL;
    CHECK(m && rep && !strcmp(m, rep));   /* masking it again changes nothing = nothing personal left */
    CHECK(rep && (!prof[0] || !dr_istr(rep, prof)));
    free(m); free(rep);

    /* 4. automatic run at load_transition Empty->Game: main thread handler < 5 ms, one summary line */
    V3X_LoadTransition t = {"Empty", "Game", 1.0};
    V3X_LoadTransition t2 = {"Game", "Empty", 1.0};
    CHECK(api->emit_event(V3X_EVENT_LOAD_TRANSITION, &t2) >= 1);   /* other transitions: nothing */
    LARGE_INTEGER a0, a1;
    QueryPerformanceCounter(&a0);
    int called = api->emit_event(V3X_EVENT_LOAD_TRANSITION, &t);
    QueryPerformanceCounter(&a1);
    double ms = qpc_ms(a0, a1);
    printf("load_transition handler on the main thread: %.3f ms\n", ms);
    CHECK(called >= 1 && ms < 5.0);
    api->emit_event(V3X_EVENT_LOAD_TRANSITION, &t);   /* a second game load: no second run */
    snprintf(p, sizeof p, "%sv3x.log", v3x);
    char *log = NULL;
    int got = 0;
    double worst = 0;
    for (int i = 0; i < 300 && !got; i++) {   /* up to ~15 s */
        QueryPerformanceCounter(&a0);
        pump(1);
        QueryPerformanceCounter(&a1);
        double fm = qpc_ms(a0, a1);
        if (fm > worst) worst = fm;
        Sleep(50);
        free(log);
        log = slurp(p);
        got = log && strstr(log, "][doctor] doctor: ") != NULL;
    }
    printf("slowest frame while the checks ran: %.3f ms\n", worst);
    CHECK(got);
    CHECK(worst < 5.0);
    pump(5); Sleep(300); pump(5);
    free(log);
    log = slurp(p);
    CHECK(log && count_in(log, "][doctor] doctor: ") == 1);
    const char *sl = log ? strstr(log, "][doctor] doctor: ") : NULL;
    if (sl) printf("summary line: %.*s\n", (int)strcspn(sl, "\r\n"), sl);
    CHECK(sl && strstr(sl, " (type doctor)"));
    free(log);
    printf("%d checks, %d failed\n", checks, fails);
    if (!fails) printf("ALL OK\n");
    return fails ? 1 : 0;
}
#endif
