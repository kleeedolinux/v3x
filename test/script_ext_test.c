/* Offline test of plugins/script_ext (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $T/sxt.exe test/script_ext_test.c
 *   $T/sxt.exe <empty temp dir> [path/to/victoria3.exe]
 * A: pack name / relative path validation.  B: BOM rule for *.yml.
 * C: begin/write/end in a temp content_packs tree: folders, atomic overwrite, BOM, manifest, stale files
 *    of the previous session deleted, foreign files kept, errors.
 * D: v3x_plugin_load with the testkit API (root fallback to v3x_dir, console command, export table).
 * E (with the exe): the RE anchors documented in docs/re/script.md still match the exe (RTTI vtables,
 *    strings), read-only on the image mapped as data. Exit 0 = ALL OK. */
#include "v3x_testkit.h"
#include "v3x_rtti.h"
#include "../plugins/script_ext/script_ext.c"

static int file_eq(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t k = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return k == n && !memcmp(buf, data, n);
}
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void part_a(void)
{
    V3X_TEST_CHECK(sx_valid_pack("my_pack") && sx_valid_pack("A-1.b"), "A valid pack names");
    V3X_TEST_CHECK(!sx_valid_pack("") && !sx_valid_pack("_x") && !sx_valid_pack(".x") && !sx_valid_pack("a/b") &&
                   !sx_valid_pack("a b") && !sx_valid_pack(NULL), "A bad pack names rejected");
    char long64[70]; memset(long64, 'a', 64); long64[64] = 0;
    V3X_TEST_CHECK(!sx_valid_pack(long64), "A pack name > 63 chars rejected");
    V3X_TEST_CHECK(sx_valid_rel("common/scripted_effects/x.txt") && sx_valid_rel("events\\e.txt") &&
                   sx_valid_rel("localization/english/x_l_english.yml"), "A valid relative paths");
    const char *bad[] = {"", "/abs.txt", "\\abs.txt", "C:/x.txt", "a/../b.txt", "../x", "a//b", "a/./b", "a/b.",
                         "x?.txt", "a\tb", "v3x_script_ext.manifest", "a/b ", NULL};
    int all = 1;
    for (int i = 0; bad[i]; i++) if (sx_valid_rel(bad[i])) { printf("  accepted: '%s'\n", bad[i]); all = 0; }
    V3X_TEST_CHECK(all && !sx_valid_rel(NULL), "A unsafe paths rejected (absolute, drive, .., empty parts, trailing dot, wildcards, manifest)");
    char longp[260]; memset(longp, 'a', 201); longp[201] = 0;
    V3X_TEST_CHECK(!sx_valid_rel(longp), "A path > 200 chars rejected");
}

static void part_b(void)
{
    V3X_TEST_CHECK(sx_needs_bom("l/x_l_english.yml", "l_english:", 10), "B .yml without BOM gets one");
    V3X_TEST_CHECK(!sx_needs_bom("l/x.YML", "\xEF\xBB\xBFl_english:", 13), "B .yml with BOM unchanged");
    V3X_TEST_CHECK(!sx_needs_bom("common/x.txt", "a", 1), "B .txt never gets a BOM");
}

static void part_c(const char *tmp)
{
    snprintf(sx_root, sizeof sx_root, "%s\\c_packs\\", tmp);
    CreateDirectoryA(sx_root, NULL);
    const char *eff = "v3x_test_effect = { debug_log = \"hello\" }\n";
    V3X_TEST_CHECK(sx_write_file("p1", "common/a.txt", "x", 1) == V3X_SCRIPT_ESTATE, "C write before begin_pack -> ESTATE");
    V3X_TEST_CHECK(sx_begin_pack("p1") == 0, "C begin_pack p1");
    V3X_TEST_CHECK(sx_write_file("p1", "common/scripted_effects/v3x_test.txt", eff, strlen(eff)) == 0, "C write nested file");
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%sp1\\common\\scripted_effects\\v3x_test.txt", sx_root);
    V3X_TEST_CHECK(file_eq(p, eff, strlen(eff)), "C file content exact, folders created");
    V3X_TEST_CHECK(sx_write_file("p1", "common/scripted_effects/v3x_test.txt", "b", 1) == 0 && file_eq(p, "b", 1),
                   "C overwrite replaces the file");
    char tmpf[MAX_PATH + 16]; snprintf(tmpf, sizeof tmpf, "%s.v3xtmp", p);
    V3X_TEST_CHECK(!exists(tmpf), "C no temp file left");
    V3X_TEST_CHECK(sx_write_file("p1", "localization/english/v3x_l_english.yml", "l_english:\n", 11) == 0, "C write .yml");
    snprintf(p, sizeof p, "%sp1\\localization\\english\\v3x_l_english.yml", sx_root);
    V3X_TEST_CHECK(file_eq(p, "\xEF\xBB\xBFl_english:\n", 14), "C .yml written with UTF-8 BOM");
    V3X_TEST_CHECK(sx_write_file("p1", "../escape.txt", "x", 1) == V3X_SCRIPT_EARG, "C escaping path refused");
    V3X_TEST_CHECK(sx_write_file("bad/pack", "a.txt", "x", 1) == V3X_SCRIPT_EARG, "C bad pack refused");
    V3X_TEST_CHECK(sx_end_pack("p1") == 2, "C end_pack returns 2 files (overwrite counted once)");
    snprintf(p, sizeof p, "%sp1\\v3x_script_ext.manifest", sx_root);
    const char *man = "common/scripted_effects/v3x_test.txt\nlocalization/english/v3x_l_english.yml\n";
    V3X_TEST_CHECK(file_eq(p, man, strlen(man)), "C manifest lists both files");
    V3X_TEST_CHECK(sx_write_file("p1", "x.txt", "x", 1) == V3X_SCRIPT_ESTATE, "C write after end_pack -> ESTATE");

    /* a foreign file in the pack, then a new session that writes only one file */
    char foreign[MAX_PATH]; snprintf(foreign, sizeof foreign, "%sp1\\common\\mine.txt", sx_root);
    FILE *f = fopen(foreign, "wb"); if (f) { fputs("keep", f); fclose(f); }
    /* a tampered manifest line must not delete outside the pack */
    char outside[MAX_PATH]; snprintf(outside, sizeof outside, "%s\\outside.txt", tmp);
    f = fopen(outside, "wb"); if (f) { fputs("keep", f); fclose(f); }
    f = fopen(p, "ab"); if (f) { fputs("../../outside.txt\n", f); fclose(f); }
    V3X_TEST_CHECK(sx_begin_pack("p1") == 0, "C second session begin_pack");
    snprintf(p, sizeof p, "%sp1\\common\\scripted_effects\\v3x_test.txt", sx_root);
    char p2[MAX_PATH]; snprintf(p2, sizeof p2, "%sp1\\localization\\english\\v3x_l_english.yml", sx_root);
    V3X_TEST_CHECK(!exists(p) && !exists(p2), "C files of the previous session deleted");
    V3X_TEST_CHECK(exists(foreign), "C foreign file kept");
    V3X_TEST_CHECK(exists(outside), "C '..' line in a tampered manifest ignored");
    V3X_TEST_CHECK(sx_write_file("p1", "events/v3x_events.txt", "namespace = v3x\n", 16) == 0 && sx_end_pack("p1") == 1,
                   "C second session writes 1 file");
    V3X_TEST_CHECK(sx_begin_pack("p2") == 0 && sx_end_pack("p2") == 0, "C empty pack ok");
    V3X_TEST_CHECK(sx_end_pack("nope") == V3X_SCRIPT_ESTATE, "C end_pack of unknown pack -> ESTATE");
    DeleteFileA(outside);
}

static void part_d(const char *tmp)
{
    V3X_API a;
    v3x_testkit_api(&a);
    snprintf(v3x_tk.dir, sizeof v3x_tk.dir, "%s\\v3x\\", tmp);
    CreateDirectoryA(v3x_tk.dir, NULL);
    sx_root[0] = 0;
    int c0 = v3x_tk.commands;
    V3X_TEST_CHECK(v3x_plugin_load(&a) == 0, "D v3x_plugin_load returns 0");
    char want[MAX_PATH]; snprintf(want, sizeof want, "%scontent_packs\\", v3x_tk.dir);
    V3X_TEST_CHECK(!strcmp(sx_root, want) && exists(want), "D root falls back to <v3x_dir>content_packs\\ (test exe is not in plugins\\): %s", sx_root);
    V3X_TEST_CHECK(v3x_tk.commands == c0 + 1 && v3x_tk.hooks == 0 && v3x_tk.patches == 0, "D one console command, no hooks, no patches");
    const V3X_ScriptExt *t = v3x_script_ext();
    V3X_TEST_CHECK(t && t->struct_size == sizeof(V3X_ScriptExt) && t->version == V3X_SCRIPT_VERSION && t->begin_pack &&
                   t->write_file && t->end_pack && !strcmp(t->packs_dir(), want), "D export table complete");
    cmd_scriptext("", NULL);
}

static void part_e(const char *exe)
{
    V3X_API a;
    if (v3x_testkit_map_exe(exe, &a)) { V3X_TEST_CHECK(0, "E map %s", exe); return; }
    V3xRtti r;
    if (v3x_rtti_build_ex(&r, (uintptr_t)v3x_tk.base, v3x_tk.size, v3x_tk.pref_base)) { V3X_TEST_CHECK(0, "E RTTI build"); return; }
    static const struct { const char *cls; unsigned long long rva; } vt[] = {
        {"CJominiEffect", 0x4a80bd8}, {"CJominiTrigger", 0x4a75328}, {"CDebugLogEffect", 0x4a7e6f8},
        {"CDebugLogTrigger", 0x4a74788}, {".?AV?$CEffectEntry@VCDebugLogEffect@@@@", 0x4a77610},
        {".?AV?$CTriggerEntry@VCDebugLogTrigger@@@@", 0x4a775d8},
    };
    for (size_t i = 0; i < sizeof vt / sizeof *vt; i++) {
        void *v = v3x_rtti_find_vtable(&r, vt[i].cls);
        V3X_TEST_CHECK(v && v3x_testkit_rva(v) == vt[i].rva, "E vtable %s at %llx (doc %llx)", vt[i].cls, v3x_testkit_rva(v), vt[i].rva);
    }
    int ne = 0, nt = 0;
    ne = v3x_rtti_enum(&r, ".?AV?$CEffectEntry@", NULL, NULL);
    nt = v3x_rtti_enum(&r, ".?AV?$CTriggerEntry@", NULL, NULL);
    V3X_TEST_CHECK(ne == 709 && nt == 1054, "E RTTI counts: %d CEffectEntry<>, %d CTriggerEntry<> (doc 709 / 1054)", ne, nt);
    v3x_rtti_free(&r);
    /* strings named in the doc, and the absence of vanilla names in the v3x_ namespace the guide reserves */
    const char *s[] = {"debug_log", "Effect Runner", "Trigger Runner", "RunEffect", NULL};
    for (int i = 0; s[i]; i++) {
        void *p = a.find_string(s[i]);
        void *x = p ? a.find_xref(p, 0) : NULL;
        V3X_TEST_CHECK(p && x, "E string \"%s\" present and referenced (fn %llx)", s[i], v3x_testkit_rva(x ? a.function_start(x) : NULL));
    }
    int v3x_names = 0;
    for (int k = 0; k < v3x_tk.ndata; k++)
        for (size_t i = 1; i + 4 < v3x_tk.data[k].n; i++)
            if (v3x_tk.data[k].p[i - 1] == 0 && !memcmp(v3x_tk.data[k].p + i, "v3x_", 4)) v3x_names++;
    V3X_TEST_CHECK(v3x_names == 0, "E no vanilla string starts with \"v3x_\" (%d)", v3x_names);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <empty temp dir> [victoria3.exe]\n", argv[0]); return 2; }
    part_a();
    part_b();
    part_c(argv[1]);
    part_d(argv[1]);
    if (argc > 2) part_e(argv[2]);
    return v3x_testkit_done();
}
