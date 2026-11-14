# ADR 0004: SDK safety: bounded helpers, checked builds, fuzzing

Status: accepted (2026-11-13). Audit "Rust-like safety plan", findings M1, M3, L7, L9, L13; actions 7,
8, 14, 15.

## Context

The C code base repeated a few dangerous idioms: `n += snprintf(buf + n, sizeof buf - n, ...)` (42
sites; once `n` passes the size the remaining size wraps), unchecked 32-bit offset arithmetic in
readers of on-disk and in-memory formats, hand-rolled path joins, plain globals shared across threads,
and variadic log functions without format checking. C cannot be made memory-safe, but these patterns
can be replaced by a small set of checked primitives and a build that catches the rest.

## Decision

* `sdk/v3x_safe.h` (header-only, C99 and C++, `-Wconversion` clean) is the one place for:
  `V3xStr` bounded builder (never overruns, sticky `truncated`, UTF-8-safe cut), `V3xSpan` checked
  reader (sticky error), `v3x_add_ok` / `v3x_mul_ok` / `v3x_alloc_array` (`__builtin_*_overflow`),
  `V3xResult` + `V3X_MUST_USE`, `v3x_path_rel_ok` / `v3x_path_join_checked`, `v3x_sanitize_line`,
  `v3x_atomic_*`, `V3X_ASSERT_MAIN`, `V3X_PRINTF` / `V3X_NONNULL`. Ownership naming: `*_new` / `*_free`
  own, `*_view` and const pointers borrow. SDK helpers are not removed within one `V3X_SDK_VERSION`.
* `include/v3x.h` carries `V3X_PRINTF` on `log`, `print`, `log_at` (attribute on the field: no ABI
  change, `test/abi_test.c`) and the thread rule of every API entry.
* Build (`build.sh`, `sdk/build_plugin.py`): `-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2
  -ftrivial-auto-var-init=zero -fno-delete-null-pointer-checks -Wall -Wextra -Wformat=2 -Wvla
  -Wshadow`; `-Werror` for the core and every SDK header (compiled as C and C++); plugins with
  `V3X_WERROR=1` (CI, once all are clean). Tests: `SANITIZE=1` = `-fsanitize=undefined
  -fsanitize-trap=undefined` (zig has no UBSan runtime on windows-gnu; trap = crash = failed test).
  Flags that do not work with zig 0.17.1 on x86_64-windows-gnu (audit probe) are not used:
  `-mguard=cf`, ASan, `-fsanitize=fuzzer`, CFI, `-fstack-clash-protection` (unused there).
* Fuzzing: `test/fuzz/` is a deterministic in-tree mutational fuzzer (fixed-seed xorshift, guard-page
  inputs, UBSan trap) with one `fuzz_<name>.c` per parser and the `V3X_FUZZ_TARGET` API; `run_all.sh`
  runs a quick pass every time, `FUZZ_ITERS` for long runs. windows.h-free harnesses also build as
  libFuzzer targets (`-DV3X_FUZZ_LIBFUZZER`) on Linux with ASan.
* `tools/check_unsafe.sh` bans the unbounded idioms in CI; `test/static_analysis.sh` runs
  clang-tidy / cppcheck as a non-blocking report until the baseline is triaged.

## Consequences

* New code uses the helpers or explains why not (`v3x-unsafe-ok:` comment); reviews can grep.
* A format mismatch in a plugin's `api->log` call is now a compiler warning (an error with
  `V3X_WERROR=1`).
* The fuzzer already paid off: it found undefined behaviour in `include/v3x_rtti.h` (unaligned PE
  header access on a crafted `e_lfanew`), fixed with bounded `memcpy` reads.
* Hardening flags may cost a few percent in hot loops (not yet measured in game). They can be
  relaxed per file only with a comment and a reason.
