/* Fuzz harness: plugins/console_plus/cp_logic.h - the V3X `help` listing parser (cp_parse_help), the
 * line completer (cp_complete_line, candidates = the parsed command names / sub-words, so mutated help
 * text drives completion of mutated lines), the argument-kind classifier and console_plus.ini.
 * Invariants: every name / sub-word is NUL-terminated within its slot; the completed line is a
 * NUL-terminated string < 1024; idx entries index the candidate array; a unique match ends in ' '. */
#include "v3x_fuzz.h"
#include "../../plugins/console_plus/cp_logic.h"

static CpHelp g_h;

static int cands(int argi, const char *cmd, const char *const **out, void *user)
{
    (void)user;
    if (argi == 0) { *out = g_h.names; return g_h.n; }
    int i = cp_help_find(&g_h, cmd);
    if (i < 0) return 0;
    *out = g_h.subs[i];
    return g_h.nsub[i];
}

static void fuzz_help(const uint8_t *d, size_t n)
{
    /* input = help text, then a 0x01 byte, then the line to complete */
    const char *t = v3x_fuzz_cstr(d, n);
    const char *sep = strchr(t, 0x01);
    char help[8192];
    size_t hl = sep ? (size_t)(sep - t) : strlen(t);
    if (hl >= sizeof help) hl = sizeof help - 1;
    memcpy(help, t, hl); help[hl] = 0;
    cp_parse_help(help, &g_h);
    V3X_FUZZ_ASSERT(g_h.n >= 0 && g_h.n <= CP_HELP_MAX);
    for (int i = 0; i < g_h.n; i++) {
        V3X_FUZZ_ASSERT(memchr(g_h.name[i], 0, sizeof g_h.name[i]) && g_h.names[i] == g_h.name[i] && g_h.name[i][0]);
        V3X_FUZZ_ASSERT(g_h.nsub[i] >= 0 && g_h.nsub[i] <= CP_SUB_MAX);
        for (int k = 0; k < g_h.nsub[i]; k++) V3X_FUZZ_ASSERT(memchr(g_h.sub[i][k], 0, sizeof g_h.sub[i][k]) != NULL);
    }
    const char *line = sep ? sep + 1 : "";
    static CpResult r;
    cp_complete_line(line, cands, NULL, &r);
    V3X_FUZZ_ASSERT(memchr(r.line, 0, sizeof r.line) != NULL);
    V3X_FUZZ_ASSERT(r.nmatch >= 0);
    int shown = r.nmatch < 64 ? r.nmatch : 64;
    for (int i = 0; i < shown; i++) V3X_FUZZ_ASSERT(r.idx[i] >= 0 && r.cands && strlen(r.cands[r.idx[i]]) < 32);
    if (r.nmatch == 1 && r.changed) { size_t L = strlen(r.line); V3X_FUZZ_ASSERT(L == 0 || r.line[L - 1] == ' ' || L == sizeof r.line - 1); }
    (void)cp_arg_index(line);
}
V3X_FUZZ_TARGET(cp_help, fuzz_help)
V3X_FUZZ_SEED(cp_help, "V3X console commands:\n  help                  list commands\n  profile               profile on|off|report|reset - devtools\n"
                       "  ticks                 tick task profile; 'ticks reset' clears it\n  doctor  health check: doctor | doctor fix <ID> | doctor report\n\x01pro")
V3X_FUZZ_SEED(cp_help, "  a  x|y|z\n  ab  <placeholder>|q\n  abc\r\n\x01" "ab q")
V3X_FUZZ_SEED(cp_help, "  lazymesh  status | dump (all) | load <type>\n  lean  x\n  log  level\n\x01l")

static void fuzz_ini(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    static CpCfg c;
    cp_cfg_defaults(&c);
    cp_parse_ini(t, &c);
    V3X_FUZZ_ASSERT(c.novr >= 0 && c.novr <= CP_OVR_MAX);
    for (int i = 0; i < c.novr; i++) {
        V3X_FUZZ_ASSERT(memchr(c.ovr[i].cmd, 0, sizeof c.ovr[i].cmd) != NULL && c.ovr[i].nkinds >= 0 && c.ovr[i].nkinds <= CP_MAX_ARGS);
        for (int k = 0; k < c.ovr[i].nkinds; k++) V3X_FUZZ_ASSERT(c.ovr[i].kinds[k] >= 0 && c.ovr[i].kinds[k] < CK_COUNT);
    }
}
V3X_FUZZ_TARGET(cp_ini, fuzz_ini)
V3X_FUZZ_SEED(cp_ini, "[console_plus]\nenabled = 1\nnames = 0\n[commands]\nannex = country\nchange_law = country law\nx = nonsense kind kind kind kind kind kind\n")
