# Script system (effects, triggers, script values): static findings (1.15.1)

Worker `script`. All addresses are RVAs in `binaries/victoria3.exe` 1.15.1. Everything here is
**static analysis only** (disassembly with `tools/re_enh_lib.py`, RTTI with
`tools/re_script_rtti.py`); nothing was run in the game. Source files named in the exe's
strings: `cw/jomini/modules/script_system/source/jomini_effect*.cpp`, `jomini_trigger*.cpp`,
`jomini_scriptvalue.cpp`, `script_system_gui/source/jomini_effect_runner.cpp`,
`jomini_script_runner_inspector.cpp`.

Regenerate the RTTI list: `python tools/re_script_rtti.py ../binaries/victoria3.exe`
(writes `tools/re_script_tmp/rtti.txt`, one `vtable_rva offset class` per line).

## 1. Registration: CRT static initializers, keyed by token id

| what | function | callers | registry (magic static) |
|---|---|---|---|
| effect | `0x3553dc0(uint32 token, CEffectEntry *entry)` | 711 | hash map getter 0x3553cc0 (data 0x5cfb490), ordered token list 0x3553e70 |
| trigger | `0x3593dc0(uint32 token, CTriggerEntry *entry)` | 1060 | hash map getter 0x3593e70 (data 0x5cfc628), ordered token list 0x3593f70 |

* Each registrar is a small function whose pointer sits in the `.rdata` CRT initializer table
  (example: `debug_log` effect registrar 0x5d9110; trigger `is_tutorial_active` 0x2b5a0, pointer
  at .rdata 0x454b5d0). Some templated families (every_/random_/ordered_ list effects, e.g.
  0x17bceb0) are registered from another initializer.
* Registrar shape: name -> token with `0x3e1d6f0(registry, &{const char*, u32 len, u8 0})`, the
  registry pointer at `[0x5c40a38]` (created lazily by 0x3e1df20); then a 0x18-byte entry is
  allocated with the engine allocator 0x43d95c0: `{vtable, const char *description, u32 token}`;
  then the register call.
* The register call hashes the token (FNV-1a over its 4 bytes), finds-or-inserts in the map and
  stores the entry at node+0x10. **An existing entry for the same token is destroyed and
  replaced** (vtable slot 0 with 1), so a name collision would silently replace a vanilla effect.
  It also appends the token to the ordered list (used by `script_docs`).
* Entry vtables (`CEffectEntry<T>` / `CTriggerEntry<T>`, RTTI): 2 slots: [0] 0x677ef0 deleting
  destructor (mi_free), [1] `Create(const Entry *)` -> new T, with T+8 = entry token.
* RTTI counts: 709 `CEffectEntry<>`, 1054 `CTriggerEntry<>`, 409 `CScriptMathEntry<>` (script
  value operators, `CJominiScriptMathDatabase`).

## 2. Effect objects (`CJominiEffect`, vtable 0x4a80bd8, 25 slots)

* Constructor 0x3554430. Base layout: +0x08 token, +0x0c byte, +0x20 CPdxArray of child effects
  (count +0x2c). The base class is the effect block.
* Slot 22 = `Execute(this, const Context *ctx)`. Base 0x3555ff0 executes every child through
  0x3555b50, which first checks the scope (slot 6 = supported scopes bitmask, error
  "Wrong scope for effect: %s, expected %s").
* Slot 16 base 0x3555980 = name (token string via 0x3e1e010).
* Example `debug_log` (`CDebugLogEffect`, vtable 0x4a7e6f8, 26 slots): Create 0x35f2810 (0x60
  bytes, message CString at +0x40), slot 25 0x194dfe0 = set value from a string token, slot 22
  0x3558890 = Execute (formats the message with ROOT/SCOPE/PREV and logs "{}: {}" from
  jomini_effect_impl.cpp line 459).

## 3. Trigger objects (`CJominiTrigger`, vtable 0x4a75328, 26 slots)

* Constructor 0x3594960. Slot 2 = deleting destructor, slot 3 = set value from string,
  slot 24 = `bool Evaluate(this, const Context *ctx)` (pure in the base).
* Example `CDebugLogTrigger` (vtable 0x4a74788): Create 0x35f6860 (0x58 bytes, CString +0x38),
  Evaluate 0x35a4360 logs "Trigger, {}: {}" and returns true.

## 4. Context and scopes

* For both effects and triggers `ctx[0]` points to the current scope reference, 16 bytes:
  `{u16 scope type index; 6 bytes pad; u64 object id}`.
* Scope type table: getter 0x357a180 -> CPdxArray at 0x5cfc2a8, 0x50-byte entries: +0x00 name
  token, +0x10 `bool valid(const ScopeRef *)`.
* In the `event` console command (ExecuteEvent 0x86d5a0) the root is type 5 (country) with id
  = player country id `[[[exe+0x5c3ef90]+0x608]+0xf8]`; 0x4c / 0x44 / 0x2c are other types
  chosen from the selected map object (not named yet).
* Top scope (`CJominiTopScope`, about 0xb8 bytes): ctor 0x6ab2e0 / 0x6ab340, dtor 0x6a7690; root
  ref at +0, a per-scope random seed at +0x10 (from the counters at 0x5429950 / 0x5429928).

## 5. Running script text at runtime: the engine's Script Runner

1.15.1 has **no** `effect` / `trigger` console command (runtime registrations listed with
`tools/re_console_reg.py <exe> 3dc2ad0`: only `script_docs`, `Script.Profiling.*`, container
commands). The object explorer's Script Runner (`CScriptRunnerInspector`) runs text:

| step | function | notes |
|---|---|---|
| UI entry | 0x2df6f30 (`"RunEffect"`) | takes the script random scope: 0x37879e0(&rec, ?, &srcloc) (cmpxchg owner tid at 0x540ae60+0x10), then `xchg [0x540ae80], &rec`; restores and releases after |
| run | 0x2df7ad0(inspector, bool execute) | root scope ref from 0x2df75a0 ("No object set!", "Game object is not valid") |
| parse effect | 0x2df4680(CJominiEffect **out, const CString *text, u16 scope type, CString *error) | reader named "Effect Runner"; builds a CJominiEffect block |
| tooltip only | 0x2df4b70 | 0x355dba0 = effect description text |
| execute | 0x2df4d00(holder {effect*}, CJominiTopScope *out, const ScopeRef *root, const std::function<void(CJominiTopScope&)> *init) | game-state access 0x3689380(guard, 2, 1, "jomini_effect_runner.cpp:21"), execute 0x3555540(effect, top scope), release 0x3689490 |
| parse trigger | 0x2dfa400(result *out, const CString *text, u16 scope type) | "Trigger Runner"; result = {trigger* or error CString, bool is_error at +0x20} |
| evaluate trigger | 0x2dfb0d0(holder {trigger*}, const ScopeRef *root, const std::function *init) -> bool | top-level evaluate 0x35958d0; caller is `CScriptDebugger::EvaluateConditionTrigger` 0x2de3d70 |

* Game-state access 0x3689380 is re-entrant per thread (TLS byte +0x5b60 = "this thread holds
  it"; nested calls only swap the access mode, TLS +0x53c). Console commands take the random
  scope first and then Logic access (docs/re/enhance.md 5); that order races with a running
  tick at speed 5.
* The std::function initializer must be non-empty (empty -> `_Xbad_function_call` 0x43e6348).

## 6. Not verified / open

* Nothing above was executed. Arguments of Create beyond `this`, what else may call into the
  debug_log effect/trigger objects (tooltips, localization), and the exact top-scope size are
  inferred from one or two call sites.
* The meaning of the third access argument (1 in the runner, 0 in console commands) is not known.
* Multiplayer: running script locally would desync; no MP check is identified.
* Layers built on this (plugins/script_ext): see docs/scripting.md for what is implemented.
