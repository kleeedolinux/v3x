# ADR 0003: Hook lifetime: never free a trampoline at runtime; W^X hook stubs

Status: accepted (2026-11-13). Audit findings M2, L2; implemented in `src/safe_hook.c`.

## Context

`MH_RemoveHook` frees the MinHook trampoline of a target. Another thread may have just loaded the
trampoline address (from `*original`, or from the hook_safe thunk) and be about to call it: after the
free that call executes freed memory. The loader called it on `unhook`, when a raw hook replaced a
disabled hook_safe thunk, and in `rollback_module` for failed plugins. Separately, the hook_safe stub
pool was one `PAGE_EXECUTE_READWRITE` region for the whole process lifetime (a write-anywhere bug
becomes code execution there).

## Decision

* At runtime a hook that was ever enabled is only **disabled** (`MH_DisableHook`: original bytes
  back), never removed. The MinHook entry, its trampoline and the V3X stub stay until process exit.
  `MH_RemoveHook` is used only on a hook that was never enabled (a failed install).
* Every target in the registry has one stub: `mov r10, <ctx>; jmp [r10+0x28]`. For hook_safe,
  `ctx->jmp` is the shared thunk; for a raw `api->hook` it is the plugin's detour and `*original` is the
  record's trampoline. Re-hooking a disabled target (raw or hook_safe, any plugin) re-points `ctx->jmp`
  while the hook is disabled and re-enables the same MinHook entry: the old trampoline stays valid.
* Stub pages are generated completely while still private and read-write, then made
  `PAGE_EXECUTE_READ` before any stub is handed out, and never written again. Per-hook data
  (`SafeCtx`: trampoline, callbacks, jump target) lives in separate `PAGE_READWRITE` pages.
* `api->unhook` and rollback are documented as "disable"; plugins must keep their own detour code
  loaded (V3X never `FreeLibrary`s a loaded plugin).

## Consequences

* A few dozen bytes per ever-hooked target are kept until exit (bounded: 4 pages of 1024 records).
* No use-after-free of trampolines from concurrent callers; a later hook on the same function reuses
  the trampoline (tests: `test/hook_chain_test.c` "M2:", "L2:").
* Raw detours now enter through the stub, so they see `r10 = ctx` (r10 is volatile and never an
  argument in the Win64 ABI) and pay one extra indirect jump.
* A hook installed outside the registry (direct MinHook use) is only disabled by `unhook` too, so the
  target cannot be hooked again through MinHook in that process.
