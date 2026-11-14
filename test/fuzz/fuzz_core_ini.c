/* Fuzz harness: the loader's v3x.ini scanner (src/core_ini.h: ini_each, ini_plugin_enabled) and the
 * SDK ini reader (sdk/v3x_ini.h). Invariants: every key/value span handed to the callback lies inside
 * the text and contains no newline; lookups never fail on any input. */
#include "v3x_fuzz.h"
#include "../../src/core_ini.h"
#include "v3x_ini.h"

static const char *g_text;
static size_t g_len;

static void check_kv(const char *k, size_t kn, const char *v, size_t vn, void *ctx)
{
    size_t *count = (size_t *)ctx;
    V3X_FUZZ_ASSERT(k >= g_text && k + kn <= g_text + g_len);
    V3X_FUZZ_ASSERT(v >= g_text && v + vn <= g_text + g_len);
    V3X_FUZZ_ASSERT(!memchr(k, '\n', kn) && !memchr(v, '\n', vn));
    (*count)++;
}

static void fuzz_core_ini(const uint8_t *d, size_t n)
{
    g_text = v3x_fuzz_cstr(d, n);
    g_len = strlen(g_text);
    size_t c = 0;
    static const char *const sections[] = {"", "plugins", "log", "PLUGINS", " plugins "};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) ini_each(g_text, sections[i], check_kv, &c);
    (void)ini_plugin_enabled(g_text, "hello");
    (void)ini_plugin_enabled(g_text, "");
    /* a stem taken from the input itself */
    char stem[32];
    size_t k = n < sizeof stem - 1 ? n : sizeof stem - 1;
    memcpy(stem, d, k);
    stem[k] = 0;
    (void)ini_plugin_enabled(g_text, stem);
}
V3X_FUZZ_TARGET(core_ini, fuzz_core_ini)
V3X_FUZZ_SEED(core_ini, "console=0\nunknown_build = load\n[plugins]\nhello = 0\nfast_load=off\n[log]\nlevel = debug\nv3x = trace\n")
V3X_FUZZ_SEED(core_ini, "\xEF\xBB\xBF[ Plugins ]\r\n  Hello\t=\tno ; c\r\n#x=1\r\n;y=2\r\n[log\nlevel=9\n[]\n=\n")

static void fuzz_sdk_ini(const uint8_t *d, size_t n)
{
    const char *text = v3x_fuzz_cstr(d, n);
    V3xIni ini;
    if (v3x_ini_parse(&ini, text) != 0) return;
    char out[16];
    char sec[24], key[24];
    /* section / key names from the input, so lookups hit real entries */
    size_t a = n > 8 ? 8 : n;
    memcpy(sec, d, a); sec[a] = 0;
    size_t b = n > 16 ? 8 : 0;
    memcpy(key, d + (n > 16 ? 8 : 0), b); key[b] = 0;
    static const char *const keys[] = {"enabled", "mode", "rate", "x", ""};
    for (int i = 0; i < ini.n; i++) {
        V3X_FUZZ_ASSERT(ini.kv[i].section && ini.kv[i].key && ini.kv[i].value);
        (void)v3x_ini_int(&ini, ini.kv[i].section, ini.kv[i].key, 0);
        (void)v3x_ini_double(&ini, ini.kv[i].section, ini.kv[i].key, 0);
        (void)v3x_ini_bool(&ini, ini.kv[i].section, ini.kv[i].key, 0);
        char *s = v3x_ini_str(&ini, ini.kv[i].section, ini.kv[i].key, "d", out, sizeof out);
        V3X_FUZZ_ASSERT(s == out && strlen(out) < sizeof out);
        if (i > 64) break;
    }
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        (void)v3x_ini_int(&ini, sec, keys[i], 1);
        (void)v3x_ini_str(&ini, "", keys[i], NULL, out, sizeof out);
        (void)v3x_ini_bool(&ini, sec, key, 0);
    }
    v3x_ini_free(&ini);
}
V3X_FUZZ_TARGET(sdk_ini, fuzz_sdk_ini)
V3X_FUZZ_SEED(sdk_ini, "; comment\nflat = 5\n[My_Plugin]\nenabled=0\nrate = 2.5 ; inline\nmode = lazy\nhex=0x10\nflag = yes\n")
V3X_FUZZ_SEED(sdk_ini, "[a]\nx=99999999999999999999999\ny=-9223372036854775809\nz=1e400\nw=nan\n[b]\nmode=a;b\n")
