/* cpu_sched core logic (header-only, no OS calls, so test/cpu_sched_test.c can include it).
 * - masked signature compare and the two code-walk steps used to find the worker entry
 * - parsing of a raw GetSystemCpuSetInformation buffer and P/E classification
 * - policy names
 * RVAs and background: docs/re/threading.md. Anchors are mirrored in tools/re_threads_check.py. */
#ifndef CS_CORE_H
#define CS_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ signatures
 * mask: 'x' = must match, '?' = wildcard. Length = strlen(mask). */
static int cs_sig_match(const unsigned char *p, const unsigned char *sig, const char *mask)
{
    for (size_t i = 0; mask[i]; i++)
        if (mask[i] == 'x' && p[i] != sig[i]) return 0;
    return 1;
}

/* Scheduler init (pdx_task.cpp, 1.15.1 RVA 0x3df8a50): ecx = Threading.TaskThreadCount (-1 = auto).
 * push rbp/rbx/rsi/rdi/r14/r15; lea rbp,[rsp-0x2f]; sub rsp,0xa8; mov ebx,ecx; xor r15d,r15d;
 * call <hardware_concurrency>; mov edi,4; cmp ebx,-1 */
static const unsigned char CS_SIG_INIT[] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xd1,
    0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x8b, 0xd9, 0x45, 0x33, 0xff,
    0xe8, 0, 0, 0, 0, 0xbf, 0x04, 0x00, 0x00, 0x00, 0x83, 0xfb, 0xff };
static const char CS_MASK_INIT[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxx";

/* AddWorker (1.15.1 RVA 0x3dfa400): mov [rsp+20],rbx; mov [rsp+10],rdx; mov [rsp+8],rcx; push rbp..r15 */
static const unsigned char CS_SIG_ADDWORKER[] = {
    0x48, 0x89, 0x5c, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08,
    0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
static const char CS_MASK_ADDWORKER[] = "xxxxxxxxxxxxxxxxxxxxxxxxxx";

/* Worker thread entry (1.15.1 RVA 0x3df9180), rcx = thread object:
 * prologue; mov rbx,rcx; mov edx,[rcx+0x38]; mov r8d,<TLS off>; mov rax,gs:[0x58]; mov rcx,[rax];
 * mov byte [r8+rcx],1; mov [r8+rcx+8],rbx; mov [r8+rcx+4],edx; mov ecx,[rbx+0x3c]; call <SetPriorityByKind> */
static const unsigned char CS_SIG_WORKER[] = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xd9, 0x8b, 0x51, 0x38, 0x41, 0xb8, 0, 0, 0, 0,
    0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x08, 0x41, 0xc6, 0x04, 0x08, 0x01,
    0x49, 0x89, 0x5c, 0x08, 0x08, 0x41, 0x89, 0x54, 0x08, 0x04, 0x8b, 0x4b, 0x3c, 0xe8 };
static const char CS_MASK_WORKER[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

#define CS_WORKER_OFF_INDEX 0x38   /* int: slot index in its pool */
#define CS_WORKER_OFF_KIND  0x3c   /* int: 0 below normal, 1 normal, 2 above normal */

/* peek(ctx, va, n): pointer to n readable bytes at va, or NULL (keeps scans inside the image). */
typedef const unsigned char *(*cs_peek_fn)(void *ctx, uint64_t va, size_t n);

/* First `call rel32` in [code, code+n) (code is at address va) whose target matches sig/mask. 0 = none. */
static uint64_t cs_find_call_to(const unsigned char *code, size_t n, uint64_t va, const unsigned char *sig,
                                const char *mask, cs_peek_fn peek, void *ctx)
{
    size_t sl = strlen(mask);
    for (size_t i = 0; i + 5 <= n; i++) {
        if (code[i] != 0xe8) continue;
        int32_t rel; memcpy(&rel, code + i + 1, 4);
        uint64_t t = va + i + 5 + (int64_t)rel;
        const unsigned char *p = peek(ctx, t, sl);
        if (p && cs_sig_match(p, sig, mask)) return t;
    }
    return 0;
}

/* First `lea rcx,[rip+rel32]; call rel32` in [code, code+n) whose lea target matches sig/mask. 0 = none. */
static uint64_t cs_find_lea_rcx_call(const unsigned char *code, size_t n, uint64_t va, const unsigned char *sig,
                                     const char *mask, cs_peek_fn peek, void *ctx)
{
    size_t sl = strlen(mask);
    for (size_t i = 0; i + 12 <= n; i++) {
        if (code[i] != 0x48 || code[i + 1] != 0x8d || code[i + 2] != 0x0d || code[i + 7] != 0xe8) continue;
        int32_t rel; memcpy(&rel, code + i + 3, 4);
        uint64_t t = va + i + 7 + (int64_t)rel;
        const unsigned char *p = peek(ctx, t, sl);
        if (p && cs_sig_match(p, sig, mask)) return t;
    }
    return 0;
}

/* ------------------------------------------------------------------ CPU sets
 * Raw SYSTEM_CPU_SET_INFORMATION layout (winnt.h): Size u32 @0, Type u32 @4 (0 = CpuSetInformation),
 * Id u32 @8, Group u16 @12, LogicalProcessorIndex u8 @14, CoreIndex u8 @15, LastLevelCacheIndex u8 @16,
 * NumaNodeIndex u8 @17, EfficiencyClass u8 @18, AllFlags u8 @19 (bit0 Parked, bit1 Allocated,
 * bit2 AllocatedToTargetProcess, bit3 RealTime). Entries are variable-size: advance by Size. */
#define CS_MAX_CPUS 256

typedef struct { uint32_t id; uint16_t group; uint8_t lp, core, eff, flags; } cs_cpu;

typedef struct {
    int n;                       /* parsed CPU sets */
    cs_cpu cpu[CS_MAX_CPUS];
    int hybrid;                  /* 1 if more than one EfficiencyClass is present */
    uint8_t eff_min, eff_max;
    int np, ne;                  /* counts */
    uint32_t pid[CS_MAX_CPUS];   /* CPU set ids of P-cores (EfficiencyClass == max) */
    uint32_t eid[CS_MAX_CPUS];   /* CPU set ids of all other cores */
    int p_phys, e_phys;          /* distinct (group, core) pairs */
} cs_map;

/* Parse and classify. Returns number of CPU sets parsed (0 = bad/empty buffer). */
static int cs_classify(const unsigned char *buf, size_t len, cs_map *m)
{
    memset(m, 0, sizeof *m);
    for (size_t off = 0; off + 8 <= len && m->n < CS_MAX_CPUS; ) {
        uint32_t size, type; memcpy(&size, buf + off, 4); memcpy(&type, buf + off + 4, 4);
        if (size < 8 || off + size > len) break;               /* malformed: stop */
        if (type == 0 && size >= 20) {
            cs_cpu c; memcpy(&c.id, buf + off + 8, 4); memcpy(&c.group, buf + off + 12, 2);
            c.lp = buf[off + 14]; c.core = buf[off + 15]; c.eff = buf[off + 18]; c.flags = buf[off + 19];
            m->cpu[m->n++] = c;
        }
        off += size;
    }
    if (!m->n) return 0;
    m->eff_min = m->eff_max = m->cpu[0].eff;
    for (int i = 1; i < m->n; i++) {
        if (m->cpu[i].eff < m->eff_min) m->eff_min = m->cpu[i].eff;
        if (m->cpu[i].eff > m->eff_max) m->eff_max = m->cpu[i].eff;
    }
    m->hybrid = m->eff_min != m->eff_max;
    for (int i = 0; i < m->n; i++) {
        const cs_cpu *c = &m->cpu[i];
        int p = c->eff == m->eff_max, seen = 0;
        for (int j = 0; j < i; j++)
            if (m->cpu[j].group == c->group && m->cpu[j].core == c->core) { seen = 1; break; }
        if (p) { m->pid[m->np++] = c->id; if (!seen) m->p_phys++; }
        else   { m->eid[m->ne++] = c->id; if (!seen) m->e_phys++; }
    }
    return m->n;
}

/* ------------------------------------------------------------------ policy */
enum { CS_ALL = 0, CS_PCORES = 1, CS_ECORES = 2 };
static const char *const CS_POLICY_NAMES[] = { "all", "pcores", "ecores" };

/* Parse "all" / "pcores" / "ecores" (case-insensitive, surrounding spaces ignored); def on anything else. */
static int cs_parse_policy(const char *s, int def)
{
    if (!s) return def;
    while (*s == ' ' || *s == '\t') s++;
    for (int k = 0; k < 3; k++) {
        const char *n = CS_POLICY_NAMES[k]; size_t i = 0;
        while (n[i] && (s[i] | 0x20) == n[i]) i++;
        if (!n[i]) {
            const char *r = s + i;
            while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') r++;
            if (!*r) return k;
        }
    }
    return def;
}

/* CPU set ids for a policy. Returns count (0 = no restriction: CS_ALL, not hybrid, or the class is empty). */
static int cs_policy_ids(const cs_map *m, int policy, const uint32_t **ids)
{
    *ids = NULL;
    if (!m->hybrid) return 0;
    if (policy == CS_PCORES && m->np) { *ids = m->pid; return m->np; }
    if (policy == CS_ECORES && m->ne) { *ids = m->eid; return m->ne; }
    return 0;
}

#endif
