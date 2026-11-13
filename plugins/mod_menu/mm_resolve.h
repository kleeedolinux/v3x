/* mod_menu engine anchors (RE notes: docs/re/gui.md).
 *
 * The engine registers every GUI data-binding global function from a CRT static initializer.
 * For void functions (onclick actions) the shape is (OpenWikiUrl registrar, 1.15.1 rva 0x2cd050):
 *     lea  rcx,[rbp-19h]  ; call ARRAY_CTOR          (0x678200) argument-name array of the description
 *     vmovsd xmm0,[rip+"OpenWikiUrl"] ...             name CString built in place (SSO)
 *     lea  rdx,[rbp-39h]  ; lea rcx,[rbp+1Fh] ; call DESC_MAKE   (0x794c20) description object
 *     ... xor r9d,r9d ; lea r8,[rip+EXEC] ; xor edx,edx ; lea rcx,[rbp-1] ; call REGISTER (0x796350)
 * REGISTER(const CString *name, void *is_valid, exec_fn exec, void *a4, void *a5, int a6, desc *d)
 * returns bool; before the data-type system exists it queues the entry (0x3d4c780 -> pending
 * list), the system constructor 0x3d4a080 later flushes the queue into the registry.
 * EXEC for a void function is `bool exec(void *, void *, void *args)` (OpenWikiUrl's 0x2090a00 is
 * the adapter "sub rsp,38h; mov r9,r8; lea rcx,[fn]; call 0x7492f0", which returns 1). */
#ifndef MM_RESOLVE_H
#define MM_RESOLVE_H
#include <stddef.h>
#include <stdint.h>

typedef struct mm_anchors {
    uint8_t *registrar;    /* static initializer that registers OpenWikiUrl (hooked) */
    uint8_t *array_ctor;   /* void ctor(PdxArray *) */
    uint8_t *desc_make;    /* desc *make(desc *out, const desc_in *in) */
    uint8_t *reg;          /* bool register(...) */
    uint8_t *engine_exec;  /* OpenWikiUrl's exec adapter (shape check only) */
    const char *err;       /* why resolving failed (NULL on success) */
} mm_anchors;

/* Checks the registrar body [fn, fn+n) that references `name_str` (the address of "OpenWikiUrl")
 * and fills array_ctor/desc_make/reg/engine_exec. Returns 1 if every byte check passes.
 * Pure: reads memory only. `fn` must be the function start. */
int mm_check_registrar(uint8_t *fn, size_t n, const uint8_t *name_str, mm_anchors *a);

/* Scans text [text, text+n) for "vmovsd xmm0,[rip+X]" (C5 FB 10 05 disp32) with X == target.
 * Returns the index-th hit or NULL. */
uint8_t *mm_find_vmovsd_ref(uint8_t *text, size_t n, const uint8_t *target, int index);

#endif
