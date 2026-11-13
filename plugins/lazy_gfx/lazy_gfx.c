/* lazy_gfx: why texture loading is slow, per texture and per stage, plus a safe prefetch.
 * See docs/re/lazy_gfx.md. RVAs below are 1.15.1 comments only: every target is found from the
 * CreateTexture anchor (string "Could not find texture" + byte shapes, same as map_cache) and the
 * call sites inside it; any mismatch = fail closed (no hooks at all).
 *
 * CreateTexture 0x3c5af20(out, path std::string*, mips, flags) does, in this order (static RE):
 *   +0x0e3 call opts      0x3c5df50(&opts, &view)    load_settings lookup for the path       ret +0x0e8
 *   +0x1bb call VFSOpen   0x3e1c4f0(&file, &view, 0) PhysFS: getRealDir 0x3dffd50 (stat on every
 *                         root until found, called at VFSOpen+0x40) then openRead (CreateFileW on
 *                         every root until found)                                         ret +0x1c0
 *   +0x1c7 call move      0x3e3c720(&local, &file)   = "VFSOpen returned"                    ret +0x1cc
 *   +0x20f Reserve (map_cache hooks it) + file vslot +8 read
 *   +0x248 call decode    0x3c5e0d0(&img, &view, buf, size, &opts) DDS / FreeImage (png, jpg) ret +0x24d
 *   +0x365 call [ctx]+0x50 CGfxDX11MasterContext::CreatePixelBuffer 0x3c6ac30 (CreateTexture2D) ret +0x368
 *   +0x3fa call obj       0x3c42ef0(ctx, &out, pixbuf, &desc, path, flag) texture object; its
 *                         +0xa3 call stamp 0x3e14180(path) runs getRealDir AGAIN (file stamp) ret +0x3ff
 * Each of opts / getRealDir / move / decode / pixbuf / obj / stamp gets a hook_safe pre+post that
 * does nothing unless the return address is the CreateTexture (or VFSOpen / obj) call site above,
 * so the many other callers of these functions are untouched. CreateTexture, GetOrLoadTexture and
 * Reserve are NOT hooked here (map_cache owns them); VFSOpen is NOT hooked (par_load owns it).
 *
 * mode=off      nothing.
 * mode=measure  (default) timing only, no behaviour change. Writes v3x/lazy_gfx_report.txt and
 *               v3x/lazy_gfx_textures.csv after every load transition, at exit and on `lazygfx report`,
 *               and v3x/lazy_gfx_prefetch.txt (textures read before the first Game load, any thread,
 *               resolved to real files) when the first Game load begins.
 * mode=on       measure + prefetch: at start, background threads read the files of the previous
 *               run's v3x/lazy_gfx_prefetch.txt (size+mtime checked) so the main thread's opens and
 *               reads hit the OS file cache. No engine call, no engine memory touched. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "v3x.h"
#include "lg_core.h"
#include "lg_cache.h"

V3X_EXPORT const char *v3x_plugin_name = "lazy_gfx";
V3X_PLUGIN_MANIFEST("lazy_gfx", "1.0.0", "v3x", "texture load stage split + prefetch of last run's startup textures",
                    "release/1.15.1");

static const V3X_API *api;
static lg_state g_st;
static lg_prefetch g_pf;
static volatile LONG g_pf_running, g_log_left, g_manifest_done;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static uint32_t g_main_tid;
static char g_dir[MAX_PATH];

/* ------------------------------------------------------------------ anchors */

enum { H_OPTS, H_RDIR, H_MOVE, H_DEC, H_PIX, H_OBJ, H_STAMP, H_N };
static const char *H_NAME[H_N] = { "opts", "getrealdir", "vfs_move", "decode", "pixbuf", "obj", "stamp" };

typedef struct lg_anchors {
    uint8_t *ct, *gol, *vfs;          /* CreateTexture, GetOrLoadTexture (classification only), VFSOpen */
    uint8_t *fn[H_N];                 /* hooked functions */
    uint8_t *ret[H_N];                /* the only return address each hook reacts to */
} lg_anchors;
static lg_anchors g_a;

typedef struct { int off; const char *pat; } shape_t;
/* CreateTexture shapes: prologue + argument moves + the Reserve site (as map_cache) + every call site we use */
static const shape_t CT_SHAPE[] = {
    { 0x000, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 48 89 4C 24 08 55 41 54 41 55 41 56 41 57" },
    { 0x02C, "41 8B F9 45 8B F0 4C 8B E2 4C 8B F9" },
    { 0x204, "3B 5D DC 7E 0F 8B D3 48 8D 4D D0 E8" },
    { 0x0DA, "48 8D 54 24 40 48 8D 4D E8 E8" },                      /* opts(&opts, &view)          E8 at +0xe3 */
    { 0x1AF, "45 33 C0 48 8D 54 24 40 48 8D 4D 60 E8" },             /* VFSOpen(&file, &view, 0)    E8 at +0x1bb */
    { 0x1C0, "48 8B D0 48 8D 4D 00 E8" },                            /* move(&local, file)          E8 at +0x1c7 */
    { 0x23A, "44 8B CB 4C 8B C6 48 8D 55 50 48 8D 4D 80 E8" },       /* decode(&img,&view,buf,size) E8 at +0x248 */
    { 0x35C, "4C 8D 45 80 48 8D 54 24 40 FF 50 50" },                /* [ctx]+0x50 CreatePixelBuffer  at +0x365 */
    { 0x3EA, "4C 8D 4C 24 40 4C 8B C3 48 8D 54 24 70 49 8B CD E8" }, /* obj(ctx,&out,pixbuf,&desc,..) E8 at +0x3fa */
};
static const shape_t GOL_SHAPE[] = {
    { 0x000, "48 89 4C 24 08 53 56 57 48 83 EC 40 41 8B D8 48 8B F2 48 8B F9" },
    { 0x051, "44 8B CB 48 8B D6 48 8D 4C 24 78 E8" },
};
#define CT_OPTS 0x0E3
#define CT_VFS 0x1BB
#define CT_MOVE 0x1C7
#define CT_DEC 0x248
#define CT_PIX_RET 0x368
#define CT_OBJ 0x3FA
#define VFS_RDIR 0x040
#define OBJ_STAMP 0x0A3
#define OBJ_SLOT60 0x037
#define GOL_CT_CALL 0x05C

static int in_image(const void *p, size_t n)
{
    uintptr_t b = api->image_base(), a = (uintptr_t)p;
    return a >= b && a + n <= b + api->image_size();
}
static uint32_t rva(const void *p) { return (uint32_t)((uintptr_t)p - api->image_base()); }
static uint8_t *rel32(const uint8_t *ins) { int32_t d; memcpy(&d, ins + 1, 4); return (uint8_t *)ins + 5 + d; }

static int match(const uint8_t *p, const char *pat)
{
    for (const char *s = pat; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { s += s[1] == '?' ? 2 : 1; p++; continue; }
        unsigned v; if (sscanf(s, "%2x", &v) != 1 || *p != (uint8_t)v) return 0;
        s += 2; p++;
    }
    return 1;
}
static int shapes_ok(const uint8_t *fn, const shape_t *sh, int n, size_t span)
{
    if (!fn || !in_image(fn, span)) return 0;
    for (int i = 0; i < n; i++) if (!match(fn + sh[i].off, sh[i].pat)) return 0;
    return 1;
}

static int section(const char *name, uint8_t **p, size_t *n)
{
    uint8_t *b = (uint8_t *)api->image_base();
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!strncmp((const char *)s[i].Name, name, 8)) { *p = b + s[i].VirtualAddress; *n = s[i].Misc.VirtualSize; return 1; }
    return 0;
}
static uint64_t pref_base(void)
{
    uint8_t *b = (uint8_t *)api->image_base();
    return ((IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew))->OptionalHeader.ImageBase;
}
/* an absolute code pointer stored in .rdata -> address in our mapping (relocated at runtime; at the
 * preferred base when an offline test maps the exe as an image) */
static uint8_t *abs_ptr(uint64_t v)
{
    uintptr_t b = api->image_base(); size_t n = api->image_size();
    if (v >= b && v < b + n) return (uint8_t *)(uintptr_t)v;
    uint64_t pb = pref_base();
    if (v >= pb && v < pb + n) return (uint8_t *)(b + (uintptr_t)(v - pb));
    return NULL;
}

/* the unique function referencing string s (all xrefs must be inside one function) */
static uint8_t *fn_by_string(const char *s)
{
    void *str = api->find_string(s);
    uint8_t *f = NULL;
    for (int i = 0; str && i < 64; i++) {
        void *x = api->find_xref(str, i);
        if (!x) break;
        uint8_t *g = api->function_start(x);
        if (!g) return NULL;
        if (f && g != f) return NULL;
        f = g;
    }
    return f;
}

/* every function referencing string s (distinct, up to max) */
static int fns_by_string(const char *s, uint8_t **out, int max)
{
    void *str = api->find_string(s);
    int n = 0;
    for (int i = 0; str && i < 64; i++) {
        void *x = api->find_xref(str, i);
        if (!x) break;
        uint8_t *g = api->function_start(x);
        int dup = 0;
        for (int k = 0; k < n; k++) if (out[k] == g) dup = 1;
        if (g && !dup && n < max) out[n++] = g;
    }
    return n;
}
static int in_list(const uint8_t *f, uint8_t *const *v, int n) { for (int i = 0; f && i < n; i++) if (v[i] == f) return 1; return 0; }

/* Returns the number of failed anchors (each logged); fills g_a only when all are OK. */
static int lg_resolve(void)
{
    lg_anchors a; memset(&a, 0, sizeof a);
    int bad = 0;
#define FAIL(...) do { api->log("lazy_gfx", "FAIL-CLOSED " __VA_ARGS__); bad++; } while (0)
    void *s = api->find_string("Could not find texture");
    for (int i = 0; s && i < 16 && !a.ct; i++) {
        void *x = api->find_xref(s, i);
        if (!x) break;
        uint8_t *f = api->function_start(x);
        if (shapes_ok(f, CT_SHAPE, (int)(sizeof CT_SHAPE / sizeof *CT_SHAPE), 0x440)) a.ct = f;
    }
    if (!a.ct) { FAIL("create: 'Could not find texture' function with the expected call sites not found"); memset(&g_a, 0, sizeof g_a); return H_N + 1; }
    a.fn[H_OPTS] = rel32(a.ct + CT_OPTS);  a.ret[H_OPTS] = a.ct + CT_OPTS + 5;
    a.vfs        = rel32(a.ct + CT_VFS);
    a.fn[H_MOVE] = rel32(a.ct + CT_MOVE);  a.ret[H_MOVE] = a.ct + CT_MOVE + 5;
    a.fn[H_DEC]  = rel32(a.ct + CT_DEC);   a.ret[H_DEC]  = a.ct + CT_DEC + 5;
    a.fn[H_OBJ]  = rel32(a.ct + CT_OBJ);   a.ret[H_OBJ]  = a.ct + CT_OBJ + 5;
    a.ret[H_PIX] = a.ct + CT_PIX_RET;
    /* VFSOpen: the "VFSOpen Error: " function; getRealDir is its call at +0x40 (lea rcx,[rbp-0x79]) */
    if (!in_image(a.vfs, 0x60) || fn_by_string("VFSOpen Error: ") != a.vfs || !match(a.vfs, "48 8B C4 48 89 58 08 48 89 70 18")
        || !match(a.vfs + VFS_RDIR - 4, "48 8D 4D 87 E8"))
        FAIL("vfs: CreateTexture+0x1bb is not the VFSOpen function / getRealDir call moved");
    else {
        a.fn[H_RDIR] = rel32(a.vfs + VFS_RDIR); a.ret[H_RDIR] = a.vfs + VFS_RDIR + 5;
        if (!in_image(a.fn[H_RDIR], 0x20) || !match(a.fn[H_RDIR], "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00"))
            FAIL("getrealdir: prologue mismatch");
    }
    if (!in_image(a.fn[H_OPTS], 0x10) || !match(a.fn[H_OPTS], "40 57 41 55 48 83 EC 48")) FAIL("opts: prologue mismatch");
    if (!in_image(a.fn[H_MOVE], 0x20) || !match(a.fn[H_MOVE], "48 89 5C 24 08 57 48 83 EC 30 48 8B FA 48 8B D9 48 3B CA"))
        FAIL("vfs_move: prologue mismatch");
    if (!in_image(a.fn[H_DEC], 0x20) || fn_by_string("Failed to load image '{}': unknown format.") != a.fn[H_DEC]
        || !match(a.fn[H_DEC], "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18"))
        FAIL("decode: CreateTexture+0x248 is not the texture loader");
    /* obj: prologue, the [ctx]+0x60 call (CreateTexture object slot) and the stamp call */
    if (!in_image(a.fn[H_OBJ], 0xc0) || !match(a.fn[H_OBJ], "48 89 5C 24 08 48 89 54 24 10 55 56 57 48 83 EC 70")
        || !match(a.fn[H_OBJ] + OBJ_SLOT60, "FF 50 60") || !match(a.fn[H_OBJ] + OBJ_STAMP - 5, "48 8D 4C 24 50 E8")
        || !match(a.fn[H_OBJ] + OBJ_STAMP + 5, "89 45 7C"))
        FAIL("obj: shape mismatch");
    else {
        a.fn[H_STAMP] = rel32(a.fn[H_OBJ] + OBJ_STAMP); a.ret[H_STAMP] = a.fn[H_OBJ] + OBJ_STAMP + 5;
        if (!in_image(a.fn[H_STAMP], 0x20) || !match(a.fn[H_STAMP], "48 89 5C 24 08 48 89 74 24 20 57 48 83 EC 70"))
            FAIL("stamp: prologue mismatch");
    }
    /* pixbuf: CGfxDX11MasterContext slot +0x50. Both strings are referenced by two backends (DX11 and
     * Vulkan), so: a vtable in .rdata whose slot X is a "Failed creating pixel buffer" function, slot
     * X+0x10 a "Failed creating texture" function, X = vtable+0x50, and whose RTTI class (complete object
     * locator at vtable-8) is CGfxDX11MasterContext. Exactly one such vtable. */
    uint8_t *pixc[4], *texc[4];
    int npix = fns_by_string("Failed creating pixel buffer", pixc, 4), ntex = fns_by_string("Failed creating texture", texc, 4);
    int vt_hits = 0;
    uint8_t *rd; size_t rdn, txn0; uint8_t *tx0;
    if (npix > 0 && ntex > 0 && section(".rdata", &rd, &rdn) && section(".text", &tx0, &txn0))
        for (size_t i = 0x58; i + 0x18 <= rdn; i += 8) {
            uint64_t v0, v1, colp; memcpy(&v0, rd + i, 8);
            uint8_t *f0 = abs_ptr(v0);
            if (!f0 || f0 < tx0 || f0 >= tx0 + txn0 || !in_list(f0, pixc, npix)) continue;
            memcpy(&v1, rd + i + 0x10, 8);
            if (!in_list(abs_ptr(v1), texc, ntex)) continue;
            memcpy(&colp, rd + i - 0x58, 8);                   /* vtable = rd+i-0x50; COL pointer at vtable-8 */
            uint8_t *col = abs_ptr(colp);
            if (!col || !in_image(col, 24)) continue;
            uint32_t td; memcpy(&td, col + 12, 4);
            const char *name = (const char *)api->image_base() + td + 16;
            if (td && in_image(name, 28) && !memcmp(name, ".?AVCGfxDX11MasterContext@@", 28)) { vt_hits++; a.fn[H_PIX] = f0; }
        }
    if (vt_hits != 1 || !match(a.fn[H_PIX], "48 89 5C 24 08 48 89 6C 24 18 48 89 54 24 10")) {
        FAIL("pixbuf: CGfxDX11MasterContext slot +0x50 not found (%d vtables)", vt_hits);
        a.fn[H_PIX] = NULL;
    }
    /* GetOrLoadTexture (no hook; only to tell cache misses from direct requests) */
    uint8_t *tx; size_t txn;
    if (section(".text", &tx, &txn))
        for (size_t i = 0; i + 5 <= txn && !a.gol; i++) {
            if (tx[i] != 0xE8 || rel32(tx + i) != a.ct) continue;
            uint8_t *f = api->function_start(tx + i);
            if (f && tx + i == f + GOL_CT_CALL && shapes_ok(f, GOL_SHAPE, 2, 0x100)) a.gol = f;
        }
    if (!a.gol) api->log("lazy_gfx", "note: GetOrLoadTexture not found; via_cache column will be 0");
#undef FAIL
    if (bad) { memset(&g_a, 0, sizeof g_a); return bad; }
    g_a = a;
    for (int i = 0; i < H_N; i++) api->log("lazy_gfx", "anchor %-10s OK at %08x (call site ret %08x)", H_NAME[i], rva(a.fn[i]), rva(a.ret[i]));
    return 0;
}

/* ------------------------------------------------------------------ per-thread record */

enum { E_OPTS_PRE = 1, E_OPTS_POST, E_RD_PRE, E_RD_POST, E_MOVE, E_DEC_PRE, E_DEC_POST, E_PIX_PRE, E_PIX_POST,
       E_OBJ_PRE, E_ST_PRE, E_ST_POST, E_OBJ_POST };

typedef struct tls_t {
    int stage;                 /* last event (E_*), 0 = idle */
    /* decode-time state (cache + layout learning) */
    const uint8_t *dec_buf; size_t dec_n; int dds_ok; lg_dds dds;
    int cacheable; char abs[600]; uint64_t src_size, src_mtime; double t_dec0;
    int have_hdr; lg_cache_hdr hdr;           /* a valid cache entry exists for this source */
    uint8_t *sub; size_t sub_n;               /* cached DDS fed to decode instead of the source (free after the object) */
    double t_start, t_last;
    const void *rkey; int ridx; /* last getRealDir dirName pointer -> root index */
    lg_rec r;
} tls_t;

static tls_t *tls_get(void)
{
    tls_t *t = TlsGetValue(g_tls);
    if (!t) {
        t = calloc(1, sizeof *t);
        if (t) { t->ridx = 255; TlsSetValue(g_tls, t); }
    }
    return t;
}

static int readable(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p, end = a + n;
    if (!p || end < a) return 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

/* requester chain: the return addresses above CreateTexture's own frame. frames[] from
 * RtlCaptureStackBackTrace; ct_ret = the return address of the hooked call inside CreateTexture. */
static int extract_req(void *const *frames, int n, const void *ct_ret, uint32_t *out)
{
    int k = 0, start = -1;
    for (int i = 0; i < n; i++) if (frames[i] == ct_ret) { start = i + 1; break; }
    if (start < 0) return 0;
    for (int i = start; i < n && k < LG_FRAMES; i++)
        if (in_image(frames[i], 1)) out[k++] = rva(frames[i]);
    return k;
}

static void drop_sub(tls_t *t) { free(t->sub); t->sub = NULL; t->sub_n = 0; t->have_hdr = 0; t->cacheable = 0; t->dds_ok = 0; }

static void finish(tls_t *t, int complete)
{
    drop_sub(t);                            /* the engine has created the pixel buffer from it by now */
    double now = lg_now_ms(&g_st);
    t->r.total_ms = now - t->t_start;
    t->r.complete = (uint8_t)complete;
    if (!complete) t->r.ok = 0;
    if (g_pf_running || g_pf.n) t->r.prefetched = (uint8_t)lg_pf_was_read(&g_pf, lg_hash(t->r.path, strlen(t->r.path)));
    lg_add(&g_st, &t->r);
    if (complete && g_st.cfg.slow_log_ms && t->r.total_ms >= g_st.cfg.slow_log_ms && InterlockedDecrement(&g_log_left) >= 0)
        api->log("lazy_gfx", "slow %.1f ms %.0f KB %s tid=%lu stat %.1f open %.1f read %.1f decode %.1f upload %.1f obj %.1f (stamp %.1f) req %06x<-%06x '%s'",
                 t->r.total_ms, (double)t->r.bytes / 1024.0, t->r.main ? "main" : "worker", (unsigned long)t->r.tid,
                 t->r.st[LG_ST_STAT], t->r.st[LG_ST_OPEN], t->r.st[LG_ST_READ], t->r.st[LG_ST_DECODE], t->r.st[LG_ST_UPLOAD],
                 t->r.st[LG_ST_OBJ], t->r.st[LG_ST_STAMP], t->r.req[0], t->r.req[1], t->r.path);
    t->stage = 0;
}

/* one event on the current thread's record: the time since the previous event goes to `stage` */
static void ev(tls_t *t, int e, int stage)
{
    double now = lg_now_ms(&g_st);
    if (stage >= 0) t->r.st[stage] += (float)(now - t->t_last);
    t->t_last = now;
    t->stage = e;
}

static void start_rec(tls_t *t, const V3X_HookCall *c)
{
    if (t->stage) finish(t, 0);            /* the previous CreateTexture on this thread never got its object (file missing) */
    memset(&t->r, 0, sizeof t->r);
    const uint8_t *view = (const uint8_t *)c->args[1];      /* {const char *p; int len; ...} */
    const char *p = view ? *(const char *const *)view : NULL;
    int len = view ? *(const int *)(view + 8) : 0;
    if (p && len > 0) {
        if (len >= LG_PATH) { p += len - (LG_PATH - 1); len = LG_PATH - 1; }   /* keep the tail */
        memcpy(t->r.path, p, (size_t)len);
        t->r.path[len] = 0;
        for (char *q = t->r.path; *q; q++) if (*q == ',' || *q == '"') *q = '_';
    }
    t->r.ext = (uint8_t)lg_ext_of(t->r.path, strlen(t->r.path));
    t->r.tid = GetCurrentThreadId();
    t->r.main = g_main_tid && t->r.tid == g_main_tid;
    t->r.phase = (uint8_t)g_st.cur_phase;
    t->r.root = 255;
    t->r.ok = 1;
    void *fr[32];
    USHORT nf = RtlCaptureStackBackTrace(0, 32, fr, NULL);
    extract_req(fr, nf, g_a.ret[H_OPTS], t->r.req);
    t->r.via_cache = g_a.gol && t->r.req[0] && t->r.req[0] >= rva(g_a.gol) && t->r.req[0] < rva(g_a.gol) + 0x100;
    t->t_start = t->t_last = lg_now_ms(&g_st);
    t->r.t_ms = t->t_start;
    t->stage = E_OPTS_PRE;
}

/* ------------------------------------------------------------------ processed-texture cache
 * png_cache=build : after the engine decoded a png/jpg/tga (FreeImage), write <cache>/<key>.v3xtex = the
 *                   engine's output digest + a single-mip DDS of exactly those bytes, with the DDS header
 *                   learned from a real DDS that decoded to the same pdx format byte.
 *          verify : as build, and when an entry exists, compare the fresh engine decode with its digest.
 *          on     : when an entry exists, decode is given the DDS bytes instead of the source (the loader
 *                   dispatches by content: 'DDS ' -> 0x3c37c50); afterwards the result must equal the
 *                   digest of the original decode, else the entry is deleted and the cache turned off.
 * Nothing acts before layout_min plain 2D DDS decodes confirmed the img layout and none contradicted it.
 * Key = absolute source file (getRealDir root + path) + size + mtime + game_build_id(). */
static lg_fmt g_fmt[256];
static SRWLOCK g_fmt_lock = SRWLOCK_INIT;
static volatile LONG g_lay_good, g_lay_bad, g_cache_off, g_c_built, g_c_hits, g_c_verify_ok, g_c_verify_bad, g_c_fail, g_c_nofmt;
static volatile LONG64 g_c_bytes;
static volatile LONG g_c_saved_ms;
static char g_cache_dir[MAX_PATH], g_build[48];
static int layout_ok(void) { return g_lay_good >= g_st.cfg.layout_min && !g_lay_bad; }

static void cache_fail(tls_t *t, const char *why)
{
    InterlockedIncrement(&g_c_fail);
    if (!InterlockedExchange(&g_cache_off, 1))
        api->log("lazy_gfx", "png_cache OFF for this session: %s for '%s' (entry deleted)", why, t->r.path);
    char f[MAX_PATH]; lg_cache_file(f, sizeof f, g_cache_dir, lg_cache_key(t->abs, t->src_size, t->src_mtime, g_build));
    DeleteFileA(f);
}

typedef struct { char path[MAX_PATH]; lg_cache_hdr h; uint8_t *dds; size_t n; } wjob;
static DWORD WINAPI write_job(void *p)
{
    wjob *j = p;
    if (lg_cache_write(j->path, &j->h, j->dds, j->n) == 0) InterlockedIncrement(&g_c_built);
    free(j->dds); free(j);
    return 0;
}

static void dec_pre(tls_t *t, V3X_HookCall *c)
{
    drop_sub(t);
    t->dec_buf = (const uint8_t *)c->args[2]; t->dec_n = (size_t)(uint32_t)c->args[3];
    t->t_dec0 = lg_now_ms(&g_st);
    if (t->dec_buf && t->dec_n >= 128) t->dds_ok = lg_dds_parse(t->dec_buf, t->dec_n, &t->dds);
    int pc = g_st.cfg.png_cache;
    if (pc == LG_PC_OFF || g_cache_off || t->dds_ok || (t->r.ext != 1 && t->r.ext != 2 && t->r.ext != 3)) return;
    if (t->dec_n < (size_t)g_st.cfg.cache_min_kb * 1024 || t->r.root >= g_st.nroots) return;
    char root[260]; snprintf(root, sizeof root, "%s", g_st.roots[t->r.root]);
    size_t rl = strlen(root);
    snprintf(t->abs, sizeof t->abs, "%s%s%s", root, rl && (root[rl - 1] == '/' || root[rl - 1] == '\\') ? "" : "/", t->r.path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(t->abs, GetFileExInfoStandard, &d)) return;
    t->src_size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    t->src_mtime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    if (t->src_size != t->dec_n) return;                     /* not the file the engine read */
    t->cacheable = 1;
    char f[MAX_PATH]; lg_cache_file(f, sizeof f, g_cache_dir, lg_cache_key(t->abs, t->src_size, t->src_mtime, g_build));
    size_t n = 0;
    uint8_t *dds = lg_cache_read(f, t->abs, t->src_size, t->src_mtime, g_build, &t->hdr, &n);
    if (!dds) return;
    t->have_hdr = 1;
    if (pc == LG_PC_ON && layout_ok()) {
        t->sub = dds; t->sub_n = n;
        c->args[2] = (uint64_t)(uintptr_t)dds;               /* decode(&img, &view, buf, size, &opts) */
        c->args[3] = (uint64_t)n;
    } else free(dds);
}

typedef uint64_t (*decode_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static void dec_post(tls_t *t, V3X_HookCall *c)
{
    const uint8_t *img = (const uint8_t *)c->args[0];
    int ok = (c->ret & 0xff) != 0;
    double ms = lg_now_ms(&g_st) - t->t_dec0;
    if (!img) return;
    if (t->sub) {                                            /* we fed the cached DDS */
        lg_digest dg;
        if (!ok) {
            int32_t cnt; memcpy(&cnt, img + LG_IMG_ARRN, 4);
            cache_fail(t, "decode of the cached DDS failed");
            if (cnt == 0 && c->original) {                   /* nothing pushed yet: decode the real source */
                c->ret = ((decode_fn)c->original)(c->args[0], c->args[1], (uint64_t)(uintptr_t)t->dec_buf, t->dec_n, c->args[4]);
                t->r.ok = (c->ret & 0xff) != 0;
            }
            return;
        }
        if (!lg_digest_img(img, &dg, readable) || !lg_digest_eq(&dg, &t->hdr.dg)) { cache_fail(t, "decoded result differs from the original"); return; }
        InterlockedIncrement(&g_c_hits);
        if (t->hdr.decode_ms > ms) InterlockedExchangeAdd(&g_c_saved_ms, (LONG)(t->hdr.decode_ms - ms));
        return;
    }
    if (!ok) return;
    if (t->dds_ok) {                                         /* learn / confirm the img layout from a real DDS */
        int r = lg_img_check_dds(img, &t->dds, t->dec_buf, t->dec_n, readable);
        if (r == 1) {
            InterlockedIncrement(&g_lay_good);
            AcquireSRWLockExclusive(&g_fmt_lock); lg_fmt_learn(g_fmt, img[LG_IMG_FMT], &t->dds); ReleaseSRWLockExclusive(&g_fmt_lock);
        } else if (r == 0 && InterlockedIncrement(&g_lay_bad) <= 3)
            api->log("lazy_gfx", "img layout check FAILED on '%s' (%ux%u, %u mips): png_cache stays inactive", t->r.path, t->dds.w, t->dds.h, t->dds.mips);
        return;
    }
    if (!t->cacheable || g_cache_off || !layout_ok()) return;
    lg_digest dg;
    if (!lg_digest_img(img, &dg, readable)) return;
    if (t->have_hdr) {                                       /* verify: same source, same build -> same output? */
        if (lg_digest_eq(&dg, &t->hdr.dg)) InterlockedIncrement(&g_c_verify_ok);
        else { InterlockedIncrement(&g_c_verify_bad); cache_fail(t, "engine decode differs from the cached digest"); }
        return;
    }
    if ((uint64_t)g_c_bytes >= ((uint64_t)g_st.cfg.cache_max_mb << 20)) return;
    size_t n = 0;
    AcquireSRWLockShared(&g_fmt_lock);
    uint8_t *dds = lg_dds_build(img, g_fmt, &n, readable);
    ReleaseSRWLockShared(&g_fmt_lock);
    if (!dds) { InterlockedIncrement(&g_c_nofmt); return; }
    wjob *j = calloc(1, sizeof *j);
    if (!j) { free(dds); return; }
    lg_cache_file(j->path, sizeof j->path, g_cache_dir, lg_cache_key(t->abs, t->src_size, t->src_mtime, g_build));
    j->h.src_size = t->src_size; j->h.src_mtime = t->src_mtime;
    snprintf(j->h.build, sizeof j->h.build, "%s", g_build);
    snprintf(j->h.src, sizeof j->h.src, "%s", t->abs);
    j->h.decode_ms = (float)ms; j->h.dg = dg;
    j->dds = dds; j->n = n;
    InterlockedExchangeAdd64(&g_c_bytes, (LONG64)n);
    HANDLE th = CreateThread(NULL, 0, write_job, j, 0, NULL);   /* off the loading thread */
    if (th) CloseHandle(th); else write_job(j);
}

static void cache_init(void)
{
    if (g_st.cfg.cache_dir[0]) snprintf(g_cache_dir, sizeof g_cache_dir, "%s", g_st.cfg.cache_dir);
    else snprintf(g_cache_dir, sizeof g_cache_dir, "%scache/lazy_gfx/", g_dir);
    char d[MAX_PATH]; snprintf(d, sizeof d, "%s", g_cache_dir);
    for (char *q = d + 3; *q; q++) if (*q == '/' || *q == '\\') { char k = *q; *q = 0; CreateDirectoryA(d, NULL); *q = k; }
    CreateDirectoryA(d, NULL);
    snprintf(g_build, sizeof g_build, "%s", V3X_HAS(api, game_build_id) && api->game_build_id() ? api->game_build_id() : "unknown");
}

static void pre(V3X_HookCall *c)
{
    int h = (int)(intptr_t)c->user;
    if (c->return_address != g_a.ret[h]) return;
    tls_t *t = tls_get();
    if (!t) return;
    if (h == H_OPTS) { start_rec(t, c); return; }
    if (!t->stage) return;
    switch (h) {
    case H_RDIR:  if (t->stage == E_OPTS_POST) ev(t, E_RD_PRE, LG_ST_OPTS); break;
    case H_MOVE:  if (t->stage >= E_OPTS_POST && t->stage < E_MOVE) ev(t, E_MOVE, LG_ST_OPEN); break;
    case H_DEC:   if (t->stage == E_MOVE) { ev(t, E_DEC_PRE, LG_ST_READ); t->r.bytes = (uint32_t)c->args[3]; dec_pre(t, c); } break;
    case H_PIX:   if (t->stage == E_DEC_POST) ev(t, E_PIX_PRE, LG_ST_DECODE); break;
    case H_OBJ:   if (t->stage == E_PIX_POST || t->stage == E_DEC_POST) ev(t, E_OBJ_PRE, LG_ST_UPLOAD); break;   /* DEC_POST: Vulkan (no DX11 pixbuf call) */
    case H_STAMP: if (t->stage == E_OBJ_PRE) ev(t, E_ST_PRE, LG_ST_OBJ); break;
    }
}

static void post(V3X_HookCall *c)
{
    int h = (int)(intptr_t)c->user;
    if (c->return_address != g_a.ret[h]) return;
    tls_t *t = TlsGetValue(g_tls);
    if (!t || !t->stage) return;
    switch (h) {
    case H_OPTS: if (t->stage == E_OPTS_PRE) ev(t, E_OPTS_POST, LG_ST_OPTS); break;
    case H_RDIR:
        if (t->stage != E_RD_PRE) break;
        ev(t, E_RD_POST, LG_ST_STAT);
        if (c->ret) {                       /* DirHandle*: +8 = dirName (the mounted directory) */
            const uint8_t *dh = (const uint8_t *)c->ret;
            const char *dir = NULL;
            if (readable(dh, 16)) dir = *(const char *const *)(dh + 8);
            if (dir && dir == t->rkey) t->r.root = (uint8_t)t->ridx;
            else if (dir && readable(dir, 1)) {
                char tmp[260]; size_t i = 0;
                while (i < sizeof tmp - 1 && readable(dir + i, 1) && dir[i]) { tmp[i] = dir[i]; i++; }
                tmp[i] = 0;
                t->rkey = dir; t->ridx = lg_root(&g_st, dir, tmp);
                t->r.root = (uint8_t)t->ridx;
            }
        }
        break;
    case H_DEC:
        if (t->stage != E_DEC_PRE) break;
        ev(t, E_DEC_POST, LG_ST_DECODE);
        t->r.ok = (uint8_t)(c->ret & 0xff) ? 1 : 0;
        if (c->args[0]) { memcpy(t->r.img, (const void *)c->args[0], LG_IMG); t->r.has_img = 1; }   /* CreateTexture's stack */
        dec_post(t, c);
        break;
    case H_PIX:   if (t->stage == E_PIX_PRE) ev(t, E_PIX_POST, LG_ST_UPLOAD); break;
    case H_STAMP: if (t->stage == E_ST_PRE) ev(t, E_ST_POST, LG_ST_STAMP); break;
    case H_OBJ:
        if (t->stage != E_OBJ_PRE && t->stage != E_ST_POST) break;
        ev(t, E_OBJ_POST, LG_ST_OBJ);
        if (!c->args[1] || !*(void **)c->args[1]) t->r.ok = 0;
        finish(t, 1);
        break;
    }
}

/* ------------------------------------------------------------------ reports, phases, prefetch */

static void path_in_dir(char *out, size_t n, const char *file) { snprintf(out, n, "%s%s", g_dir, file); }

static void write_reports(const char *why)
{
    static char buf[96 * 1024];
    int n = lg_format_report(&g_st, buf, sizeof buf, 40);
    char p[MAX_PATH], tmp[MAX_PATH + 8];
    path_in_dir(p, sizeof p, "lazy_gfx_report.txt");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "lazy_gfx report (%s) %02d:%02d:%02d mode=%s  RVAs relative to victoria3.exe\n", why, st.wHour, st.wMinute,
                st.wSecond, lg_mode_name(g_st.cfg.mode));
        if (g_st.cfg.png_cache)
            fprintf(f, "png_cache=%s%s: img layout confirmed by %ld DDS decodes, contradicted %ld | built %ld (%.1f MB), hits %ld (saved ~%ld ms decode),"
                       " verify ok %ld / bad %ld, failures %ld, no learned format %ld | dir %s\n",
                    g_st.cfg.png_cache == 1 ? "build" : g_st.cfg.png_cache == 2 ? "verify" : "on", g_cache_off ? " (OFF after a failure)" : "",
                    g_lay_good, g_lay_bad, g_c_built, (double)g_c_bytes / 1048576.0, g_c_hits, g_c_saved_ms, g_c_verify_ok, g_c_verify_bad,
                    g_c_fail, g_c_nofmt, g_cache_dir);
        if (g_pf.n)
            fprintf(f, "prefetch: %d files in manifest, read %ld (%.1f MB), skipped %ld, failed %ld, %.0f ms%s\n", g_pf.n, g_pf.done,
                    (double)g_pf.bytes_read / 1048576.0, g_pf.skipped, g_pf.failed, g_pf.ms, g_pf_running ? " (running)" : "");
        fwrite(buf, 1, (size_t)n, f);
        fclose(f);
        MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING);
    }
    if (g_st.cfg.csv) { path_in_dir(p, sizeof p, "lazy_gfx_textures.csv"); lg_write_csv(&g_st, p); }
    char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
    api->log("lazy_gfx", "report (%s): %s", why, buf);
}

static void write_manifest(void)
{
    if (!g_st.cfg.manifest || InterlockedExchange(&g_manifest_done, 1)) return;
    char p[MAX_PATH]; path_in_dir(p, sizeof p, "lazy_gfx_prefetch.txt");
    int n = lg_write_manifest(&g_st, p, NULL);
    api->log("lazy_gfx", "prefetch manifest: %d files (read %s the first Game load) -> %s", n, g_st.cfg.manifest_game ? "up to the end of" : "before", p);
}

static void on_begin(const char *name, void *data, void *u)
{
    (void)name; (void)u;
    const V3X_LoadTransition *t = data;
    char ph[40]; snprintf(ph, sizeof ph, "load:%s", t && t->to ? t->to : "?");
    lg_set_phase(&g_st, ph);
    if (t && t->to && !_stricmp(t->to, "Game") && !g_st.cfg.manifest_game) write_manifest();
}

static void on_end(const char *name, void *data, void *u)
{
    (void)name; (void)u;
    const V3X_LoadTransition *t = data;
    lg_set_phase(&g_st, t && t->to ? t->to : "?");
    if (t && t->to && !_stricmp(t->to, "Game")) {
        InterlockedExchange(&g_pf.stop, 1);
        if (g_st.cfg.manifest_game) write_manifest();
    }
    write_reports("load transition");
}

static void on_exit_ev(const char *name, void *data, void *u)
{
    (void)name; (void)data; (void)u;
    InterlockedExchange(&g_pf.stop, 1);
    write_reports("process exit");
}

static DWORD WINAPI pf_runner(void *u)
{
    (void)u;
    lg_pf_run(&g_pf, g_st.cfg.prefetch_threads, g_st.cfg.prefetch_low_io);
    api->log("lazy_gfx", "prefetch done: %ld of %d files, %.1f MB in %.0f ms (skipped %ld changed/missing/over budget, failed %ld)",
             g_pf.done, g_pf.n, (double)g_pf.bytes_read / 1048576.0, g_pf.ms, g_pf.skipped, g_pf.failed);
    InterlockedExchange(&g_pf_running, 0);
    return 0;
}

static void start_prefetch(void)
{
    char p[MAX_PATH]; path_in_dir(p, sizeof p, "lazy_gfx_prefetch.txt");
    int n = lg_pf_load(&g_pf, p, (uint64_t)g_st.cfg.prefetch_max_mb << 20);
    if (n <= 0) { api->log("lazy_gfx", "prefetch: no manifest yet (%s); run once to create it", p); return; }
    InterlockedExchange(&g_pf_running, 1);
    HANDLE h = CreateThread(NULL, 0, pf_runner, NULL, 0, NULL);
    if (!h) { InterlockedExchange(&g_pf_running, 0); api->log("lazy_gfx", "prefetch: CreateThread failed (%lu)", GetLastError()); return; }
    CloseHandle(h);
    api->log("lazy_gfx", "prefetch: %d files on %d threads%s", n, g_st.cfg.prefetch_threads, g_st.cfg.prefetch_low_io ? " (low I/O)" : "");
}

static void cmd(const char *args, void *u)
{
    (void)u;
    if (args && !_strnicmp(args, "report", 6)) { write_reports("console"); api->print("lazy_gfx: wrote %slazy_gfx_report.txt", g_dir); return; }
    static char buf[32 * 1024];
    lg_format_report(&g_st, buf, sizeof buf, 10);
    api->print("lazy_gfx mode=%s prefetch %ld/%d (%.1f MB)%s\n%s", lg_mode_name(g_st.cfg.mode), g_pf.done, g_pf.n,
               (double)g_pf.bytes_read / 1048576.0, g_pf_running ? " running" : "", buf);
}

/* ------------------------------------------------------------------ load */

static char *load_ini(void)
{
    char path[MAX_PATH]; path_in_dir(path, sizeof path, "lazy_gfx.ini");
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = n >= 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL;
    if (b) { size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; }
    fclose(f);
    return b;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    snprintf(g_dir, sizeof g_dir, "%s", api->v3x_dir());
    lg_config cfg; lg_config_defaults(&cfg);
    char *ini = load_ini();
    if (ini) {
        char err[160];
        int bad = lg_config_parse(&cfg, ini, err, sizeof err);
        if (bad) api->log("lazy_gfx", "lazy_gfx.ini: %d bad line(s), first: %s", bad, err);
        free(ini);
    }
    if (cfg.mode == LG_OFF) { api->log("lazy_gfx", "mode=off: nothing installed"); return 0; }
    if (!V3X_HAS(api, hook_safe)) { api->log("lazy_gfx", "needs API v4 hook_safe; inactive"); return 0; }
    if (lg_init(&g_st, &cfg)) { api->log("lazy_gfx", "FAIL-CLOSED: no memory for %d records", cfg.max_records); return 0; }
    g_log_left = cfg.log_budget;
    g_main_tid = V3X_HAS(api, main_thread_id) ? api->main_thread_id() : 0;
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) { api->log("lazy_gfx", "FAIL-CLOSED: no TLS slot"); return 0; }
    if (lg_resolve()) { api->log("lazy_gfx", "FAIL-CLOSED: anchors missing, nothing installed"); return 0; }
    int ok = 0;
    /* leaf-most first; a failure unhooks everything installed so far */
    static const int order[H_N] = { H_STAMP, H_PIX, H_DEC, H_MOVE, H_RDIR, H_OBJ, H_OPTS };
    for (; ok < H_N; ok++) {
        int h = order[ok];
        int st = api->hook_safe(g_a.fn[h], pre, post, (void *)(intptr_t)h);
        if (st) {
            api->log("lazy_gfx", "FAIL-CLOSED: hook_safe %s at %08x failed (%d); removing all", H_NAME[h], rva(g_a.fn[h]), st);
            while (ok-- > 0) api->unhook(g_a.fn[order[ok]]);
            return 0;
        }
    }
    api->on_event("load_transition_begin", on_begin, NULL);
    api->on_event("load_transition", on_end, NULL);
    api->on_event("process_exit", on_exit_ev, NULL);
    api->register_command("lazygfx", "lazy_gfx: texture load stage split | report (write v3x/lazy_gfx_report.txt + csv)", cmd, NULL);
    api->log("lazy_gfx", "mode=%s: %d hooks (timing only), main tid %lu", lg_mode_name(cfg.mode), H_N, (unsigned long)g_main_tid);
    if (cfg.mode == LG_ON) start_prefetch();
    if (cfg.png_cache) {
        cache_init();
        api->log("lazy_gfx", "png_cache=%s dir %s build %s (acts after %d confirming DDS decodes)",
                 cfg.png_cache == 1 ? "build" : cfg.png_cache == 2 ? "verify" : "on", g_cache_dir, g_build, cfg.layout_min);
    }
    return 0;
}
