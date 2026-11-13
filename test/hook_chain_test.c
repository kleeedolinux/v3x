/* Unit test for the hook registry in src/safe_hook.c (several hook_safe subscribers on one target,
 * exclusive raw hooks, per-owner unhook). No game needed.
 *   M=third_party/minhook
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -I$M/include -o $T/hct.exe \
 *       test/hook_chain_test.c $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c
 *   $T/hct.exe
 * Checks: two subscribers see every call with the right args/return/user, pre in order and post
 * in reverse, args/ret changes pass along the chain, per-subscriber scratch, skip_original (first
 * wins), float return through the dispatcher, recursion, duplicate and 9th subscriber refused,
 * removing one owner keeps the other, removing all restores the original bytes, re-subscribing,
 * raw hook on a busy target (and hook_safe on a raw-hooked one, unhook by a non-owner) ->
 * V3X_EHOOKBUSY with the owner, and 8 threads calling while subscribers come and go (also the
 * last one, so the thunk is disabled/enabled under load). Owners are fake module handles here.
 * Exit code 0 = ALL OK. */
#include "../src/safe_hook.c"
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

#define NOINL __attribute__((noinline))
typedef long long ll;
static volatile ll sink_;
NOINL ll f4(ll a, ll b, ll c, ll d) { sink_++; return a + b * 10 + c * 100 + d * 1000; }
NOINL ll f6(ll a, ll b, ll c, ll d, ll e, ll f) { sink_++; return f4(a, b, c, d) + e * 10000 + f * 100000; }
NOINL ll f2(ll a, ll b) { sink_++; return a * 3 + b * 5; }
NOINL ll fraw(ll a) { sink_++; return a * 7 + 1; }
NOINL double fd2(double a, double b) { sink_++; return a + b * 2; }
extern ll (*volatile prec)(ll);
NOINL ll frec(ll n) { sink_++; return n <= 0 ? 0 : n + prec(n - 1); }

static ll (*volatile p4)(ll, ll, ll, ll) = f4;
static ll (*volatile p6)(ll, ll, ll, ll, ll, ll) = f6;
static ll (*volatile p2)(ll, ll) = f2;
static ll (*volatile praw)(ll) = fraw;
static double (*volatile pd2)(double, double) = fd2;
ll (*volatile prec)(ll) = frec;
static ll (*raw_orig)(ll a, ll b, ll c, ll d);
NOINL ll fdet4(ll a, ll b, ll c, ll d) { return raw_orig(a, b, c, d) + 1; }   /* raw detour on f4 */

#define OWN(n) ((void *)(uintptr_t)(0x1000 * (n)))

/* ---- order log */
static char g_order[64];
static int g_olen;
static void olog(char ch) { if (g_olen < 63) g_order[g_olen++] = ch, g_order[g_olen] = 0; }
static void oreset(void) { g_olen = 0; g_order[0] = 0; }

typedef struct {
    char name;              /* pre logs name, post logs lower-case */
    volatile LONG pre, post;
    ll a[6], ret;           /* last args seen by pre, ret seen by post */
    int bad_user, bad_scratch;
    int skip;               /* pre sets skip_original and ret = skip */
    int double_a0, add_ret;
} Sub;

static void sub_pre(V3X_HookCall *c)
{
    Sub *s = c->user;
    InterlockedIncrement(&s->pre);
    olog(s->name);
    for (int i = 0; i < 6; i++) s->a[i] = (ll)c->args[i];
    if (c->scratch[0] || c->scratch[1]) s->bad_scratch++;          /* each subscriber starts at zero */
    c->scratch[0] = (uint64_t)(uintptr_t)s; c->scratch[1] = 0x5c5c;
    if (s->double_a0) c->args[0] *= 2;
    if (s->skip) { c->skip_original = 1; c->ret = (uint64_t)s->skip; }
}
static void sub_post(V3X_HookCall *c)
{
    Sub *s = c->user;
    InterlockedIncrement(&s->post);
    olog((char)(s->name | 0x20));
    s->ret = (ll)c->ret;
    if (c->scratch[0] != (uint64_t)(uintptr_t)s || c->scratch[1] != 0x5c5c) s->bad_scratch++;
    s->ret = (ll)c->ret;
    if (s->add_ret) c->ret += (uint64_t)s->add_ret;
}
static void post_only(V3X_HookCall *c)
{
    Sub *s = c->user;
    InterlockedIncrement(&s->post);
    olog((char)(s->name | 0x20));
    if (c->scratch[0] || c->scratch[1]) s->bad_scratch++;         /* no pre: its scratch stays zero */
    s->ret = (ll)c->ret;
}
static void dbl_post(V3X_HookCall *c) { Sub *s = c->user; InterlockedIncrement(&s->post); c->xmm_ret.d += 0.25; }

static int add(void *own, void *t, v3x_hook_fn pre, v3x_hook_fn post, void *u)
{
    void *b;
    int s = v3x_hook_add_safe(own, t, pre, post, u, &b);
    if (s) printf("  add %p -> %d (busy %p)\n", t, s, b);
    return s;
}

/* ---- concurrency */
#define NT 8
#define NROUNDS 400
static volatile LONG g_stop, g_wrong;
static volatile LONG64 g_ccalls;
typedef struct { volatile LONG pre, post, bad; } CSub;
static CSub g_cs[4];
static void c_pre(V3X_HookCall *c)
{
    CSub *s = c->user;
    InterlockedIncrement(&s->pre);
    if (c->scratch[0]) InterlockedIncrement(&s->bad);
    c->scratch[0] = (uint64_t)(uintptr_t)s ^ GetCurrentThreadId();
}
static void c_post(V3X_HookCall *c)
{
    CSub *s = c->user;
    InterlockedIncrement(&s->post);
    if (c->scratch[0] != ((uint64_t)(uintptr_t)s ^ GetCurrentThreadId())) InterlockedIncrement(&s->bad);
}
static DWORD WINAPI caller_thread(LPVOID p)
{
    ll k = (ll)(uintptr_t)p;
    for (int i = 0; !g_stop; i++) {
        ll a = k * 1000 + (i & 255), b = i;
        if (p2(a, b) != a * 3 + b * 5) InterlockedIncrement(&g_wrong);
        InterlockedIncrement64(&g_ccalls);
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(MH_Initialize() == MH_OK, "MH_Initialize");
    void *busy;
    uint8_t orig4[16];
    memcpy(orig4, (void *)f4, sizeof orig4);

    /* bad args */
    CHECK(v3x_hook_add_safe(NULL, (void *)f4, sub_pre, NULL, NULL, &busy) == -1, "owner NULL refused");
    CHECK(v3x_hook_add_safe(OWN(1), NULL, sub_pre, NULL, NULL, &busy) == -1, "target NULL refused");
    CHECK(v3x_hook_add_safe(OWN(1), (void *)f4, NULL, NULL, NULL, &busy) == -1, "no pre and no post refused");
    CHECK(v3x_hook_remove(OWN(1), (void *)f4, &busy) == MH_ERROR_NOT_CREATED, "unhook of an unhooked target -> MH_ERROR_NOT_CREATED");

    /* two subscribers */
    Sub A = {.name = 'A'}, B = {.name = 'B'};
    CHECK(add(OWN(1), (void *)f4, sub_pre, sub_post, &A) == 0, "subscriber A (owner 1)");
    CHECK(add(OWN(2), (void *)f4, sub_pre, sub_post, &B) == 0, "subscriber B (owner 2) on the same target");
    CHECK(v3x_hook_subscribers((void *)f4) == 2, "2 subscribers");
    int ok = 1;
    for (int i = 0; i < 100; i++) {
        oreset();
        ll r = p4(i, 2, 3, 4), want = i + 4320;   /* not f4(): a direct call would go through the hook too */
        if (r != want || A.a[0] != i || B.a[0] != i || A.a[3] != 4 || B.a[1] != 2 || A.ret != want || B.ret != want ||
            strcmp(g_order, "ABba"))
            ok = 0;
    }
    CHECK(ok && A.pre == 100 && A.post == 100 && B.pre == 100 && B.post == 100,
          "both see all 100 calls with right args/return; order %s (pre in order, post reversed)", g_order);
    CHECK(!A.bad_scratch && !B.bad_scratch, "each subscriber has its own zeroed scratch, kept pre -> post");

    /* chain effects: A doubles a0 (B sees it), B adds 1000 to ret (A sees it), A adds 1 (caller sees both) */
    A.double_a0 = 1; B.add_ret = 1000; A.add_ret = 1;
    ll r = p4(5, 0, 0, 0);
    CHECK(B.a[0] == 10 && B.ret == 10 && A.ret == 1010 && r == 1011, "args/ret changes pass along (B a0=%lld, A ret=%lld, caller %lld)",
          B.a[0], A.ret, r);
    A.double_a0 = 0; B.add_ret = 0; A.add_ret = 0;

    /* skip: B skips -> A post + B post run, original not called; first skip wins */
    Sub C = {.name = 'C'};
    CHECK(add(OWN(3), (void *)f4, sub_pre, sub_post, &C) == 0, "subscriber C (owner 3)");
    B.skip = 555; C.skip = 999;
    ll s0 = sink_;
    oreset(); r = p4(1, 1, 1, 1);
    CHECK(r == 555 && sink_ == s0 && !strcmp(g_order, "ABba"), "B skips: ret 555, original not run, C not called, order %s", g_order);
    B.skip = 0; A.skip = 444;
    oreset(); r = p4(1, 1, 1, 1);
    CHECK(r == 444 && !strcmp(g_order, "Aa"), "A skips first: B and C never see the call (order %s)", g_order);
    A.skip = 0; C.skip = 0;
    oreset(); r = p4(1, 1, 1, 1);
    CHECK(r == 1111 && !strcmp(g_order, "ABCcba"), "3 subscribers nest: %s", g_order);
    CHECK(v3x_hook_remove(OWN(3), (void *)f4, &busy) == 0 && v3x_hook_subscribers((void *)f4) == 2, "owner 3 leaves");

    /* duplicate, post-only, cap */
    CHECK(v3x_hook_add_safe(OWN(1), (void *)f4, sub_pre, sub_post, &A, &busy) == MH_ERROR_ALREADY_CREATED,
          "same owner/pre/post/user twice -> MH_ERROR_ALREADY_CREATED");
    Sub P = {.name = 'P'};
    CHECK(add(OWN(4), (void *)f4, NULL, post_only, &P) == 0, "post-only subscriber");
    oreset(); r = p4(1, 2, 0, 0);
    CHECK(r == 21 && !strcmp(g_order, "ABpba") && P.ret == 21 && !P.bad_scratch, "post-only runs innermost, sees ret (%s)", g_order);
    Sub X[8];
    int added = 0;
    for (int i = 0; i < 8; i++) {
        X[i] = (Sub){.name = 'Q'};
        if (v3x_hook_add_safe(OWN(10 + i), (void *)f4, sub_pre, NULL, &X[i], &busy) == 0) added++;
        else { CHECK(v3x_hook_subscribers((void *)f4) == V3X_HOOK_MAX_SUBSCRIBERS, "subscriber %d refused at the cap", 4 + i); break; }
    }
    CHECK(added == V3X_HOOK_MAX_SUBSCRIBERS - 3, "cap of %d subscribers (%d more fit)", V3X_HOOK_MAX_SUBSCRIBERS, added);
    for (int i = 0; i < added; i++) v3x_hook_remove(OWN(10 + i), (void *)f4, &busy);
    v3x_hook_remove(OWN(4), (void *)f4, &busy);
    CHECK(v3x_hook_subscribers((void *)f4) == 2, "back to A and B");

    /* raw hooks are exclusive */
    void *orig_raw = NULL;
    CHECK(v3x_hook_add_raw(OWN(5), (void *)f4, (void *)fraw, &orig_raw, &busy) == V3X_EHOOKBUSY && busy == OWN(1),
          "raw hook on a hook_safe target -> V3X_EHOOKBUSY, owner = first subscriber");
    CHECK(v3x_hook_remove(OWN(5), (void *)f4, &busy) == V3X_EHOOKBUSY && busy == OWN(1) && v3x_hook_subscribers((void *)f4) == 2,
          "unhook by a non-owner -> V3X_EHOOKBUSY, nothing removed");

    /* remove one, keep the other */
    CHECK(v3x_hook_remove(OWN(1), (void *)f4, &busy) == 0, "owner 1 unhooks");
    LONG a0 = A.pre, b0 = B.pre;
    oreset(); r = p4(3, 0, 0, 0);
    CHECK(r == 3 && A.pre == a0 && B.pre == b0 + 1 && !strcmp(g_order, "Bb"), "B still sees calls, A no longer (%s)", g_order);
    CHECK(memcmp(orig4, (void *)f4, 5) != 0, "target still patched");
    CHECK(v3x_hook_remove(OWN(2), (void *)f4, &busy) == 0, "owner 2 unhooks (last)");
    CHECK(memcmp(orig4, (void *)f4, sizeof orig4) == 0, "original bytes restored");
    b0 = B.pre;
    CHECK(p4(1, 2, 3, 4) == 4321 && B.pre == b0, "unhooked f4 runs plain");
    CHECK(v3x_hook_remove(OWN(2), (void *)f4, &busy) == MH_ERROR_NOT_CREATED, "second unhook -> MH_ERROR_NOT_CREATED");

    /* re-subscribe (re-enables the kept thunk) */
    CHECK(add(OWN(2), (void *)f4, sub_pre, sub_post, &B) == 0 && p4(1, 0, 0, 0) == 1 && B.pre == b0 + 1, "re-subscribe after full removal");
    CHECK(v3x_hook_remove(OWN(2), (void *)f4, &busy) == 0 && memcmp(orig4, (void *)f4, sizeof orig4) == 0, "and leave again");

    /* raw hook after the thunk left, then hook_safe / raw on the raw-hooked target */
    CHECK(v3x_hook_add_raw(OWN(5), (void *)f4, (void *)fdet4, (void **)&raw_orig, &busy) == 0, "raw hook once the target is free");
    CHECK(p4(1, 2, 3, 4) == 4322 && raw_orig && raw_orig(1, 2, 3, 4) == 4321, "raw detour and trampoline work");
    CHECK(v3x_hook_subscribers((void *)f4) == -1, "registry reports a raw hook");
    CHECK(v3x_hook_add_safe(OWN(1), (void *)f4, sub_pre, NULL, &A, &busy) == V3X_EHOOKBUSY && busy == OWN(5),
          "hook_safe on a raw-hooked target -> V3X_EHOOKBUSY, owner 5");
    CHECK(v3x_hook_add_raw(OWN(6), (void *)f4, (void *)fraw, &orig_raw, &busy) == V3X_EHOOKBUSY && busy == OWN(5),
          "second raw hook -> V3X_EHOOKBUSY, owner 5");
    CHECK(v3x_hook_remove(OWN(6), (void *)f4, &busy) == V3X_EHOOKBUSY && busy == OWN(5), "raw unhook by a non-owner refused");
    CHECK(v3x_hook_remove(OWN(5), (void *)f4, &busy) == 0 && memcmp(orig4, (void *)f4, sizeof orig4) == 0,
          "raw owner unhooks: bytes restored");
    a0 = A.pre;
    CHECK(add(OWN(1), (void *)f4, sub_pre, sub_post, &A) == 0 && p4(2, 0, 0, 0) == 2 && A.pre == a0 + 1,
          "hook_safe again after the raw hook (thunk re-created)");
    CHECK(v3x_hook_remove(OWN(1), (void *)f4, &busy) == 0 && memcmp(orig4, (void *)f4, sizeof orig4) == 0, "clean");

    /* a target hooked outside the registry */
    CHECK(MH_CreateHook((void *)fraw, (void *)f2, NULL) == MH_OK, "foreign MinHook hook on fraw");
    CHECK(v3x_hook_add_safe(OWN(1), (void *)fraw, sub_pre, NULL, &A, &busy) == V3X_EHOOKBUSY && busy == NULL,
          "hook_safe there -> V3X_EHOOKBUSY, owner unknown (NULL)");
    CHECK(v3x_hook_add_raw(OWN(1), (void *)fraw, (void *)f2, NULL, &busy) == V3X_EHOOKBUSY && busy == NULL,
          "raw hook there -> V3X_EHOOKBUSY, owner unknown");
    MH_RemoveHook((void *)fraw);
    CHECK(praw(1) == 8, "fraw plain");

    /* float return, 6 args, recursion through the shared thunk */
    Sub D1 = {.name = 'D'}, D2 = {.name = 'E'};
    CHECK(add(OWN(1), (void *)fd2, NULL, dbl_post, &D1) == 0 && add(OWN(2), (void *)fd2, NULL, dbl_post, &D2) == 0, "two on fd2");
    double d = pd2(1.0, 2.0);
    CHECK(d == 5.5 && D1.post == 1 && D2.post == 1, "double return through both posts: %g", d);
    v3x_hook_remove(OWN(1), (void *)fd2, &busy); v3x_hook_remove(OWN(2), (void *)fd2, &busy);
    Sub S6a = {.name = 'F'}, S6b = {.name = 'G'};
    add(OWN(1), (void *)f6, sub_pre, sub_post, &S6a); add(OWN(2), (void *)f6, sub_pre, sub_post, &S6b);
    r = p6(1, 2, 3, 4, 5, 6);
    CHECK(r == 654321 && S6b.a[4] == 5 && S6b.a[5] == 6 && S6a.ret == 654321, "6 args (stack slots) through the chain");
    v3x_hook_remove(OWN(1), (void *)f6, &busy); v3x_hook_remove(OWN(2), (void *)f6, &busy);
    Sub R1 = {.name = 'R'}, R2 = {.name = 'S'};
    add(OWN(1), (void *)frec, sub_pre, sub_post, &R1); add(OWN(2), (void *)frec, sub_pre, sub_post, &R2);
    r = prec(10);
    CHECK(r == 55 && R1.pre == 11 && R1.post == 11 && R2.pre == 11 && R2.post == 11 && !R1.bad_scratch && !R2.bad_scratch,
          "recursion: 11 nested calls, each subscriber paired with its own scratch");
    v3x_hook_remove(OWN(1), (void *)frec, &busy); v3x_hook_remove(OWN(2), (void *)frec, &busy);

    /* concurrency: 8 threads call f2 while subscribers (also the last one) come and go */
    uint8_t orig2[16];
    memcpy(orig2, (void *)f2, sizeof orig2);
    HANDLE th[NT];
    for (int i = 0; i < NT; i++) th[i] = CreateThread(NULL, 0, caller_thread, (LPVOID)(uintptr_t)(i + 1), 0, NULL);
    int rounds = 0, errs = 0;
    while (rounds < NROUNDS) {
        int k = rounds % 4;
        void *own = OWN(20 + k);
        if (rounds & 4) { if (v3x_hook_remove(own, (void *)f2, &busy) != 0) errs++; }
        else if (v3x_hook_add_safe(own, (void *)f2, c_pre, c_post, &g_cs[k], &busy) != 0) errs++;
        rounds++;
        if (!(rounds & 7)) Sleep(0);
    }
    g_stop = 1;
    WaitForMultipleObjects(NT, th, TRUE, INFINITE);
    for (int i = 0; i < NT; i++) CloseHandle(th[i]);
    LONG tp = 0, tq = 0, tb = 0;
    for (int k = 0; k < 4; k++) { tp += g_cs[k].pre; tq += g_cs[k].post; tb += g_cs[k].bad; }
    CHECK(g_ccalls > NROUNDS && g_wrong == 0, "%d threads, %lld calls, all returns right (%ld wrong)", NT, (long long)g_ccalls, g_wrong);
    CHECK(errs == 0 && rounds > 8, "%d add/remove rounds during the calls, none failed", rounds);
    CHECK(tp == tq && tb == 0 && tp > 0, "every pre paired with its post on the same thread/scratch (%ld pre, %ld post, %ld bad)", tp, tq, tb);
    for (int k = 0; k < 4; k++) v3x_hook_remove(OWN(20 + k), (void *)f2, &busy);
    CHECK(v3x_hook_subscribers((void *)f2) == 0 && memcmp(orig2, (void *)f2, sizeof orig2) == 0, "f2 bytes restored after the run");

    /* v3x_hook_remove_owner (loader rollback of a failed plugin): everything of one owner, nothing else */
    uint8_t o4[16], o6[16], or_[16];
    memcpy(o4, (void *)f4, 16); memcpy(o6, (void *)f6, 16); memcpy(or_, (void *)fraw, 16);
    void *orig_r = NULL;
    Sub ra = {0}, rb = {0};
    CHECK(add(OWN(30), (void *)f4, sub_pre, sub_post, &ra) == 0 && add(OWN(31), (void *)f4, sub_pre, sub_post, &rb) == 0 &&
              add(OWN(30), (void *)f6, NULL, post_only, &ra) == 0 &&
              v3x_hook_add_raw(OWN(30), (void *)fraw, (void *)f2, &orig_r, &busy) == 0,
          "owner 30: hook_safe on f4 + f6 and a raw hook on fraw; owner 31 shares f4");
    CHECK(v3x_hook_remove_owner(OWN(30)) == 3, "remove_owner(30) releases its 3 targets");
    CHECK(v3x_hook_subscribers((void *)f4) == 1 && v3x_hook_subscribers((void *)f6) == 0 && v3x_hook_subscribers((void *)fraw) == 0 &&
              memcmp(o6, (void *)f6, 16) == 0 && memcmp(or_, (void *)fraw, 16) == 0 && praw(2) == 15,
          "owner 31's subscription on f4 stays; f6 and fraw restored");
    rb.pre = 0;
    p4(1, 2, 3, 4);
    CHECK(rb.pre == 1 && v3x_hook_remove_owner(OWN(30)) == 0, "f4 still calls owner 31; a second remove_owner finds nothing");
    CHECK(v3x_hook_remove_owner(OWN(31)) == 1 && memcmp(o4, (void *)f4, 16) == 0 && v3x_hook_remove_owner(NULL) == 0,
          "remove_owner(31) restores f4; NULL owner is a no-op");

    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
