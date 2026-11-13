/* mod_menu pure logic (see mm_logic.h). Pure C: no Windows, no engine. */
#include "mm_logic.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ small helpers */

typedef struct sb { char *p; size_t n, cap; int over; } sb;

static void sb_put(sb *s, const char *t, size_t len)
{
    if (s->over || s->n + len + 1 > s->cap) { s->over = 1; return; }
    memcpy(s->p + s->n, t, len);
    s->n += len;
    s->p[s->n] = 0;
}
static void sb_puts(sb *s, const char *t) { sb_put(s, t, strlen(t)); }
static void sb_printf(sb *s, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0 || k >= (int)sizeof tmp) { s->over = 1; return; }
    sb_put(s, tmp, (size_t)k);
}

static int mm_ieq(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* One ini line, trimmed. kind: 0 blank/comment/other, 1 section (name), 2 key=value. */
typedef struct ini_line { const char *start; size_t len; int kind; const char *k; size_t kn; const char *v; size_t vn; } ini_line;

static const char *ini_next(const char *p, ini_line *l)
{
    if (!p || !*p) return NULL;
    const char *e = p;
    while (*e && *e != '\n') e++;
    l->start = p;
    l->len = (size_t)(e - p);
    if (l->len && p[l->len - 1] == '\r') l->len--;
    const char *a = p, *b = p + l->len;
    while (a < b && (*a == ' ' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
    l->kind = 0; l->k = l->v = NULL; l->kn = l->vn = 0;
    if (a < b && *a == '[') {
        const char *c = memchr(a, ']', (size_t)(b - a));
        if (c) {
            const char *s = a + 1, *t = c;
            while (s < t && (*s == ' ' || *s == '\t')) s++;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
            l->kind = 1; l->k = s; l->kn = (size_t)(t - s);
        }
    } else if (a < b && *a != '#' && *a != ';') {
        const char *eq = memchr(a, '=', (size_t)(b - a));
        if (eq) {
            const char *ke = eq, *vs = eq + 1;
            while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            while (vs < b && (*vs == ' ' || *vs == '\t')) vs++;
            l->kind = 2; l->k = a; l->kn = (size_t)(ke - a); l->v = vs; l->vn = (size_t)(b - vs);
        }
    }
    return *e ? e + 1 : e;
}

static int value_false(const char *v, size_t n)
{
    return mm_ieq(v, n, "0") || mm_ieq(v, n, "off") || mm_ieq(v, n, "no") || mm_ieq(v, n, "false");
}

/* ------------------------------------------------------------------ ini */

int mm_ini_enabled(const char *ini, const char *stem)
{
    int in = 0, en = 1;
    ini_line l;
    for (const char *p = ini; (p = ini_next(p, &l));) {
        if (l.kind == 1) in = mm_ieq(l.k, l.kn, "plugins");
        else if (l.kind == 2 && in && mm_ieq(l.k, l.kn, stem)) en = !value_false(l.v, l.vn);
    }
    return en;
}

static int known(const mm_plugin *p, int n, const char *k, size_t kn)
{
    for (int i = 0; i < n; i++)
        if (mm_ieq(k, kn, p[i].stem)) return 1;
    return 0;
}

#define MM_SECTION_NOTE "# mod_menu: 0 = this plugin DLL is not loaded at the next start (in game: main menu > V3X)"

static void emit_section(sb *o, const mm_plugin *p, int n, const char *extra)
{
    sb_puts(o, "[plugins]\n" MM_SECTION_NOTE "\n");
    for (int i = 0; i < n; i++) sb_printf(o, "%s=%d\n", p[i].stem, p[i].enabled ? 1 : 0);
    sb_puts(o, extra);
}

size_t mm_ini_rewrite(const char *ini, const mm_plugin *p, int n, char *out, size_t cap)
{
    if (!out || !cap) return 0;
    out[0] = 0;
    size_t xcap = (ini ? strlen(ini) : 0) + 2;
    char *xbuf = (char *)malloc(xcap);
    if (!xbuf) return 0;
    sb x = {xbuf, 0, xcap, 0}; xbuf[0] = 0;
    ini_line l;
    int in = 0;
    /* pass 1: lines of unknown plugins inside [plugins] sections are kept */
    for (const char *q = ini; (q = ini_next(q, &l));) {
        if (l.kind == 1) { in = mm_ieq(l.k, l.kn, "plugins"); continue; }
        if (!in || l.kind != 2 || known(p, n, l.k, l.kn)) continue;
        sb_put(&x, l.start, l.len); sb_puts(&x, "\n");
    }
    sb o = {out, 0, cap, 0};
    int emitted = 0;
    in = 0;
    for (const char *q = ini; (q = ini_next(q, &l));) {
        if (l.kind == 1) {
            int pl = mm_ieq(l.k, l.kn, "plugins");
            if (pl) {
                if (!emitted) { emit_section(&o, p, n, xbuf); emitted = 1; }
                in = 1;
                continue;
            }
            if (in) sb_puts(&o, "\n");
            in = 0;
        }
        if (in) continue;
        sb_put(&o, l.start, l.len); sb_puts(&o, "\n");
    }
    if (!emitted) {
        if (o.n && !(o.n >= 2 && out[o.n - 1] == '\n' && out[o.n - 2] == '\n')) sb_puts(&o, "\n");
        emit_section(&o, p, n, xbuf);
    }
    free(xbuf);
    if (o.over) { out[0] = 0; return 0; }
    return o.n;
}

/* ------------------------------------------------------------------ plugin list */

static const struct { const char *stem, *desc; int locked; } k_desc[] = {
    {"alloc_tracker", "diagnostic: allocation call sites (alloc_report.txt)", 0},
    {"asset_stream", "forces the engine texture streamer on", 0},
    {"console_plus", "Tab completion of console command arguments", 0},
    {"content_packs", "mounts v3x/content_packs (needed for this menu)", 1},
    {"cpu_sched", "P-core / E-core thread placement on hybrid CPUs", 0},
    {"crash_logger", "own crash log v3x/crash.log", 0},
    {"devbridge", "named pipe for local tools (MCP)", 0},
    {"devconsole", "reverse-engineering console commands", 0},
    {"devtools", "CPU / memory reports, sampling profiler", 0},
    {"enhance", "single-player engine fixes (saves, speed table)", 0},
    {"fast_load", "load-time measurements and options", 0},
    {"frame_pacer", "timer sleep frame cap, idle / background caps", 0},
    {"gfx_driver", "fewer GPU driver threads (iGPU)", 0},
    {"hello", "template plugin, logs only", 0},
    {"ironman_console", "developer console in ironman", 0},
    {"keep_history", "keeps graphs of countries you stop playing", 0},
    {"lazy_mesh", "mesh usage report / deferred mesh loading", 0},
    {"log_filter", "drops noisy engine log messages", 0},
    {"map_cache", "texture re-read measurement / cache", 0},
    {"mem_tuner", "mimalloc tuning, memory collect", 0},
    {"sim_opt", "tick diagnostics, opt-in throttles", 0},
    {"tick_par", "tick fork/join estimate (read-only)", 0},
    {"tick_profiler", "per tick task time profile", 0},
};

const char *mm_describe(const char *stem, int *locked)
{
    if (locked) *locked = 0;
    for (size_t i = 0; i < sizeof k_desc / sizeof k_desc[0]; i++)
        if (mm_ieq(stem, strlen(stem), k_desc[i].stem)) {
            if (locked) *locked = k_desc[i].locked;
            return k_desc[i].desc;
        }
    return "";
}

int mm_list_add(mm_plugin *list, int n, const char *file)
{
    size_t len = file ? strlen(file) : 0;
    if (n >= MM_NMAX || len < 5 || !mm_ieq(file + len - 4, 4, ".dll")) return n;
    size_t sl = len - 4;
    if (sl >= MM_STEM || mm_ieq(file, sl, "mod_menu")) return n;
    for (size_t i = 0; i < sl; i++) {
        unsigned char c = (unsigned char)file[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return n;   /* ini/GUI-safe names only */
    }
    for (int i = 0; i < n; i++)
        if (mm_ieq(file, sl, list[i].stem)) return n;
    mm_plugin *p = &list[n];
    memset(p, 0, sizeof *p);
    memcpy(p->stem, file, sl);
    p->stem[sl] = 0;
    p->desc = mm_describe(p->stem, &p->locked);
    p->enabled = p->enabled_at_start = 1;
    return n + 1;
}

static int cmp_stem(const void *a, const void *b)
{
    const char *x = ((const mm_plugin *)a)->stem, *y = ((const mm_plugin *)b)->stem;
    for (;; x++, y++) {
        int d = tolower((unsigned char)*x) - tolower((unsigned char)*y);
        if (d || !*x) return d;
    }
}
void mm_list_sort(mm_plugin *list, int n) { qsort(list, (size_t)n, sizeof *list, cmp_stem); }

/* ------------------------------------------------------------------ GUI */

void mm_gui_text(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!cap) return;
    for (; in && *in && o + 1 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c > 0x7e || strchr("\"[]#$@\\{}", c)) continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
}

long mm_block_end(const char *t, long open)
{
    long i = open, depth = 0;
    while (t[i] && t[i] != '{') {
        if (t[i] == '#') { while (t[i] && t[i] != '\n') i++; continue; }
        i++;
    }
    for (; t[i]; i++) {
        char c = t[i];
        if (c == '#') { while (t[i + 1] && t[i + 1] != '\n') i++; continue; }
        if (c == '"') { i++; while (t[i] && t[i] != '"') i++; if (!t[i]) return -1; continue; }
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return i;
    }
    return -1;
}

static int count_of(const char *hay, const char *needle)
{
    int c = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) c++;
    return c;
}

static long line_start(const char *t, long i) { while (i > 0 && t[i - 1] != '\n') i--; return i; }

/* trimmed line [s, e) equals want */
static int line_is(const char *t, long s, const char *want)
{
    long e = s;
    while (t[e] && t[e] != '\n') e++;
    while (s < e && (t[s] == ' ' || t[s] == '\t')) s++;
    while (e > s && (t[e - 1] == ' ' || t[e - 1] == '\t' || t[e - 1] == '\r')) e--;
    return (size_t)(e - s) == strlen(want) && !memcmp(t + s, want, (size_t)(e - s));
}

#define A1 "\"[FrontEndMainView.OnSettings]\""
#define A2 "name = \"mainmenu_panel_bottom\""

static void gen_button(sb *o, const char *ind)
{
    sb_printf(o, "%s### V3X mod_menu: generated by binaries/v3x/plugins/mod_menu.dll (do not edit)\n", ind);
    sb_printf(o, "%sbutton_group = {\n", ind);
    sb_printf(o, "%s\tonclick = \"[GetVariableSystem.Toggle('" MM_VAR_OPEN "')]\"\n", ind);
    sb_printf(o, "%s\tusing = select_button_sound\n\n", ind);
    sb_printf(o, "%s\tflowcontainer = {\n", ind);
    sb_printf(o, "%s\t\tdirection = vertical\n\n", ind);
    sb_printf(o, "%s\t\ticon = {\n", ind);
    sb_printf(o, "%s\t\t\tparentanchor = hcenter\n", ind);
    sb_printf(o, "%s\t\t\tsize = { 35 35 }\n", ind);
    sb_printf(o, "%s\t\t\ttexture = \"gfx/interface/icons/generic_icons/checkbox_greencheck.dds\"\n", ind);
    sb_printf(o, "%s\t\t}\n\n", ind);
    sb_printf(o, "%s\t\ttext_single = {\n", ind);
    sb_printf(o, "%s\t\t\tparentanchor = hcenter\n", ind);
    sb_printf(o, "%s\t\t\tmax_width = 100\n", ind);
    sb_printf(o, "%s\t\t\tmin_width = 100\n", ind);
    sb_printf(o, "%s\t\t\traw_text = \"V3X\"\n", ind);
    sb_printf(o, "%s\t\t\tdefault_format = \"#clickable;glow_color:{0.1,0.1,0.1,1}\"\n", ind);
    sb_printf(o, "%s\t\t\talign = center|nobaseline\n", ind);
    sb_printf(o, "%s\t\t}\n", ind);
    sb_printf(o, "%s\t}\n", ind);
    sb_printf(o, "%s}\n\n", ind);
}

static void gen_row(sb *o, const mm_plugin *p, int i)
{
    char name[MM_STEM], desc[128];
    mm_gui_text(p->stem, name, sizeof name);
    mm_gui_text(p->desc, desc, sizeof desc);
    const char *I = "\t\t\t\t\t\t";
    sb_printf(o, "%shbox = {\n", I);
    sb_printf(o, "%s\tlayoutpolicy_horizontal = expanding\n", I);
    sb_printf(o, "%s\tspacing = 8\n", I);
    sb_printf(o, "%s\tbutton_checkbox = {\n", I);
    sb_printf(o, "%s\t\tsize = { 26 26 }\n", I);
    if (p->locked) {
        sb_printf(o, "%s\t\tchecked = yes\n", I);
        sb_printf(o, "%s\t\tenabled = no\n", I);
    } else {
        if (p->enabled_at_start)
            sb_printf(o, "%s\t\tchecked = \"[Not(GetVariableSystem.Exists('" MM_VAR_FMT "'))]\"\n", I, i);
        else
            sb_printf(o, "%s\t\tchecked = \"[GetVariableSystem.Exists('" MM_VAR_FMT "')]\"\n", I, i);
        sb_printf(o, "%s\t\tonclick = \"[GetVariableSystem.Toggle('" MM_VAR_FMT "')]\"\n", I, i);
        sb_printf(o, "%s\t\tonclick = \"[" MM_FN_FMT "]\"\n", I, i);
    }
    sb_printf(o, "%s\t}\n", I);
    sb_printf(o, "%s\ttext_single = {\n%s\t\traw_text = \"%s\"\n%s\t\tmin_width = 150\n%s\t\tmax_width = 150\n"
                 "%s\t\telide = right\n%s\t\talign = left|nobaseline\n%s\t}\n", I, I, name, I, I, I, I, I);
    sb_printf(o, "%s\ttext_single = {\n%s\t\traw_text = \"%s\"\n%s\t\tmin_width = 380\n%s\t\tmax_width = 380\n"
                 "%s\t\telide = right\n%s\t\talign = left|nobaseline\n%s\t\tdefault_format = \"#low\"\n%s\t}\n",
              I, I, desc, I, I, I, I, I, I);
    sb_printf(o, "%s\ttext_single = {\n", I);
    if (p->locked) sb_printf(o, "%s\t\tvisible = no\n", I);
    else sb_printf(o, "%s\t\tvisible = \"[GetVariableSystem.Exists('" MM_VAR_FMT "')]\"\n", I, i);
    sb_printf(o, "%s\t\traw_text = \"restart required\"\n%s\t\tmin_width = 120\n%s\t\talign = left|nobaseline\n%s\t}\n",
              I, I, I, I);
    sb_printf(o, "%s\texpand = {}\n", I);
    sb_printf(o, "%s}\n", I);
}

static void gen_panel(sb *o, const mm_plugin *p, int n)
{
    int half = (n + 1) / 2;
    sb_puts(o, "\t### V3X mod_menu window: generated by binaries/v3x/plugins/mod_menu.dll (do not edit)\n"
               "\twidget = {\n"
               "\t\tname = \"v3x_mod_menu\"\n"
               "\t\tvisible = \"[GetVariableSystem.Exists('" MM_VAR_OPEN "')]\"\n"
               "\t\tparentanchor = center\n"
               "\t\tsize = { 100% 100% }\n"
               "\t\talwaystransparent = no\n\n"
               "\t\tbutton = {\n"
               "\t\t\tsize = { 100% 100% }\n"
               "\t\t\tonclick = \"[GetVariableSystem.Clear('" MM_VAR_OPEN "')]\"\n"
               "\t\t}\n\n"
               "\t\twidget = {\n"
               "\t\t\tparentanchor = center\n"
               "\t\t\tscale = \"[ScaleToFitElementInside('(int32)1920', '(int32)1080')]\"\n"
               "\t\t\tusing = Window_Background_Popup\n"
               "\t\t\talwaystransparent = no\n\n"
               "\t\t\tvbox = {\n"
               "\t\t\t\tset_parent_size_to_minimum = yes\n"
               "\t\t\t\tmargin = { 20 20 }\n"
               "\t\t\t\tspacing = 10\n\n"
               "\t\t\t\theader_pattern = {\n"
               "\t\t\t\t\tlayoutpolicy_horizontal = expanding\n"
               "\t\t\t\t\tblockoverride \"header_text\" {\n"
               "\t\t\t\t\t\traw_text = \"V3X plugins\"\n"
               "\t\t\t\t\t}\n"
               "\t\t\t\t\tblockoverride \"button_close\" {\n"
               "\t\t\t\t\t\tonclick = \"[GetVariableSystem.Clear('" MM_VAR_OPEN "')]\"\n"
               "\t\t\t\t\t}\n"
               "\t\t\t\t}\n\n"
               "\t\t\t\ttext_single = {\n"
               "\t\t\t\t\traw_text = \"Ticked = loaded at the next game start. Saved to binaries/v3x/v3x.ini, section plugins.\"\n"
               "\t\t\t\t}\n\n"
               "\t\t\t\thbox = {\n"
               "\t\t\t\t\tspacing = 24\n");
    for (int c = 0; c < 2; c++) {
        int a = c ? half : 0, b = c ? n : half;
        if (a >= b) break;
        sb_puts(o, "\t\t\t\t\tvbox = {\n\t\t\t\t\t\tlayoutpolicy_vertical = expanding\n\t\t\t\t\t\tspacing = 4\n");
        for (int i = a; i < b; i++) gen_row(o, &p[i], i);
        sb_puts(o, "\t\t\t\t\t\texpand = {}\n\t\t\t\t\t}\n");
    }
    sb_puts(o, "\t\t\t\t}\n"
               "\t\t\t}\n"
               "\t\t}\n"
               "\t}\n");
}

long mm_gen_gui(const char *v, const mm_plugin *p, int n, char *out, size_t cap, const char **err)
{
    const char *e = NULL;
    if (err) *err = NULL;
    if (!v || !out || !cap) { if (err) *err = "bad arguments"; return -1; }
    if (n > MM_NMAX) n = MM_NMAX;
    if (strstr(v, "v3x_mod_menu")) e = "input already contains v3x_mod_menu (not the vanilla file)";
    else if (count_of(v, A1) != 1) e = "anchor A1 (FrontEndMainView.OnSettings) not found exactly once";
    else if (count_of(v, A2) != 1) e = "anchor A2 (mainmenu_panel_bottom) not found exactly once";
    if (e) { if (err) *err = e; return -1; }

    long a1 = (long)(strstr(v, A1) - v), ins1 = -1;
    long ls = line_start(v, a1);
    for (int k = 0; k < 3 && ls > 0 && ins1 < 0; k++) {
        ls = line_start(v, ls - 1);
        if (line_is(v, ls, "button_group = {")) ins1 = ls;
    }
    if (ins1 < 0) { if (err) *err = "anchor A1: no 'button_group = {' within 3 lines above OnSettings"; return -1; }
    char ind[64]; size_t il = 0;
    while ((v[ins1 + (long)il] == '\t' || v[ins1 + (long)il] == ' ') && il + 1 < sizeof ind) { ind[il] = v[ins1 + (long)il]; il++; }
    ind[il] = 0;

    long a2 = (long)(strstr(v, A2) - v), open = a2 - 1;
    while (open >= 0 && (v[open] == ' ' || v[open] == '\t' || v[open] == '\r' || v[open] == '\n')) open--;
    if (open < 0 || v[open] != '{') { if (err) *err = "anchor A2: name is not the first line of its block"; return -1; }
    long close = mm_block_end(v, open);
    if (close < 0) { if (err) *err = "anchor A2: block of mainmenu_panel_bottom is not balanced"; return -1; }
    long ins2 = line_start(v, close);
    if (!(open < ins1 && ins1 < ins2)) { if (err) *err = "anchor A1 is not inside the mainmenu_panel_bottom block"; return -1; }

    sb o = {out, 0, cap, 0};
    out[0] = 0;
    sb_put(&o, v, (size_t)ins1);
    gen_button(&o, ind);
    sb_put(&o, v + ins1, (size_t)(ins2 - ins1));
    gen_panel(&o, p, n);
    sb_puts(&o, v + ins2);
    if (o.over) { if (err) *err = "output buffer too small"; out[0] = 0; return -1; }
    return (long)o.n;
}

/* ------------------------------------------------------------------ content_load.json */

int mm_json_mod_paths(const char *json, char *out, size_t each, int max)
{
    const char *a = json ? strstr(json, "\"enabledMods\"") : NULL;
    if (!a || !each) return 0;
    a = strchr(a, '[');
    if (!a) return 0;
    int n = 0, depth = 0;
    for (const char *p = a; *p && n < max; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { if (--depth == 0) break; }
        else if (*p == '"') {
            const char *q = p + 1;
            while (*q && *q != '"') q += (*q == '\\' && q[1]) ? 2 : 1;
            if (!*q) break;
            int is_path = (q - p - 1 == 4 && !strncmp(p + 1, "path", 4));
            p = q;
            if (!is_path) continue;
            const char *v = p + 1;
            while (isspace((unsigned char)*v)) v++;
            if (*v != ':') continue;
            v++;
            while (isspace((unsigned char)*v)) v++;
            if (*v != '"') continue;
            char *o = out + (size_t)n * each;
            size_t k = 0;
            int bad = 0;
            for (v++; *v && *v != '"'; v++) {
                char c = *v;
                if (c == '\\') {
                    v++;
                    if (*v == '\\' || *v == '"' || *v == '/') c = *v;
                    else { bad = 1; if (!*v) break; continue; }
                }
                if (k + 1 < each) o[k++] = c; else bad = 1;
            }
            o[k] = 0;
            if (!*v) break;
            p = v;
            if (!bad && k) n++;
        }
    }
    return n;
}
