/* console_plus: country wildcards in game console commands (pure logic, no Windows, no engine).
 *  - cp_wild_parse: "*" (every country that is a subject of anyone), "M:*" (subjects of the
 *    player's country), "TAG:*" (subjects of TAG), "ALL" (every country)
 *  - cp_wild_scan: finds the one wildcard token of a line in an argument of country kind
 *  - cp_wild_expand: the matching countries from a {tag, id, overlord id} list
 *  - cp_wild_line: the line with the wildcard token replaced by one tag
 *  - cp_decode_overlord: decodes the engine's GetOverlord (country id -> overlord id map in the
 *    game state) from its code */
#ifndef CP_WILD_H
#define CP_WILD_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CP_NO_ID 0xffffffffu

typedef enum { CW_NONE, CW_SUBJECTS, CW_SUBJECTS_OF_PLAYER, CW_SUBJECTS_OF_TAG, CW_ALL } CpWildKind;
typedef struct { CpWildKind k; char tag[16]; } CpWild;
typedef struct { char tag[16]; uint32_t id, overlord; } CpCountry;   /* overlord CP_NO_ID = independent */

static int cp_wild_tagchar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* token (n bytes) -> wildcard kind; CW_NONE if it is not one */
static CpWildKind cp_wild_parse(const char *t, size_t n, CpWild *w)
{
    memset(w, 0, sizeof *w);
    if (n == 1 && t[0] == '*') w->k = CW_SUBJECTS;
    else if (n == 3 && (t[0] == 'A' || t[0] == 'a') && (t[1] == 'L' || t[1] == 'l') && (t[2] == 'L' || t[2] == 'l')) w->k = CW_ALL;
    else if (n >= 3 && n - 2 < sizeof w->tag && t[n - 2] == ':' && t[n - 1] == '*') {
        size_t tl = n - 2;
        for (size_t i = 0; i < tl; i++) if (!cp_wild_tagchar(t[i])) return CW_NONE;
        if (tl == 1 && (t[0] == 'M' || t[0] == 'm')) w->k = CW_SUBJECTS_OF_PLAYER;
        else {
            for (size_t i = 0; i < tl; i++) w->tag[i] = (char)(t[i] >= 'a' && t[i] <= 'z' ? t[i] - 32 : t[i]);
            w->tag[tl] = 0;
            w->k = CW_SUBJECTS_OF_TAG;
        }
    }
    return w->k;
}

/* Is the argument `argi` (0 = first after the command) of `cmd` a country, and is `tok` not one of
 * its literal words? Supplied by the caller. */
typedef int (*cp_wild_argok_fn)(const char *cmd, int argi, const char *tok, size_t tn, void *u);

/* The wildcard of `line` ("cmd a b ..."; blanks separate tokens). Returns 1 and its byte range
 * [*ts, *te) + kind, 0 = none, -1 = more than one wildcard (not expanded). */
static int cp_wild_scan(const char *line, cp_wild_argok_fn ok, void *u, size_t *ts, size_t *te, CpWild *w)
{
    char cmd[64];
    size_t i = 0, n = strlen(line);
    while (i < n && line[i] == ' ') i++;
    size_t cs = i;
    while (i < n && line[i] != ' ') i++;
    if (i == cs || i - cs >= sizeof cmd) return 0;
    memcpy(cmd, line + cs, i - cs); cmd[i - cs] = 0;
    int found = 0, argi = 0;
    while (i < n) {
        while (i < n && line[i] == ' ') i++;
        if (i >= n) break;
        size_t s = i;
        while (i < n && line[i] != ' ') i++;
        CpWild x;
        if (cp_wild_parse(line + s, i - s, &x) != CW_NONE && ok(cmd, argi, line + s, i - s, u)) {
            if (found) return -1;
            found = 1; *ts = s; *te = i; *w = x;
        }
        argi++;
    }
    return found;
}

static int cp_wild_tageq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* indexes into c[] of the countries the wildcard names (in list order, at most max).
 * Returns the count, -1 if the player (M:*) or TAG (TAG:*) is not in the list. */
static int cp_wild_expand(const CpWild *w, const CpCountry *c, int n, uint32_t player, int *out, int max)
{
    uint32_t over = CP_NO_ID;
    if (w->k == CW_SUBJECTS_OF_PLAYER) {
        for (int i = 0; i < n; i++) if (c[i].id == player) over = player;
        if (over == CP_NO_ID) return -1;
    } else if (w->k == CW_SUBJECTS_OF_TAG) {
        for (int i = 0; i < n && over == CP_NO_ID; i++) if (cp_wild_tageq(c[i].tag, w->tag)) over = c[i].id;
        if (over == CP_NO_ID) return -1;
    }
    int m = 0;
    for (int i = 0; i < n && m < max; i++) {
        if (!c[i].tag[0]) continue;
        int take = 0;
        switch (w->k) {
        case CW_ALL: take = 1; break;
        case CW_SUBJECTS: {                      /* overlord must be a listed country */
            if (c[i].overlord == CP_NO_ID || c[i].overlord == c[i].id) break;
            for (int j = 0; j < n && !take; j++) take = c[j].id == c[i].overlord;
            break;
        }
        case CW_SUBJECTS_OF_PLAYER: case CW_SUBJECTS_OF_TAG: take = c[i].overlord == over && c[i].id != over; break;
        default: break;
        }
        if (take) out[m++] = i;
    }
    return m;
}

/* line with [ts, te) replaced by `tag`; 0 if it does not fit */
static int cp_wild_line(const char *line, size_t ts, size_t te, const char *tag, char *out, size_t cap)
{
    int r = snprintf(out, cap, "%.*s%s%s", (int)ts, line, tag, line + te);
    return r > 0 && (size_t)r < cap;
}

/* ---- the engine's GetOverlord(country) (1.15.1 0x93a030, from fn+0x2a):
 *   mov rax,[rip+R]; mov rdx,[rax+608h]; mov r8,[rdx+120h]; mov edx,[rbx+18h]; mov eax,edx;
 *   and eax,0FFFFFFh; cmp eax,[r8+CNT]; jae; mov ecx,eax; mov rax,[r8+KEYS]; cmp [rax+rcx*8],edx;
 *   jne; movsxd rcx,[rax+rcx*8+4]; mov rax,[r8+VALS]; cmp byte [rax+rcx*8+4],0; lea rax,[rax+rcx*8];
 *   je; mov eax,[rax]
 * = game state G -> id index (id & 0xffffff) < [G+CNT], keys {int id, int slot} at [G+KEYS],
 *   values {int overlord id, bool set} at [G+VALS]. -1 = byte wildcard. */
static const int16_t CP_OVL_PAT[] = {
    0x48,0x8B,0x05,-1,-1,-1,-1, 0x48,0x8B,0x90,0x08,0x06,0x00,0x00, 0x4C,0x8B,0x82,0x20,0x01,0x00,0x00,
    0x8B,0x53,0x18, 0x8B,0xC2, 0x25,0xFF,0xFF,0xFF,0x00, 0x41,0x3B,0x80,-1,-1,-1,-1, 0x73,-1, 0x8B,0xC8,
    0x49,0x8B,0x80,-1,-1,-1,-1, 0x39,0x14,0xC8, 0x75,-1, 0x48,0x63,0x4C,0xC8,0x04,
    0x49,0x8B,0x80,-1,-1,-1,-1, 0x80,0x7C,0xC8,0x04,0x00, 0x48,0x8D,0x04,0xC8, 0x74,-1, 0x8B,0x00 };
enum { CP_OVL_ROOT = 3, CP_OVL_CNT = 34, CP_OVL_KEYS = 45, CP_OVL_VALS = 62 };

static int cp_decode_overlord(const uint8_t *code, size_t n, const uint8_t **root, uint32_t *cnt, uint32_t *keys, uint32_t *vals)
{
    size_t pl = sizeof CP_OVL_PAT / sizeof CP_OVL_PAT[0];
    for (size_t i = 0; i + pl <= n; i++) {
        size_t j = 0;
        while (j < pl && (CP_OVL_PAT[j] < 0 || code[i + j] == (uint8_t)CP_OVL_PAT[j])) j++;
        if (j < pl) continue;
        int32_t rel; memcpy(&rel, code + i + CP_OVL_ROOT, 4);
        *root = code + i + CP_OVL_ROOT + 4 + rel;
        memcpy(cnt, code + i + CP_OVL_CNT, 4); memcpy(keys, code + i + CP_OVL_KEYS, 4); memcpy(vals, code + i + CP_OVL_VALS, 4);
        return *cnt < 0x100000 && *keys < 0x100000 && *vals < 0x100000;
    }
    return 0;
}

/* ---- the player country id as the console commands read it (add_liberty_desire, add_relations, ...):
 *   mov r,[rip+R]; mov r15,[r+608h]; mov edx,53Ch; mov rax,gs:[58h]; mov rcx,[rax]; cmp dword [rdx+rcx],1;
 *   mov eax,-1; je +N; mov eax,[r15+OFF]
 * Returns 1 with the root R and OFF (the id is [[R]+0x608]+OFF). */
static const uint8_t CP_PLY_TLS[] = {0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00,0x48,0x8B,0x08,0x83,0x3C,0x0A,0x01,0xB8,0xFF,0xFF,0xFF,0xFF,0x74};
static int cp_decode_player(const uint8_t *code, size_t n, const uint8_t **root, uint32_t *off)
{
    size_t tl = sizeof CP_PLY_TLS;
    for (size_t i = 20; i + tl + 9 <= n; i++) {
        if (memcmp(code + i, CP_PLY_TLS, tl)) continue;
        if (code[i - 5] != 0xBA) continue;                                  /* mov edx,imm32 */
        /* mov e?x,[r+disp32] right after the je rel8: [REX.B] 8B modrm(mod=10, reg=eax) disp32 */
        const uint8_t *p = code + i + tl + 1;
        int rexb = 0;
        if (*p == 0x41) { rexb = 1; p++; } else if (*p >= 0x40 && *p <= 0x4F) continue;
        if (p[0] != 0x8B || (p[1] & 0xC0) != 0x80 || (p[1] & 0x38) != 0 || (p[1] & 7) == 4) continue;
        int reg = (p[1] & 7) | (rexb << 3);
        uint32_t d; memcpy(&d, p + 2, 4);
        /* before the TLS read: mov reg,[rX+608h] (REX.W), and before that mov rX,[rip+R] */
        const uint8_t *q = code + i - 5 - 7;
        if (!((q[0] & 0xF8) == 0x48 && q[1] == 0x8B && (q[2] & 0xC0) == 0x80)) continue;
        int qreg = ((q[2] >> 3) & 7) | ((q[0] & 4) ? 8 : 0), qrm = (q[2] & 7) | ((q[0] & 1) ? 8 : 0);
        uint32_t d608; memcpy(&d608, q + 3, 4);
        if (qreg != reg || d608 != 0x608 || (qrm & 7) == 4) continue;
        const uint8_t *m = q - 7;
        if (m < code || !((m[0] & 0xFB) == 0x48 && m[1] == 0x8B && (m[2] & 0xC7) == 0x05)) continue;
        int mreg = ((m[2] >> 3) & 7) | ((m[0] & 4) ? 8 : 0);
        if (mreg != qrm) continue;
        int32_t rel; memcpy(&rel, m + 3, 4);
        *root = m + 7 + rel;
        *off = d;
        return d < 0x10000;
    }
    return 0;
}

#endif
