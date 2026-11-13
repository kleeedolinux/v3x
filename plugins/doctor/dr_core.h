/* dr_core.h - the in-game doctor's checks, as pure functions over collected inputs (docs/doctor.md).
 *
 * doctor.c collects the inputs (v3x.log text, ini texts, memory/disk numbers, module list, crash logs,
 * content_load.json) and calls dr_run(); every check below only reads dr_inputs, so test/doctor_test.c
 * runs them offline on fake data. The only file system access here is the mod-override check (it walks
 * <v3x>/content_packs and tests files inside the enabled mods) and dr_apply_fix (atomic ini edit).
 *
 * Check IDs are shared with the offline doctor (V3X-Setup.exe --doctor, chat #4423/#4426):
 *   D001 game_build      D002 loader_files   D003 foreign_proxy  D004 plugin_manifests
 *   D005 plugin_status   D006 plugin_faults  D007 perf_settings  D008 memory_commit
 *   D009 pagefile        D010 disk_free      D011 mod_overrides  D012 crash_loop
 *   D013 last_crash      D014 ini_syntax     (D015 game_running: offline only)
 * Per-plugin rows add ".<name>" (D005.par_load, D007.alloc_tracker); `doctor fix D007` = every D007.* fix. */
#ifndef DR_CORE_H
#define DR_CORE_H
#include <stddef.h>
#include <stdint.h>

#define DR_OK 0
#define DR_WARN 1
#define DR_PROBLEM 2

#define DR_MAX_RESULTS 96
#define DR_MAX_PLUGINS 64
#define DR_MAX_INIS 24

/* A safe fix: set `key = value` in <v3x>/<file> (section "" = top of the file). any_section: the
 * plugin's own parser ignores [sections], so every occurrence in the file is replaced. */
typedef struct dr_fix {
    char file[48], section[32], key[48], value[32];
    int any_section;
} dr_fix;

typedef struct dr_result {
    char id[48];          /* "D007.alloc_tracker" */
    const char *name;     /* "perf_settings" */
    int sev;              /* DR_OK / DR_WARN / DR_PROBLEM */
    char msg[400];        /* one plain line: what is wrong (or right) */
    char todo[400];       /* one line: what to do ("" for OK) */
    int has_fix;
    dr_fix fix;
} dr_result;

typedef struct dr_results {
    dr_result r[DR_MAX_RESULTS];
    int n;
} dr_results;

/* load status of one plugin file, from v3x.log (or the `plugins` command output) */
enum {
    DR_PS_UNKNOWN = 0,   /* DLL in plugins/ but no load record */
    DR_PS_LOADED,
    DR_PS_DISABLED_INI,  /* [plugins] x = 0 in v3x.ini */
    DR_PS_GATED,         /* skipped by the unknown-build gate */
    DR_PS_REFUSED_API,   /* manifest min_api > loader */
    DR_PS_REFUSED_BUILD, /* manifest REQUIRE_BUILD and the build is not listed */
    DR_PS_FAILED_RC,     /* v3x_plugin_load returned non-zero (fail closed, rolled back) */
    DR_PS_LOAD_ERROR,    /* LoadLibrary failed / no export / bad manifest */
    DR_PS_FAULT_LOAD,    /* faulted inside v3x_plugin_load (rolled back) */
    DR_PS_CRASH_SKIP     /* crashed the game while loading last start: skipped this start */
};

typedef struct dr_plugin {
    char stem[64];        /* file name without .dll */
    char name[64];        /* plugin / manifest name ("" = same as stem) */
    char version[24];
    int state;
    char why[240];        /* refusal / failure reason */
    int file_present;
    int faulted;          /* FAULT at runtime this session */
    char fault[300];
    int anchor_fail;
    char anchor[240];
    int manifest_warn;    /* loaded although its manifest does not list this build */
    int inactive;         /* the plugin logged that it does nothing (mode=off, env off, inactive) */
    const char *rc_pos;   /* position of the "returned N" line in the log (for why) */
} dr_plugin;

typedef struct dr_plugins {
    dr_plugin p[DR_MAX_PLUGINS];
    int n;
    int log_summary;      /* the core's "N/M plugins loaded" line was seen */
    int gate_active;      /* "Safe mode" unknown-build warning */
    int gate_forced;      /* "unknown_build = load: loading every plugin anyway" */
    char ignored[DR_MAX_INIS][120];   /* "<plugin>.ini: N lines ignored" style lines */
    int nignored;
} dr_plugins;

typedef struct dr_file {
    const char *name;     /* "v3x.ini", "alloc_tracker.ini", ... */
    const char *text;     /* NULL = file missing */
} dr_file;

typedef struct dr_inputs {
    const char *game_version, *build_id, *known_builds, *loader_version;
    int api_version;
    const char *v3x_log;          /* this session's v3x.log */
    const char *plugins_out;      /* output of the `plugins` command, or NULL */
    const char *const *plugin_files;   /* stems of the DLLs in v3x/plugins */
    int nplugin_files;
    dr_file inis[DR_MAX_INIS];
    int ninis;
    int env_profile, env_no_tickprof;  /* V3X_PROFILE=1, V3X_NO_TICKPROF=1 */
    const char *env_unknown_build;     /* V3X_UNKNOWN_BUILD or NULL */
    /* memory, bytes (have_mem = 0: not measured) */
    int have_mem;
    uint64_t private_bytes, commit_total, commit_limit, phys_total, phys_avail;
    /* disk, bytes */
    char game_drive[8];           /* "D:" */
    int have_game_disk, have_c_disk;
    uint64_t game_free, game_total, c_free, c_total;
    /* mods */
    const char *content_load_json;     /* NULL = not found */
    const char *content_packs_dir;     /* "<v3x>\content_packs" */
    /* process modules (full paths) */
    const char *const *modules;
    int nmodules;
    const char *exe_dir;          /* "<game>\binaries\" */
    const char *loader_path;      /* full path of the V3X version.dll */
    /* crash logs */
    const char *crash_log, *crash_prev;
    unsigned long pid;
} dr_inputs;

/* ---- the checks */
void dr_parse_log(const char *log, dr_plugins *ps);
void dr_parse_plugins_cmd(const char *out, dr_plugins *ps);
void dr_run(const dr_inputs *in, dr_results *out, dr_plugins *ps);
void dr_count(const dr_results *r, int *problems, int *warnings);
/* "doctor: 0 problems, 2 warnings (type doctor)" */
void dr_summary_line(const dr_results *r, char *out, size_t n);
/* one result as console text: "[WARN] D007.alloc_tracker perf_settings: ...\n       what to do: ...\n" */
size_t dr_format_result(const dr_result *r, char *out, size_t n);
/* results whose id is `id` or starts with "id." */
int dr_id_matches(const char *rid, const char *id);

/* ---- last-session crash summary (crash.log / crash.prev.log of crash_logger) */
typedef struct dr_crash {
    int found;            /* a fatal exception / abort line exists */
    int unhandled;        /* UNHANDLED EXCEPTION or abort: certainly the crash */
    int at_end;           /* the exception block ends the file: the session died there */
    char line[240];       /* the exception line (without timestamp) */
    char module[64];      /* faulting module ("victoria3.exe") */
    char plugin_fault[64];/* faulting module is this V3X plugin (stem) */
    char plugin_stack[64];/* first V3X plugin on the crash stack (stem) */
    char memory[200];     /* "memory: private=..." line after the exception */
    long long commit_avail_mb;   /* from that line, -1 = unknown */
} dr_crash;
void dr_crash_parse(const char *text, const char *const *plugin_stems, int nstems, dr_crash *c);

/* ---- ini helpers (comment-preserving) */
/* Value of key in [section] ("" = before the first [section]; any_section = anywhere, last wins).
 * Returns 1 if found. */
int dr_ini_get(const char *text, const char *section, const char *key, int any_section, char *out, size_t n);
/* Sets key = value: every matching line in scope gets the new value (leading space, key spelling,
 * spacing around '=', inline comment and line ending kept); if none, a "key = value" line is added at
 * the end of the section (the section is appended when missing). Returns 1 changed (*out = malloc'd
 * new text), 0 nothing to change (already that value), -1 out of memory. old = the first previous
 * value ("" if the key was not set). */
int dr_ini_set(const char *text, const char *section, const char *key, const char *value, int any_section,
               char **out, char *old, size_t oldn);
/* Applies f to <v3x_dir><f->file>: read, dr_ini_set, keep the first original as <file>.doctor-bak,
 * write atomically (temp + rename). Never deletes anything. msg = what changed and how to undo.
 * Returns 1 changed, 0 already set, -1 error. */
int dr_apply_fix(const char *v3x_dir, const dr_fix *f, char *msg, size_t n);

/* ---- report privacy: replaces the user profile path and the Windows user name ("<user>") */
char *dr_mask(const char *text, const char *user, const char *profile);   /* malloc'd */

/* ---- small text helpers (exported for the test) */
int dr_truthy(const char *v);
int dr_list_has(const char *list, const char *item);   /* ';'-separated, case-insensitive */
#endif
