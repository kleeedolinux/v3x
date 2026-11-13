/* log_filter rule engine. See lf_rules.h. */
#include "lf_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Built-in configuration, used when v3x/log_filter.ini does not exist.
 * Must stay byte-identical to plugins/log_filter/log_filter.ini.example (the test checks it). */
const char LF_DEFAULT_INI[] =
"# log_filter configuration. Copy to <game>/binaries/v3x/log_filter.ini and edit.\n"
"# Without that file the plugin uses exactly these built-in defaults.\n"
"#\n"
"# Options\n"
"#   preset           default | aggressive. Sets the dedupe options below; options written\n"
"#                    after the preset line override it.\n"
"#                      default:    dedupe_first = 3, dedupe_template = 0, summary_interval = 60\n"
"#                      aggressive: dedupe_first = 1, dedupe_template = 1, summary_interval = 0\n"
"#                    aggressive logs the FIRST message of every (file:line, template) pair and\n"
"#                    only counts the rest in log_filter_stats.txt: every kind of error stays\n"
"#                    visible once, the repeats of the same mod bug do not.\n"
"#   enabled          1/0. 0 = hook stays installed but every message passes through.\n"
"#   dedupe_first     log the first N identical messages (same file:line and same text),\n"
"#                    suppress the rest. 0 = no dedupe.\n"
"#   dedupe_template  1 = 'identical' means same file:line and same message template: quoted\n"
"#                    text, numbers, paths and identifiers (words with digits or _ . / \\ :)\n"
"#                    are ignored. 0 = exact text.\n"
"#   summary_interval seconds between 'suppressed N more like this' lines in the game log.\n"
"#                    0 = never write summaries (counts still go to log_filter_stats.txt).\n"
"#   min_level        only filter messages at this spdlog level or above:\n"
"#                    0 trace, 1 debug, 2 info, 3 warning, 4 error, 5 critical.\n"
"#\n"
"# Rules (any number, max 128). LOC is matched against 'file.cpp:123', MSG against the\n"
"# formatted message text (multi-line messages included). Globs: * = anything, ? = one char,\n"
"# case-sensitive, the whole string must match (so use *text* for 'contains').\n"
"#   drop_loc = LOC           drop every message from that source location\n"
"#   drop_msg = MSG           drop messages whose text matches\n"
"#   drop     = LOC :: MSG    drop when both match\n"
"#   keep_loc / keep_msg / keep   same syntax; a keep match always wins over drop rules\n"
"#                                and is exempt from dedupe.\n"
"# Dropped messages vanish from every log the engine writes (error.log, debug.log, ...)\n"
"# and from the in-game error view, so only drop what is noise.\n"
"\n"
"[log_filter]\n"
"# change only this line to switch presets (default | aggressive)\n"
"preset = default\n"
"enabled = 1\n"
"min_level = 0\n"
"# optional overrides of the preset (uncomment to force a value):\n"
"# dedupe_first = 3\n"
"# dedupe_template = 0\n"
"# summary_interval = 60\n"
"\n"
"# Harmless noise (mod-authoring lint, not bugs):\n"
"# an event nothing fires; it can still be fired by console or other mods\n"
"drop = jomini_eventmanager.cpp:* :: Event * is orphaned\n"
"# a modifier type that only exists in script; works, the engine just wants script_only = yes\n"
"drop = modifier_type_definition_database.cpp:* :: Modifier type definition * is defined in script but not in code*\n"
"# a variable written but never read\n"
"drop = jomini_effect.cpp:* :: Variable '*' is set but is never used*\n"
"# script file without BOM; the engine reads it anyway\n"
"drop = lexer.cpp:* :: File '*' should be in utf8-bom encoding (will try to use it anyways)\n"
"# store lookup of DLC items when offline / not on that store\n"
"drop = dlc.cpp:* :: Could not find item in store backend.\n"
"\n"
"# Opt-in examples (remove the # to enable):\n"
"# drop = pdx_gui_localize.cpp:* :: Unlocalized text '*'\n"
"# drop = pdx_localize.cpp:* :: Duplicate localization key.*\n"
"# drop_loc = pdx_mod_metadata.cpp:168\n"
"\n"
"# Never hidden by default: parse errors (pdx_persistent_reader), script runtime errors\n"
"# (jomini_script_system), missing files/textures, shader compile errors, crashes.\n";

int lf_glob(const char *pat, const char *s, size_t n)
{
    size_t i = 0, star_s = 0;
    const char *star_p = NULL;
    while (i < n) {
        if (*pat == '*') { star_p = ++pat; star_s = i; continue; }
        if (*pat && (*pat == '?' || *pat == s[i])) { pat++; i++; continue; }
        if (star_p) { pat = star_p; i = ++star_s; continue; }
        return 0;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

int lf_config_init(lf_config *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    c->dedupe_first = 3;
    c->summary_interval = 60;
    c->min_level = 0;
    c->dups = (lf_dup *)calloc(LF_DEDUPE_CAP, sizeof(lf_dup));
    return c->dups ? 0 : -1;
}

void lf_config_free(lf_config *c)
{
    for (int i = 0; i < c->nrules; i++) { free(c->rules[i].loc); free(c->rules[i].msg); }
    if (c->dups) {
        for (uint32_t i = 0; i < LF_DEDUPE_CAP; i++) free(c->dups[i].sample);
        free(c->dups);
    }
    memset(c, 0, sizeof *c);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int lf_config_parse(lf_config *c, const char *text, char *err, size_t errn)
{
    int bad = 0, line_no = 0;
    if (err && errn) err[0] = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char buf[1024];
        line_no++;
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, p, len); buf[len] = 0;
        p = e ? e + 1 : p + strlen(p);
        char *s = trim(buf);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        const char *why = NULL;
        if (!eq) why = "missing '='";
        else {
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            if (!strcmp(k, "enabled")) c->enabled = atoi(v);
            else if (!strcmp(k, "dedupe_first")) c->dedupe_first = atoi(v);
            else if (!strcmp(k, "summary_interval")) c->summary_interval = atoi(v);
            else if (!strcmp(k, "min_level")) c->min_level = atoi(v);
            else if (!strcmp(k, "dedupe_template")) c->dedupe_template = atoi(v);
            else if (!strcmp(k, "preset")) {
                if (!strcmp(v, "default")) {
                    c->preset = 0; c->dedupe_first = 3; c->dedupe_template = 0; c->summary_interval = 60;
                } else if (!strcmp(v, "aggressive")) {
                    c->preset = 1; c->dedupe_first = 1; c->dedupe_template = 1; c->summary_interval = 0;
                } else why = "preset must be default or aggressive";
            }
            else {
                int keep = !strncmp(k, "keep", 4);
                const char *kind = k + 4;
                if (!keep && strncmp(k, "drop", 4)) why = "unknown key";
                else if (!*v) why = "empty pattern";
                else if (c->nrules >= LF_MAX_RULES) why = "too many rules";
                else {
                    const char *loc = "*", *msg = "*";
                    char *sep;
                    if (!strcmp(kind, "_loc")) loc = v;
                    else if (!strcmp(kind, "_msg")) msg = v;
                    else if (!*kind && (sep = strstr(v, "::"))) {
                        *sep = 0; loc = trim(v); msg = trim(sep + 2);
                        if (!*loc || !*msg) why = "empty LOC or MSG in 'LOC :: MSG'";
                    } else why = *kind ? "unknown key" : "expected 'LOC :: MSG'";
                    if (!why) {
                        lf_rule *r = &c->rules[c->nrules++];
                        r->keep = keep; r->loc = dupstr(loc); r->msg = dupstr(msg);
                        r->hits = 0; r->line_no = line_no;
                    }
                }
            }
        }
        if (why) {
            if (!bad && err && errn) snprintf(err, errn, "line %d: %s", line_no, why);
            bad++;
        }
    }
    return bad;
}

uint64_t lf_hash(const char *loc, const char *msg, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    for (const char *q = loc; *q; q++) { h ^= (uint8_t)*q; h *= 1099511628211ull; }
    h ^= 0xff; h *= 1099511628211ull;
    for (size_t i = 0; i < len; i++) { h ^= (uint8_t)msg[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

#define TPUT(ch) do { char ch_ = (ch); h ^= (uint8_t)ch_; h *= 1099511628211ull; \
    if (out && o + 1 < n) out[o++] = ch_; } while (0)

static int is_ws(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

uint64_t lf_template(const char *loc, const char *msg, size_t len, char *out, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    size_t o = 0, i = 0;
    for (const char *q = loc; *q; q++) { h ^= (uint8_t)*q; h *= 1099511628211ull; }
    h ^= 0xfe; h *= 1099511628211ull;
    while (i < len) {
        char ch = msg[i];
        if (is_ws(ch)) {
            while (i < len && is_ws(msg[i])) i++;
            TPUT(' ');
        } else if (ch == '\'' || ch == '"') {
            size_t j = i + 1;
            while (j < len && msg[j] != ch) j++;
            i = j < len ? j + 1 : len;
            TPUT('Q');
        } else {
            size_t j = i; int noisy = 0;
            while (j < len && !is_ws(msg[j]) && msg[j] != '\'' && msg[j] != '"') {
                char d = msg[j];
                if ((d >= '0' && d <= '9') || d == '_' || d == '.' || d == '/' || d == '\\' || d == ':') noisy = 1;
                j++;
            }
            if (noisy) TPUT('*');
            else for (size_t k = i; k < j; k++) TPUT(msg[k]);
            i = j;
        }
    }
    if (out && n) out[o] = 0;
    return h ? h : 1;
}

static lf_locstat *locstat(lf_config *c, const char *loc)
{
    for (uint32_t i = 0; i < c->nlocs; i++)
        if (!strcmp(c->locs[i].loc, loc)) return &c->locs[i];
    if (c->nlocs >= LF_LOC_CAP) return NULL;
    lf_locstat *l = &c->locs[c->nlocs++];
    snprintf(l->loc, sizeof l->loc, "%s", loc);
    return l;
}

static lf_dup *dup_slot(lf_config *c, uint64_t h)
{
    if (!c->dups) return NULL;
    uint32_t mask = LF_DEDUPE_CAP - 1, i = (uint32_t)h & mask;
    for (uint32_t probe = 0; probe < 64; probe++, i = (i + 1) & mask) {
        lf_dup *d = &c->dups[i];
        if (d->hash == h) return d;
        if (!d->hash) {
            if (c->ndups >= LF_DEDUPE_CAP / 4 * 3) return NULL;   /* keep the table sparse */
            d->hash = h; c->ndups++;
            return d;
        }
    }
    return NULL;
}

int lf_decide(lf_config *c, int level, const char *loc, const char *msg, size_t len, lf_dup **dup_out)
{
    if (dup_out) *dup_out = NULL;
    c->seen++;
    if (!c->enabled || level < c->min_level) { c->passed++; return LF_PASS; }
    lf_rule *drop = NULL;
    for (int i = 0; i < c->nrules; i++) {
        lf_rule *r = &c->rules[i];
        if (!r->keep && drop) continue;               /* already dropping; only a keep can change it */
        if (!lf_glob(r->loc, loc, strlen(loc)) || !lf_glob(r->msg, msg, len)) continue;
        if (r->keep) { r->hits++; c->kept_rule++; c->passed++; return LF_KEEP; }
        drop = r;
    }
    if (drop) {
        drop->hits++; c->dropped_rule++;
        lf_locstat *l = locstat(c, loc);
        if (l) l->dropped++;
        return LF_DROP_RULE;
    }
    if (c->dedupe_first > 0) {
        lf_dup *d = dup_slot(c, c->dedupe_template ? lf_template(loc, msg, len, NULL, 0) : lf_hash(loc, msg, len));
        if (d && ++d->count > (uint32_t)c->dedupe_first) {
            c->dropped_dup++;
            lf_locstat *l = locstat(c, loc);
            if (l) l->deduped++;
            if (!d->sample) {
                size_t ln = strlen(loc), m = len;
                char *s = (char *)malloc(ln + 2 + LF_SAMPLE_LEN + 1);
                if (s) {
                    if (m > LF_SAMPLE_LEN) m = LF_SAMPLE_LEN;
                    memcpy(s, loc, ln); s[ln] = ':'; s[ln + 1] = ' ';
                    memcpy(s + ln + 2, msg, m); s[ln + 2 + m] = 0;
                    for (char *q = s; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
                    d->sample = s;
                }
            }
            if (dup_out) *dup_out = d;
            return LF_DROP_DUP;
        }
    }
    c->passed++;
    return LF_PASS;
}

#define APPEND(...) do { if (o < n) { int w_ = snprintf(buf + o, n - o, __VA_ARGS__); \
    if (w_ > 0) o += (size_t)w_; if (o >= n) o = n - 1; } } while (0)

size_t lf_stats_report(const lf_config *c, char *buf, size_t n)
{
    size_t o = 0;
    if (!n) return 0;
    buf[0] = 0;
    APPEND("log_filter stats\n");
    APPEND("messages seen %llu, passed %llu (kept by rule %llu), dropped by rule %llu, deduped %llu\n",
           (unsigned long long)c->seen, (unsigned long long)c->passed, (unsigned long long)c->kept_rule,
           (unsigned long long)c->dropped_rule, (unsigned long long)c->dropped_dup);
    APPEND("config: preset=%s enabled=%d dedupe_first=%d dedupe_template=%d summary_interval=%d min_level=%d rules=%d\n\n",
           c->preset ? "aggressive" : "default", c->enabled, c->dedupe_first, c->dedupe_template,
           c->summary_interval, c->min_level, c->nrules);
    APPEND("== rules (hits, ini line, action, LOC :: MSG) ==\n");
    for (int i = 0; i < c->nrules; i++)
        APPEND("%10llu  line %-4d %s %s :: %s\n", (unsigned long long)c->rules[i].hits, c->rules[i].line_no,
               c->rules[i].keep ? "keep" : "drop", c->rules[i].loc, c->rules[i].msg);
    APPEND("\n== locations (dropped, deduped) ==\n");
    unsigned char used[LF_LOC_CAP] = {0};
    for (int k = 0; k < 40; k++) {
        int best = -1; uint64_t bv = 0;
        for (uint32_t i = 0; i < c->nlocs; i++) {
            uint64_t v = c->locs[i].dropped + c->locs[i].deduped;
            if (!used[i] && v > bv) { bv = v; best = (int)i; }
        }
        if (best < 0) break;
        used[best] = 1;
        APPEND("%10llu %10llu  %s\n", (unsigned long long)c->locs[best].dropped,
               (unsigned long long)c->locs[best].deduped, c->locs[best].loc);
    }
    APPEND("\n== most repeated %s (times seen, first %d logged; sample = first suppressed one) ==\n",
           c->dedupe_template ? "templates" : "messages", c->dedupe_first);
    if (c->dups) {
        uint32_t last = 0xffffffffu; uint32_t lasti = 0xffffffffu;
        for (int k = 0; k < 30; k++) {
            uint32_t best = 0xffffffffu, bv = 0;
            for (uint32_t i = 0; i < LF_DEDUPE_CAP; i++) {
                const lf_dup *d = &c->dups[i];
                if (!d->sample) continue;
                /* strictly below the previous pick, or equal with a higher index */
                if ((d->count < last || (d->count == last && i > lasti)) && d->count > bv) {
                    bv = d->count; best = i;
                }
            }
            if (best == 0xffffffffu) break;
            last = bv; lasti = best;
            APPEND("%10u  %s\n", bv, c->dups[best].sample);
        }
    }
    return o;
}
