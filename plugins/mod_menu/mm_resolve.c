/* mod_menu engine anchors (see mm_resolve.h). Reads memory only. */
#include "mm_resolve.h"
#include <string.h>

static uint8_t *rel32(uint8_t *at) /* at = address of the disp32, instruction ends at at+4 */
{
    int32_t d;
    memcpy(&d, at, 4);
    return at + 4 + d;
}

/* first match of pat (0x100 = wildcard) in [p, p+n) */
static uint8_t *find(uint8_t *p, size_t n, const uint16_t *pat, size_t pn)
{
    for (size_t i = 0; i + pn <= n; i++) {
        size_t k = 0;
        while (k < pn && (pat[k] == 0x100 || p[i + k] == pat[k])) k++;
        if (k == pn) return p + i;
    }
    return NULL;
}

#define W 0x100
/* lea rcx,[rbp-19h]; call rel32 */
static const uint16_t P_CTOR[] = {0x48, 0x8D, 0x4D, 0xE7, 0xE8};
/* vmovsd xmm0,[rip+name] */
static const uint16_t P_NAME[] = {0xC5, 0xFB, 0x10, 0x05};
/* lea rdx,[rbp-39h]; lea rcx,[rbp+1Fh]; call rel32 */
static const uint16_t P_DESC[] = {0x48, 0x8D, 0x55, 0xC7, 0x48, 0x8D, 0x4D, 0x1F, 0xE8};
/* mov [rsp+30h],rax; mov [rsp+28h],ebx; mov [rsp+20h],rbx; xor r9d,r9d;
 * lea r8,[rip+exec]; xor edx,edx; lea rcx,[rbp-1]; call rel32 */
static const uint16_t P_REG[] = {0x48, 0x89, 0x44, 0x24, 0x30, 0x89, 0x5C, 0x24, 0x28, 0x48, 0x89, 0x5C, 0x24, 0x20,
                                 0x45, 0x33, 0xC9, 0x4C, 0x8D, 0x05, W, W, W, W, 0x33, 0xD2, 0x48, 0x8D, 0x4D, 0xFF, 0xE8};
/* register prologue: mov [rsp+8],rbx; mov [rsp+10h],rsi; mov [rsp+18h],rdi; push rbp; push r14; push r15 */
static const uint8_t B_REG[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C,
                                0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57};
/* array ctor: push rbx; sub rsp,20h; xor eax,eax; mov rbx,rcx; mov [rcx],rax; mov [rcx+8],rax */
static const uint8_t B_CTOR[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC0, 0x48, 0x8B, 0xD9, 0x48, 0x89, 0x01,
                                 0x48, 0x89, 0x41, 0x08};
/* desc make: mov [rsp+18h],rbx; mov [rsp+20h],rsi; mov [rsp+8],rcx; push rdi; sub rsp,20h; mov rdi,rdx */
static const uint8_t B_DESC[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C,
                                 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xFA};
/* void exec adapter: sub rsp,38h; mov r9,r8; lea rcx,[rip+fn]; call rel32; add rsp,38h; ret */
static const uint8_t B_EXEC1[] = {0x48, 0x83, 0xEC, 0x38, 0x4D, 0x8B, 0xC8, 0x48, 0x8D, 0x0D};
static const uint8_t B_EXEC2[] = {0x48, 0x83, 0xC4, 0x38, 0xC3};

#define N(a) (sizeof(a) / sizeof((a)[0]))

int mm_check_registrar(uint8_t *fn, size_t n, const uint8_t *name_str, mm_anchors *a)
{
    a->array_ctor = a->desc_make = a->reg = a->engine_exec = NULL;
    uint8_t *c = find(fn, n, P_CTOR, N(P_CTOR));
    if (!c) { a->err = "registrar: array ctor call not found"; return 0; }
    uint8_t *s = find(fn, n, P_NAME, N(P_NAME));
    if (!s || rel32(s + 4) != name_str) { a->err = "registrar: name load not found"; return 0; }
    uint8_t *d = find(fn, n, P_DESC, N(P_DESC));
    if (!d) { a->err = "registrar: description call not found"; return 0; }
    uint8_t *r = find(fn, n, P_REG, N(P_REG));
    if (!r) { a->err = "registrar: register call not found"; return 0; }
    if (!(c < s && s < d && d < r)) { a->err = "registrar: calls out of order"; return 0; }
    uint8_t *ctor = rel32(c + 5), *desc = rel32(d + 9), *exec = rel32(r + 20), *reg = rel32(r + 31);
    if (memcmp(ctor, B_CTOR, sizeof B_CTOR)) { a->err = "array ctor prologue mismatch"; return 0; }
    if (memcmp(desc, B_DESC, sizeof B_DESC)) { a->err = "description prologue mismatch"; return 0; }
    if (memcmp(reg, B_REG, sizeof B_REG)) { a->err = "register prologue mismatch"; return 0; }
    if (memcmp(exec, B_EXEC1, sizeof B_EXEC1) || exec[14] != 0xE8 || memcmp(exec + 19, B_EXEC2, sizeof B_EXEC2)) {
        a->err = "OpenWikiUrl exec adapter shape mismatch (void exec ABI changed?)";
        return 0;
    }
    a->array_ctor = ctor; a->desc_make = desc; a->reg = reg; a->engine_exec = exec;
    a->err = NULL;
    return 1;
}

uint8_t *mm_find_vmovsd_ref(uint8_t *text, size_t n, const uint8_t *target, int index)
{
    for (size_t i = 0; i + 8 <= n; i++) {
        if (text[i] != 0xC5 || text[i + 1] != 0xFB || text[i + 2] != 0x10 || text[i + 3] != 0x05) continue;
        if (rel32(text + i + 4) == target && index-- == 0) return text + i;
    }
    return NULL;
}
