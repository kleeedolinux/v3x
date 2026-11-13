/* v3x_anchor.h - fail-closed anchor resolution for V3X plugins (header-only, C).
 *
 * The rule (docs/safety.md): never hook or patch an address you have not proven. An anchor is a
 * description of how to find one engine function or code site from things that survive patches
 * (strings, byte shapes), plus the checks that must all pass. If any anchor fails, the plugin
 * (or that feature) does NOTHING and logs why. A wrong hook crashes the game minutes later in a
 * place nobody can debug; a refused hook costs one log line.
 *
 *   static V3xAnchor A[] = {
 *       // the function referencing this exact string; every xref must agree on one function,
 *       // and its first bytes must match the prologue
 *       {"console_blocked", "No Console commands in ironman!", "48 89 5C 24 ?? 57 48 83 EC", V3X_ANCHOR_ALL_SAME},
 *       // a unique byte pattern in .text, result = match + 5
 *       {"spin", NULL, "F3 90 EB ?? 48 8B 4B", 0, 5},
 *   };
 *   if (v3x_anchor_resolve_all(api, "my_plugin", A, 2) != 0) return 1;   // fail closed, logged
 *   api->hook_safe(A[0].result, my_pre, NULL, NULL);
 *
 * Modes (field `xref`), used when `string` is set:
 *   V3X_ANCHOR_ALL_SAME (-1)  every xref of the string lies in the same function (else ambiguous)
 *   V3X_ANCHOR_SHAPE    (-2)  exactly one distinct function among the xrefs has `pattern` at its
 *                             start (the pattern selects; required)
 *   0, 1, 2, ...              the n-th xref (order in .text; brittle across builds, avoid)
 * With `string`: result = function start (+ offset); `pattern` (if set) must match at the
 * function start. Without `string`: `pattern` must match exactly once in .text; result =
 * match + offset. An optional `check(api, result, user)` callback runs last (return 0 = ok,
 * anything else = fail): verify a call target, a field offset, a vtable slot. */
#ifndef V3X_ANCHOR_H
#define V3X_ANCHOR_H

#include <stdio.h>
#include "v3x.h"
#include "v3x_scan.h"

#define V3X_ANCHOR_ALL_SAME (-1)
#define V3X_ANCHOR_SHAPE (-2)
#define V3X_ANCHOR_MAX_XREFS 64

typedef struct V3xAnchor {
    const char *name;        /* for the log */
    const char *string;      /* exact NUL-terminated string in .rdata/.data, or NULL */
    const char *pattern;     /* IDA pattern: prologue check (with string) or unique .text pattern */
    int xref;                /* V3X_ANCHOR_ALL_SAME, V3X_ANCHOR_SHAPE or an xref index (with string) */
    int offset;              /* added to the result */
    int (*check)(const V3X_API *api, void *result, void *user);   /* optional extra check */
    void *check_user;
    /* outputs */
    void *result;            /* NULL unless resolved */
    char err[120];           /* why it failed ("" on success) */
} V3xAnchor;

/* sets a->err (printf format), clears a->result, evaluates to -1 */
#define V3X_ANCHOR_FAIL_(a, ...) (snprintf((a)->err, sizeof (a)->err, __VA_ARGS__), (a)->result = NULL, -1)

/* Resolves one anchor in the running game. Returns 0 (a->result set) or -1 (a->err says why). */
static int v3x_anchor_resolve(const V3X_API *api, V3xAnchor *a)
{
    V3xSection text;
    a->result = NULL; a->err[0] = 0;
    if (!v3x_section(api->image_base(), ".text", &text)) return V3X_ANCHOR_FAIL_(a, "no .text section");
    if (!a->string) {
        void *hit = NULL;
        if (!a->pattern) return V3X_ANCHOR_FAIL_(a, "neither string nor pattern");
        int n = v3x_scan_count(&text, a->pattern, &hit);
        if (n < 0) return V3X_ANCHOR_FAIL_(a, "bad pattern");
        if (n != 1) return V3X_ANCHOR_FAIL_(a, "pattern matches %lld times (need exactly 1)", (long long)n);
        a->result = (uint8_t *)hit + a->offset;
    } else {
        void *s = api->find_string(a->string);
        if (!s) return V3X_ANCHOR_FAIL_(a, "string \"%.80s\" not found", a->string);
        void *x[V3X_ANCHOR_MAX_XREFS];
        int nx = v3x_xrefs(&text, s, x, V3X_ANCHOR_MAX_XREFS);
        if (nx == 0) return V3X_ANCHOR_FAIL_(a, "string has no code xref");
        if (nx > V3X_ANCHOR_MAX_XREFS) nx = V3X_ANCHOR_MAX_XREFS;
        V3xPattern pro;
        int has_pro = a->pattern && v3x_pattern_compile(a->pattern, &pro);
        if (a->pattern && !has_pro) return V3X_ANCHOR_FAIL_(a, "bad pattern");
        void *fn = NULL;
        if (a->xref >= 0) {
            if (a->xref >= nx) return V3X_ANCHOR_FAIL_(a, "only %lld xrefs", (long long)nx);
            fn = api->function_start(x[a->xref]);
            if (!fn) return V3X_ANCHOR_FAIL_(a, "xref not inside a .pdata function");
        } else if (a->xref == V3X_ANCHOR_ALL_SAME) {
            for (int i = 0; i < nx; i++) {
                void *f = api->function_start(x[i]);
                if (!f) return V3X_ANCHOR_FAIL_(a, "xref %lld not inside a .pdata function", (long long)i);
                if (fn && f != fn) return V3X_ANCHOR_FAIL_(a, "ambiguous: xrefs in several functions (%lld xrefs)", (long long)nx);
                fn = f;
            }
        } else if (a->xref == V3X_ANCHOR_SHAPE) {
            if (!has_pro) return V3X_ANCHOR_FAIL_(a, "V3X_ANCHOR_SHAPE needs a pattern");
            void *seen[V3X_ANCHOR_MAX_XREFS];
            int distinct = 0;
            for (int i = 0; i < nx; i++) {
                void *f = api->function_start(x[i]);
                if (!f || !v3x_pattern_match_at(&pro, (const uint8_t *)f)) continue;
                int dup = 0;
                for (int k = 0; k < distinct; k++) dup |= seen[k] == f;
                if (!dup) seen[distinct++] = f;
            }
            if (distinct) fn = seen[0];
            if (!fn) return V3X_ANCHOR_FAIL_(a, "no xref function has the expected shape (%lld xrefs)", (long long)nx);
            if (distinct > 1) return V3X_ANCHOR_FAIL_(a, "%lld functions have the shape (ambiguous)", (long long)distinct);
        } else {
            return V3X_ANCHOR_FAIL_(a, "bad xref mode %lld", (long long)a->xref);
        }
        if (has_pro && !v3x_pattern_match_at(&pro, (const uint8_t *)fn))
            return V3X_ANCHOR_FAIL_(a, "function start does not match the pattern (game updated?)");
        a->result = (uint8_t *)fn + a->offset;
    }
    if (a->check) {
        int rc = a->check(api, a->result, a->check_user);
        if (rc) return V3X_ANCHOR_FAIL_(a, "check failed (%lld)", (long long)rc);
    }
    return 0;
}

/* Resolves every anchor and logs one line each (`tag` = your plugin name): "anchor X at
 * exe+0x... ok" or "anchor X FAILED: why". Returns the number of anchors that failed (0 = all
 * ok). Fail closed on non-zero: install nothing that depends on a failed anchor. */
static int v3x_anchor_resolve_all(const V3X_API *api, const char *tag, V3xAnchor *a, int n)
{
    int failed = 0;
    for (int i = 0; i < n; i++) {
        if (v3x_anchor_resolve(api, &a[i]) == 0)
            api->log(tag, "anchor %s at exe+0x%llx ok", a[i].name,
                     (unsigned long long)((uintptr_t)a[i].result - api->image_base()));
        else {
            failed++;
            api->log(tag, "anchor %s FAILED: %s", a[i].name, a[i].err);
        }
    }
    return failed;
}

#endif
