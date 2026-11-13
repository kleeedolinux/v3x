/* console_plus: game-independent completion logic (no engine access, no Windows calls).
 * Included by console_plus.c and by test/console_plus_test.c.
 *
 *  - argument kinds: what an argument of an in-game console command names (country tag, state
 *    region, law, ...), derived from the engine's own argument descriptions ("country_TAG",
 *    "<country tag/id>", "law key, country tag (optional)", "<building key> <pm key> <state
 *    region key>") or from [commands] overrides in console_plus.ini
 *  - cp_arg_index: which argument the engine is completing (it passes the text between the
 *    command name and the token being typed)
 *  - cp_complete_*: prefix completion for the V3X console window (case-insensitive, longest
 *    common prefix, list of candidates when ambiguous)
 *  - cp_parse_help: command names + "a|b|c" sub-command words from the V3X `help` output
 *  - cp_parse_ini: console_plus.ini */
#ifndef CP_LOGIC_H
#define CP_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ argument kinds */
typedef enum {
    CK_NONE = 0,
    CK_COUNTRY, CK_STATE_REGION, CK_STRATEGIC_REGION, CK_LAW, CK_GOODS, CK_JOURNAL, CK_BUILDING,
    CK_PRODUCTION_METHOD, CK_CULTURE, CK_RELIGION, CK_IDEOLOGY, CK_POP_TYPE, CK_DECREE, CK_TECH,
    CK_MOVEMENT, CK_COUNTRY_TYPE, CK_WAR_GOAL, CK_COMPANY_TYPE, CK_SOCIAL_HIERARCHY, CK_STRAIT,
    CK_IG_TYPE, CK_SUBGOAL,
    CK_EVENT,      /* not a CGameDatabase: delegates to the engine's own `event` completer */
    CK_COUNT
} CpKind;

/* name in console_plus.ini, engine database class (CGameDatabase<db, T> with keys at T+0x18) */
static const struct { const char *name; const char *db; } CP_KINDS[CK_COUNT] = {
    {"none", 0},
    {"country", "CCountryDefinitionDatabase"},
    {"state_region", "CStateRegionTemplateDatabase"},
    {"strategic_region", "CStrategicRegionsDatabase"},
    {"law", "CLawTypeDatabase"},
    {"goods", "CGoodsDatabase"},
    {"journal", "CJournalEntryTypeDataBase"},
    {"building", "CBuildingDatabase"},
    {"production_method", "CProductionMethodDataBase"},
    {"culture", "CCultureTypeDataBase"},
    {"religion", "CReligionDataBase"},
    {"ideology", "CIdeologyDataBase"},
    {"pop_type", "CPopTypeDatabase"},
    {"decree", "CDecreeDatabase"},
    {"tech", "CTechnologyDatabase"},
    {"movement", "CPoliticalMovementTypeDatabase"},
    {"country_type", "CCountryTypeDatabase"},
    {"war_goal", "CWarGoalTypeDatabase"},
    {"company_type", "CCompanyTypeDatabase"},
    {"social_hierarchy", "CSocialHierarchyDatabase"},
    {"strait", "CStraitDefinitionDatabase"},
    {"ig_type", "CInterestGroupTypeDatabase"},
    {"subgoal", "CObjectiveSubgoalDatabase"},
    {"event", 0},
};

#define CP_MAX_ARGS 12

static int cp_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int cp_streqi(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (cp_lower((unsigned char)*a) != cp_lower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* case-insensitive: does s start with p? */
static int cp_prefixi(const char *s, const char *p)
{
    for (; *p; s++, p++) if (!*s || cp_lower((unsigned char)*s) != cp_lower((unsigned char)*p)) return 0;
    return 1;
}

/* case-insensitive substring of the first n chars of s */
static int cp_containsi(const char *s, size_t n, const char *w)
{
    size_t m = strlen(w);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && cp_lower((unsigned char)s[i + k]) == cp_lower((unsigned char)w[k])) k++;
        if (k == m) return 1;
    }
    return 0;
}

/* like cp_containsi, but the match must start a word (not preceded by a letter):
 * "tag" matches "country_TAG" and "all/tag" but not "stage"; "culture" not "agriculture" */
static int cp_hasword(const char *s, size_t n, const char *w)
{
    size_t m = strlen(w);
    for (size_t i = 0; i + m <= n; i++) {
        if (i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z'))) continue;
        size_t k = 0;
        while (k < m && cp_lower((unsigned char)s[i + k]) == cp_lower((unsigned char)w[k])) k++;
        if (k == m) return 1;
    }
    return 0;
}

static CpKind cp_kind_from_name(const char *name)
{
    for (int k = 0; k < CK_COUNT; k++) if (cp_streqi(name, CP_KINDS[k].name)) return (CpKind)k;
    return (CpKind)-1;
}

/* One argument (sub-)description -> kind. Order matters: "state region tag" is a state region,
 * not a country; "country ID" / "state ID" are numbers (CK_NONE). */
static CpKind cp_classify_one(const char *s, size_t n)
{
#define HAS(w) cp_hasword(s, n, w)
    if (HAS("event")) return CK_EVENT;
    if (HAS("strategic region")) return CK_STRATEGIC_REGION;
    if (HAS("state region") || HAS("state_region")) return CK_STATE_REGION;
    if (HAS("journal entry")) return CK_JOURNAL;
    if (HAS("pm key") || HAS("production method")) return CK_PRODUCTION_METHOD;
    if (HAS("company type")) return CK_COMPANY_TYPE;
    if (HAS("building key") || HAS("building type")) return CK_BUILDING;
    if (HAS("war goal")) return CK_WAR_GOAL;
    if (HAS("movement type")) return CK_MOVEMENT;
    if (HAS("social hierarchy")) return CK_SOCIAL_HIERARCHY;
    if (HAS("subgoal")) return CK_SUBGOAL;
    if (HAS("strait")) return CK_STRAIT;
    if (HAS("law")) return CK_LAW;
    if (HAS("goods")) return CK_GOODS;
    if (HAS("tech")) return CK_TECH;
    if (HAS("culture")) return CK_CULTURE;
    if (HAS("religion")) return CK_RELIGION;
    if (HAS("ideology")) return CK_IDEOLOGY;
    if (HAS("pop type")) return CK_POP_TYPE;
    if (HAS("decree")) return CK_DECREE;
    if (HAS("country type")) return CK_COUNTRY_TYPE;
    if (HAS("tag") || HAS("country definition")) return CK_COUNTRY;
    if (HAS("country") && !cp_containsi(s, n, " id") && !cp_containsi(s, n, "_id")) return CK_COUNTRY;
    return CK_NONE;
#undef HAS
}

/* Literal choices: "lines/ai/market", "all/tag", "<0/1>", "true/false", "<start|stop>",
 * "Failed Test Save: before/none/after". Words are kept in a shared pool. */
#define CP_POOL_WORDS 4096
typedef struct { char buf[1 << 16]; size_t used; const char *w[CP_POOL_WORDS]; int n; } CpPool;
typedef struct { CpKind kind; int lit0, nlit; } CpArg;   /* nlit words at pool->w[lit0..] */

static int cp_pool_add(CpPool *pl, const char *s, size_t n)
{
    if (!pl || pl->n >= CP_POOL_WORDS || pl->used + n + 1 > sizeof pl->buf) return -1;
    char *d = pl->buf + pl->used;
    memcpy(d, s, n); d[n] = 0;
    pl->used += n + 1;
    pl->w[pl->n] = d;
    return pl->n++;
}

static int cp_lit_word(const char *s, size_t n)
{
    if (!n || n > 23) return 0;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    /* placeholders, not choices */
    static const char *const ph[] = {"id", "key", "name", "type", "value", "amount", "number", "count"};
    for (size_t k = 0; k < sizeof ph / sizeof ph[0]; k++) {
        size_t m = strlen(ph[k]);
        if (n >= m && !memcmp(s + n - m, ph[k], m)) return 0;
    }
    return 1;
}

/* one sub-argument -> kind + literal words */
static CpArg cp_classify_sub(const char *s, size_t n, CpPool *pl)
{
    CpArg r = {CK_NONE, pl ? pl->n : 0, 0};
    size_t i0 = 0, alt = 0;
    for (size_t i = 0; i < n; i++) if (s[i] == '/' || s[i] == '|') { alt = i; break; }
    if (!alt) { r.kind = cp_classify_one(s, n); return r; }
    for (size_t i = 0; i < alt; i++) if (s[i] == ':') i0 = i + 1;          /* "Arguments: a/b" */
    size_t p = i0;
    while (p <= n) {
        size_t e = p; while (e < n && s[e] != '/' && s[e] != '|') e++;
        size_t a = p, b = e;
        while (a < b && (s[a] == ' ' || s[a] == '<' || s[a] == '[' || s[a] == '"')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '>' || s[b - 1] == ']' || s[b - 1] == '"')) b--;
        CpKind k = cp_classify_one(s + a, b - a);
        if (k != CK_NONE) { if (r.kind == CK_NONE) r.kind = k; }
        else if (cp_lit_word(s + a, b - a) && pl) { if (cp_pool_add(pl, s + a, b - a) >= 0) r.nlit++; }
        p = e + 1;
    }
    return r;
}

/* Split one engine argument description into sub-arguments and classify each.
 * Separators: ',' and "> <" (e.g. "<building key> <pm key> <state region key>").
 * A parenthesised remark "(optional, defaults to player)" is not split. Returns the count. */
static int cp_classify_arg(const char *desc, CpArg *out, int max, CpPool *pl)
{
    int n = 0, depth = 0;
    const char *a = desc;
    if (!desc || !*desc) return 0;
    for (const char *p = desc;; p++) {
        int cut = 0;
        if (*p == '(') depth++;
        else if (*p == ')' && depth) depth--;
        if (!*p) cut = 1;
        else if (!depth && *p == ',') cut = 1;
        else if (!depth && *p == '>' ) { const char *q = p + 1; while (*q == ' ') q++; if (*q == '<') cut = 2; }
        if (cut) {
            size_t len = (size_t)(p - a) + (cut == 2);
            int blank = 1;
            for (size_t i = 0; i < len; i++) if (a[i] != ' ' && a[i] != '\t') { blank = 0; break; }
            if (!blank && n < max) out[n++] = cp_classify_sub(a, len, pl);
            if (!*p) break;
            a = p + 1;
        }
    }
    return n;
}

/* All argument descriptions of a command (engine order) -> one CpArg per argument position. */
static int cp_classify_args(const char *const *descs, int ndescs, CpArg *out, int max, CpPool *pl)
{
    int n = 0;
    for (int i = 0; i < ndescs && n < max; i++) n += cp_classify_arg(descs[i], out + n, max - n, pl);
    return n;
}

static int cp_any_arg(const CpArg *a, int n)
{
    for (int i = 0; i < n; i++) if (a[i].kind != CK_NONE || a[i].nlit) return 1;
    return 0;
}

/* The engine passes the text between the command name and the token being completed
 * ("" for the first argument, "SOK" for the second of "own SOK STATE_..."). The index of the
 * argument being completed = number of tokens in it. */
static int cp_arg_index(const char *prev)
{
    int n = 0, in = 0;
    for (const char *p = prev ? prev : ""; *p; p++) {
        int sp = (*p == ' ' || *p == '\t');
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

/* ------------------------------------------------------------------ prefix completion (V3X console) */
/* Candidates matching `token` by case-insensitive prefix. idx[] receives their indexes (up to
 * maxidx), `common` the longest common prefix of all matches (letters as in the first match,
 * but never shorter than what was typed: the typed text is kept as typed). Returns the number
 * of matches. Exact duplicates (case-insensitive) count once. */
static int cp_match(const char *token, const char *const *cands, int ncands, int *idx, int maxidx,
                    char *common, size_t common_sz)
{
    int m = 0;
    size_t tl = strlen(token);
    if (common_sz) { snprintf(common, common_sz, "%s", token); }
    size_t cl = 0;
    const char *first = NULL;
    for (int i = 0; i < ncands; i++) {
        const char *c = cands[i];
        if (!c || !*c || !cp_prefixi(c, token)) continue;
        int dup = 0;
        for (int j = 0; j < m && j < maxidx; j++) if (cp_streqi(cands[idx[j]], c)) { dup = 1; break; }
        if (dup) continue;
        if (m < maxidx) idx[m] = i;
        if (!first) { first = c; cl = strlen(c); }
        else {
            size_t k = 0;
            while (k < cl && c[k] && cp_lower((unsigned char)c[k]) == cp_lower((unsigned char)first[k])) k++;
            cl = k;
        }
        m++;
    }
    if (first && common_sz && cl > tl) {
        size_t n = cl < common_sz - 1 ? cl : common_sz - 1;
        memcpy(common, token, tl < n ? tl : n);
        if (n > tl) memcpy(common + tl, first + tl, n - tl);
        common[n] = 0;
    }
    return m;
}

/* candidate provider: argi 0 = command names; argi >= 1 = words for argument argi of `cmd` */
typedef int (*cp_cands_fn)(int argi, const char *cmd, const char *const **out, void *user);

typedef struct {
    char line[1024];     /* the new line */
    int nmatch;          /* 0 none, 1 unique (a space was appended), >1 ambiguous */
    int idx[64];         /* indexes into cands of the first matches (ambiguous listing) */
    const char *const *cands;
    int changed;         /* line differs from the input */
} CpResult;

/* Complete the last token of `line` (text up to the cursor). */
static void cp_complete_line(const char *line, cp_cands_fn fn, void *user, CpResult *r)
{
    memset(r, 0, sizeof *r);
    size_t n = strlen(line);
    if (n >= sizeof r->line) n = sizeof r->line - 1;
    memcpy(r->line, line, n); r->line[n] = 0;
    /* token = after the last blank */
    size_t ts = n;
    while (ts > 0 && r->line[ts - 1] != ' ' && r->line[ts - 1] != '\t') ts--;
    char head[1024]; memcpy(head, r->line, ts); head[ts] = 0;
    const char *token = r->line + ts;
    /* argi = tokens before the token; cmd = first token */
    int argi = cp_arg_index(head);
    char cmd[64] = {0};
    {
        const char *p = head; while (*p == ' ' || *p == '\t') p++;
        size_t k = 0; while (p[k] && p[k] != ' ' && p[k] != '\t' && k < sizeof cmd - 1) { cmd[k] = p[k]; k++; }
        cmd[k] = 0;
    }
    const char *const *cands = NULL;
    int nc = fn ? fn(argi, cmd, &cands, user) : 0;
    if (nc <= 0 || !cands) return;
    char common[512];
    int m = cp_match(token, cands, nc, r->idx, 64, common, sizeof common);
    r->nmatch = m; r->cands = cands;
    if (!m) return;
    char out[1024];
    if (m == 1) snprintf(out, sizeof out, "%s%s ", head, cands[r->idx[0]]);
    else snprintf(out, sizeof out, "%s%s", head, common);
    r->changed = strcmp(out, r->line) != 0;
    snprintf(r->line, sizeof r->line, "%s", out);
}

/* ------------------------------------------------------------------ V3X `help` output */
#define CP_HELP_MAX 128
#define CP_SUB_MAX 16
typedef struct {
    int n;
    char name[CP_HELP_MAX][32];
    const char *names[CP_HELP_MAX];          /* = name[i], for cp_match */
    int nsub[CP_HELP_MAX];
    char sub[CP_HELP_MAX][CP_SUB_MAX][24];
    const char *subs[CP_HELP_MAX][CP_SUB_MAX];
} CpHelp;

static int cp_word_ch(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

/* Words of a help text that are alternatives: directly before or after a '|' (spaces allowed),
 * e.g. "on|off|report|reset", "status | dump (...) | load <type>" -> status dump load. Words
 * inside <...> are placeholders and skipped. */
static void cp_help_subwords(const char *h, CpHelp *ch, int i)
{
    size_t n = strlen(h);
    for (size_t p = 0; p < n; p++) {
        if (!cp_word_ch((unsigned char)h[p]) || (p > 0 && cp_word_ch((unsigned char)h[p - 1]))) continue;
        size_t e = p; while (e < n && cp_word_ch((unsigned char)h[e])) e++;
        if (p > 0 && h[p - 1] == '<') { p = e; continue; }
        size_t b = p; while (b > 0 && h[b - 1] == ' ') b--;
        size_t a = e; while (a < n && h[a] == ' ') a++;
        int alt = (b > 0 && h[b - 1] == '|') || (a < n && h[a] == '|');
        if (alt && e - p < 24 && ch->nsub[i] < CP_SUB_MAX) {
            char w[24]; memcpy(w, h + p, e - p); w[e - p] = 0;
            int dup = 0;
            for (int k = 0; k < ch->nsub[i]; k++) if (cp_streqi(ch->sub[i][k], w)) dup = 1;
            if (!dup) {
                snprintf(ch->sub[i][ch->nsub[i]], 24, "%s", w);
                ch->subs[i][ch->nsub[i]] = ch->sub[i][ch->nsub[i]];
                ch->nsub[i]++;
            }
        }
        p = e;
    }
}

/* Parses the V3X core `help` listing: one "  <name> <help text>" line per command (name without
 * blanks, at most 31 chars). Lines that do not start with two blanks are headers and skipped. */
static void cp_parse_help(const char *text, CpHelp *ch)
{
    memset(ch, 0, sizeof *ch);
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > 2 && p[0] == ' ' && p[1] == ' ' && ch->n < CP_HELP_MAX) {
            const char *q = p + 2; while (*q == ' ' && q < p + len) q++;
            size_t k = 0; while (q + k < p + len && q[k] != ' ' && q[k] != '\r' && k < 31) k++;
            if (k > 0) {
                int i = ch->n++;
                memcpy(ch->name[i], q, k); ch->name[i][k] = 0;
                ch->names[i] = ch->name[i];
                char h[512]; size_t hl = (size_t)(p + len - (q + k));
                if (hl >= sizeof h) hl = sizeof h - 1;
                memcpy(h, q + k, hl); h[hl] = 0;
                cp_help_subwords(h, ch, i);
            }
        }
        p = e ? e + 1 : NULL;
    }
}

static int cp_help_find(const CpHelp *ch, const char *name)
{
    for (int i = 0; i < ch->n; i++) if (cp_streqi(ch->name[i], name)) return i;
    return -1;
}

/* ------------------------------------------------------------------ console_plus.ini */
#define CP_OVR_MAX 64
typedef struct {
    int enabled, game_console, v3x_console, override_engine, log_installs, scan_frames;
    int novr;
    struct { char cmd[48]; CpKind kinds[CP_MAX_ARGS]; int nkinds; } ovr[CP_OVR_MAX];
    int bad_lines;
} CpCfg;

static void cp_cfg_defaults(CpCfg *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1; c->game_console = 1; c->v3x_console = 1; c->override_engine = 0;
    c->log_installs = 1; c->scan_frames = 120;
}

static char *cp_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* [console_plus] enabled game_console v3x_console override_engine log_installs scan_frames
 * [commands]     <command> = <kind> [<kind> ...]   (one kind per argument; "none" = skip that
 *                argument; a line with only "none" disables completion for the command) */
static void cp_parse_ini(const char *text, CpCfg *c)
{
    char sect[32] = "";
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        char ln[512]; if (len >= sizeof ln) len = sizeof ln - 1;
        memcpy(ln, p, len); ln[len] = 0;
        p = e ? e + 1 : NULL;
        char *s = cp_trim(ln);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') { char *q = strchr(s, ']'); if (q) *q = 0; snprintf(sect, sizeof sect, "%s", s + 1); continue; }
        char *eq = strchr(s, '=');
        if (!eq) { c->bad_lines++; continue; }
        *eq = 0;
        char *k = cp_trim(s), *v = cp_trim(eq + 1);
        if (cp_streqi(sect, "console_plus")) {
            int iv = atoi(v);
            if (cp_streqi(k, "enabled")) c->enabled = iv != 0;
            else if (cp_streqi(k, "game_console")) c->game_console = iv != 0;
            else if (cp_streqi(k, "v3x_console")) c->v3x_console = iv != 0;
            else if (cp_streqi(k, "override_engine")) c->override_engine = iv != 0;
            else if (cp_streqi(k, "log_installs")) c->log_installs = iv != 0;
            else if (cp_streqi(k, "scan_frames")) c->scan_frames = iv < 10 ? 10 : iv > 100000 ? 100000 : iv;
            else c->bad_lines++;
        } else if (cp_streqi(sect, "commands")) {
            if (c->novr >= CP_OVR_MAX || !*k || strlen(k) >= sizeof c->ovr[0].cmd) { c->bad_lines++; continue; }
            int i = c->novr, nk = 0, bad = 0;
            snprintf(c->ovr[i].cmd, sizeof c->ovr[i].cmd, "%s", k);
            for (char *t = strtok(v, " \t,"); t && nk < CP_MAX_ARGS; t = strtok(NULL, " \t,")) {
                CpKind kd = cp_kind_from_name(t);
                if ((int)kd < 0) { bad = 1; break; }
                c->ovr[i].kinds[nk++] = kd;
            }
            if (bad || !nk) { c->bad_lines++; continue; }
            c->ovr[i].nkinds = nk; c->novr++;
        } else c->bad_lines++;
    }
}

static int cp_cfg_override(const CpCfg *c, const char *cmd)
{
    for (int i = 0; i < c->novr; i++) if (cp_streqi(c->ovr[i].cmd, cmd)) return i;
    return -1;
}

#endif
