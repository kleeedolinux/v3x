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
* Entry vtables (`CEffectEntry<T>` / `CTriggerEntry<T>`, RTTI): `CEffectEntry` has 2 slots,
  `CTriggerEntry` has **3**: [0] 0x677ef0 deleting destructor (mi_free), [1] `Create(const Entry *)`
  -> new T, with T+8 = entry token; trigger entries also have [2] 0x67dda0 = `xor eax,eax; ret`
  (returns 0). debug_log: effect entry vtable 0x4a77610 (Create 0x35f2810), trigger entry vtable
  0x4a775d8 (Create 0x35f6860).
* RTTI counts: 709 `CEffectEntry<>`, 1054 `CTriggerEntry<>`, 409 `CScriptMathEntry<>` (script
  value operators, `CJominiScriptMathDatabase`).

## 2. Effect objects (`CJominiEffect`, vtable 0x4a80bd8, 25 slots)

* Constructor 0x3554430. Base layout: +0x08 token, +0x0c byte, +0x20 CPdxArray of child effects
  (count +0x2c). The base class is the effect block.
* Slot 22 = `Execute(this, const Context *ctx)`; `ctx[0]` = pointer to the current scope reference
  (0x3555540 builds {&top scope, 0, &top scope, ...} for the top level). Base 0x3555ff0 executes every child through
  0x3555b50, which first checks the scope (slot 6 = supported scopes bitmask, error
  "Wrong scope for effect: %s, expected %s").
* Slot 16 base 0x3555980 = name (token string via 0x3e1e010).
* Example `debug_log` (`CDebugLogEffect`, vtable 0x4a7e6f8, 26 slots): Create 0x35f2810 (0x60
  bytes, message CString at +0x40), slot 25 0x194dfe0 = set value from a string token, slot 22
  0x3558890 = Execute (formats the message with ROOT/SCOPE/PREV and logs "{}: {}" from
  jomini_effect_impl.cpp line 459). Slot 1 0x35578a0 ("Failed to assign {} at {}") calls slot 25 with
  the value token, so `debug_log = "text"` stores `text` in the CString at +0x40; vtable 0x4a7e6f8 has
  exactly 26 slots (the word after slot 25 is the next vtable's RTTI pointer) and is referenced only
  by Create.

## 3. Trigger objects (`CJominiTrigger`, vtable 0x4a75328, 26 slots)

* Constructor 0x3594960. Slot 2 = deleting destructor, slot 3 = set value from string,
  slot 24 = `bool Evaluate(this, const Context *ctx)` (pure in the base).
* Example `CDebugLogTrigger` (vtable 0x4a74788, 26 slots): Create 0x35f6860 (0x58 bytes, CString
  +0x38), slot 3 0x35a4810 stores the value text at +0x38, Evaluate (slot 24) 0x35a4360 logs
  "Trigger, {}: {}" (jomini_trigger_impl.cpp line 0x8a8). Evaluate gets the same `ctx` (ctx[0] =
  scope reference, ctx[2] = top scope).

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

* Details used by script_ext (read from the disassembly, none executed):
  * **RunInspector** 0x2df7ad0 (called only from RunEffect, `mov dl,1`) does parse -> tooltip text ->
    execute -> history -> destroy. The destroy steps are `0x6a7690(top scope)` and, on the holder,
    `call [vtable+0](effect, 1)` = deleting destructor. Its string temp is destroyed with 0x67b890.
  * **CString** = MSVC `std::string` (0x20 bytes: 16-byte SSO buffer or pointer, size +0x10, capacity
    +0x18, heap when capacity > 15). The parse functions read the text (`size + 1` bytes incl. NUL)
    and never free it. On error they assign the message into the error string (effect: 4th argument;
    trigger: inside the 0x28-byte result) with the engine allocator; the caller releases it (0x67b890).
  * **Effect parse** 0x2df4680 writes `*out = effect` (NULL on error) and leaves the error string
    untouched on success. **Trigger parse** 0x2dfa400 fills {trigger* | CString error (32 bytes)}
    and a byte at +0x20 = 1 on error. The trigger's deleting destructor is vtable slot 2
    (`[rax+0x10]`), the effect's slot 0.
  * **std::function init argument** = MSVC `function<void(CJominiTopScope&)>` with local storage, 0x40
    bytes: impl vtable at +0, captured pointer at +8, `_Ptr` at +0x38 pointing at the impl. 0x2df45c0
    (top scope construct: ctor 0x6ab2e0, seed from the counters 0x5429950/0x5429954, root ref copied
    to +0) does `rcx = [fn+0x38]; call [[rcx]+0x10](rcx, topscope)`. The inspector's impl vtable is
    0x49e6520 = {copy, move, do_call, type, delete_this, target}; its do_call 0x2e13280 only records
    the run in the UI history, so a plugin can pass its own vtable whose do_call returns (script_ext
    does). The engine destroys such an object with `call [vt+0x20](this, this != local storage)`.
  * **CJominiTopScope** is 0x138 bytes (ctor 0x6ab2e0 initializes up to +0x138, dtor 0x6a7690). The
    trigger path (0x2dfb0d0) builds its own smaller scope and takes no game-state access; the effect
    path (0x2df4d00) takes access (2, 1, "jomini_effect_runner.cpp:21") itself.
  * **Random scope record** (RunEffect 0x2df6f30, `0x37879e0(&rec, ?, &srcloc)`): `rec` = {owner object
    0x540ae60, byte acquired}. The function does `lock cmpxchg [owner+0x10], tid` from 0: success =
    acquired 1; owner already this thread = no-op (acquired 0); another thread = assertion "Random
    should not be acquired by any other thread when trying to acquire access". Then `xchg [0x540ae80],
    &rec` (old value kept), run, `[0x540ae80] = old`, and if acquired `xchg [owner+0x10], 0` (must
    return our tid). srcloc = {u32 line, u32 col, file, function}, read only on the assertion path.
  * **Player country**: ExecuteEvent 0x86d5a0 (anchor string "Please specify event name") reads
    `mov rdx,[0x5c3ef90]; mov r13,[rdx+0x608]; ... mov eax,[r13+0xf8]` and builds the root ref
    {5, id}; the console's own commands do the same (docs/re/console.md section 6b).
* Game-state access 0x3689380 is re-entrant per thread (TLS byte +0x5b60 = "this thread holds
  it"; nested calls only swap the access mode, TLS +0x53c). Console commands take the random
  scope first and then Logic access (docs/re/enhance.md 5); that order races with a running
  tick at speed 5.
* The std::function initializer must be non-empty (empty -> `_Xbad_function_call` 0x43e6348).

## 6. Implementation in script_ext (what is checked)

`plugins/script_ext/sx_engine.h` (run text) and `sx_reg.h` (new keywords). Every address is resolved
at run time and cross-checked, never hard-coded. The offline test `test/script_ext_test.c` (parts F and
I) checks all of them against the 1.15.1 exe (addresses in the table) and that losing any anchor
switches the feature off.

| what | anchor | check |
|---|---|---|
| RunEffect 0x2df6f30 | string "RunEffect" (one function) | prologue |
| effect parse 0x2df4680, trigger parse 0x2dfa400 | strings "Effect Runner", "Trigger Runner" | prologue; RunInspector calls the effect parse |
| RunInspector 0x2df7ad0 | the `B2 01 48 8B 0B E8` call inside RunEffect | prologue |
| effect execute 0x2df4d00, top scope dtor 0x6a7690, string dtor 0x67b890 | call sites inside RunInspector (byte shape) | prologue each |
| trigger evaluate 0x2dfb0d0 | unique 40-byte prologue in .text | shares a caller with trigger parse |
| random acquire 0x37879e0, global 0x540ae80, owner 0x540ae60 | call + `lea`/`xchg` inside RunEffect; `lea; mov [rcx],rax; cmpxchg` inside acquire | `lea` and `xchg` name the same global; equals sdk/v3x_game.h `v3x_game_resolve` |
| player id | ExecuteEvent 0x86d5a0: the 51-byte read sequence | global in .data; offset 0xf8 |
| registrars 0x5d9110 (effect), 0x5e6250 (trigger) | the two xrefs of string "debug_log"; 130-byte registrar shape | exactly one effect and one trigger registrar (told apart by Create's allocation size 0x60 / 0x58) |
| token 0x3e1d6f0, allocator 0x43d95c0, registry getter 0x3e1df20, registry global 0x5c40a38, register 0x3553dc0 / 0x3593dc0 | call targets and operands inside the registrar | prologue each; both registrars agree on the first four |
| debug_log object vtables 0x4a7e6f8 / 0x4a74788 | `lea rax,[vtable]; mov [rbx],rax` inside Create (entry vtable slot 1) | 26 code slots then a non-code word; slot 22 / 24 holds an xref of the jomini_effect_impl.cpp / jomini_trigger_impl.cpp path string; set-value slot (25 / 3) assigns to +0x40 / +0x38 |
| entry vtables 0x4a77610 / 0x4a775d8 | `lea rcx,[vtable]; mov [rax],rcx` inside the registrar | effect 2 code slots, trigger 3 (slot 2 = `33 C0 C3`), then a non-code word |

Fault handling: a vectored exception handler (installed once the run anchors resolved, last in the
chain) restores the random scope if a hardware fault (access violation, illegal instruction, divide,
in-page error) happens on the thread that is running script_ext's own run, and disables running until
restart. The loader's SEH guard would otherwise disable the plugin's callbacks with the scope still
taken. C++ exceptions are ignored.

## 7. Not verified / open

* script_ext's calls (run text, keyword registration) were **not executed in a running game**. The test
  uses the real exe only as data, and fake engine functions for the control flow.
* Nothing above was executed. Arguments of Create beyond `this`, what else may call into the
  debug_log effect/trigger objects (tooltips, localization), and the exact top-scope size are
  inferred from one or two call sites.
* The meaning of the third access argument (1 in the runner, 0 in console commands) is not known.
* Multiplayer: running script locally would desync; no MP check is identified.
* Layers built on this (plugins/script_ext): see docs/scripting.md for what is implemented.
* Open for the in-game test: that `v3x_x = "text"` reaches slot 25 for every value token type
  (identifier, quoted string, number); that the keyword is accepted wherever scripts use it (scope
  validation uses debug_log's supported-scope mask); what the debug_log trigger's slot 25 (0x359ca50)
  does in tooltips; that the trigger runner needs no game-state access; whether running script while
  the simulation ticks at speed 5 can race (the engine's own console commands take the scope first
  and Logic access second, the same order).
