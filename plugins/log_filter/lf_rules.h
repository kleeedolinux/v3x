/* log_filter rule engine: pure C, no Windows or engine dependencies, so test/log_filter_test.c
 * can compile it directly (#include "../plugins/log_filter/lf_rules.c").
 * Not thread-safe by itself: the plugin serialises calls with a lock. */
#ifndef LF_RULES_H
#define LF_RULES_H
#include <stddef.h>
#include <stdint.h>

#define LF_MAX_RULES   128
#define LF_DEDUPE_CAP  (1u << 15)  /* distinct messages tracked for dedupe (power of two) */
#define LF_LOC_CAP     1024u       /* distinct source locations tracked for stats */
#define LF_SAMPLE_LEN  200

enum { LF_PASS = 0, LF_DROP_RULE = 1, LF_DROP_DUP = 2, LF_KEEP = 3 };

typedef struct lf_rule {
    int keep;                 /* 1 = keep rule (beats drop rules, exempt from dedupe) */
    char *loc;                /* glob on "file.cpp:123" ("*" = any) */
    char *msg;                /* glob on the formatted message ("*" = any) */
    uint64_t hits;
    int line_no;              /* line in the ini, for stats */
} lf_rule;

typedef struct lf_dup {       /* dedupe slot */
    uint64_t hash;            /* 0 = empty */
    uint32_t count;           /* times seen */
    uint32_t reported;        /* suppressed count already reported in a summary line */
    void *user;               /* plugin data (engine logger + log_msg copy), set on first suppression */
    char *sample;             /* "file:line: message" (truncated), set on first suppression */
} lf_dup;

typedef struct lf_locstat { char loc[96]; uint64_t dropped, deduped; } lf_locstat;

typedef struct lf_config {
    int enabled;
    int dedupe_first;         /* log the first N identical messages, 0 = no dedupe */
    int summary_interval;     /* seconds between summary lines, 0 = never write to the game log */
    int min_level;            /* only filter messages with spdlog level >= this (0 trace .. 5 critical) */
    int dedupe_template;      /* 1 = dedupe key is file:line + message template (lf_template), not exact text */
    int preset;               /* 0 default, 1 aggressive (info only; preset= sets the options above) */
    lf_rule rules[LF_MAX_RULES];
    int nrules;
    /* stats */
    uint64_t seen, passed, dropped_rule, dropped_dup, kept_rule;
    lf_dup *dups;             /* LF_DEDUPE_CAP slots, allocated by lf_config_init */
    uint32_t ndups;
    lf_locstat locs[LF_LOC_CAP];
    uint32_t nlocs;
} lf_config;

extern const char LF_DEFAULT_INI[];

/* glob: '*' = any run (including newlines), '?' = one char, everything else literal (case-sensitive).
 * `s` has explicit length (engine payloads are not NUL-terminated). */
int lf_glob(const char *pat, const char *s, size_t n);

int  lf_config_init(lf_config *c);                 /* zero + defaults for options, no rules; 0 = ok */
void lf_config_free(lf_config *c);
/* Parses ini text, appending rules. Returns number of bad lines; first error text into err. */
int  lf_config_parse(lf_config *c, const char *text, char *err, size_t errn);

/* Decide one message. loc = "file:line" (NUL-terminated), msg/len = payload.
 * Updates stats. On LF_DROP_DUP and *dup_out != NULL the caller may attach dup->user / sample. */
int  lf_decide(lf_config *c, int level, const char *loc, const char *msg, size_t len, lf_dup **dup_out);

uint64_t lf_hash(const char *loc, const char *msg, size_t len);

/* Message template: whitespace runs -> one ' ', a quoted run ('..' or "..") -> 'Q', a word containing
 * a digit or one of _ . / \ : -> '*', other words kept. Writes up to n-1 chars + NUL into out
 * (out may be NULL); returns the 64-bit hash of loc + full template. Mirrored in re_log_stats.py. */
uint64_t lf_template(const char *loc, const char *msg, size_t len, char *out, size_t n);

/* Writes a human-readable stats report into buf (always NUL-terminated). Returns length. */
size_t lf_stats_report(const lf_config *c, char *buf, size_t n);

#endif
