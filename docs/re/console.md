# In-game developer console: command registry and Tab completion (1.15.1)

Reverse-engineered for `plugins/console_plus` (worker autocomplete). All addresses are RVAs in
`binaries/victoria3.exe` 1.15.1. Tools: `tools/re_console_cmds.py` (static command descriptors),
`tools/re_console_reg.py` (runtime registrations), `tools/re_console_db.py` (database getters),
plus `tools/re_enh_lib.py` for disassembly. The offline check is `test/console_plus_test.c`
part C.

## 1. The engine already has Tab completion

Source: `cw/clausewitz/pdx_applicationutils/console/console.cpp`, class `CConsole`
(vtable 0x4a90d88), GUI `game/gui/console.gui` (`console_window`, editbox `console_edit`).

* `CConsole` vtable slot 1, 0x36bd110 = key/input handler. Key code 9 (Tab) calls
  `CConsole::Autocomplete(this, mode)` 0x36be560 with mode 0; two more call sites use mode 1
  and 2 (cycle forward / back, page down).
* `Autocomplete` 0x36be560 is the only function that references
  `"> (found: {} | tab: next, esc: exit)"` and `"> (found: {}/{} | tab: next, esc: exit, page-down: more)"`.
  It calls the manager getter 0x3dc0e00, then the matcher 0x3dc2140(manager, input, &result).
  The candidate list is shown 15 per page; Tab cycles through it. With exactly one match the
  token is replaced: without a space in the input the line becomes `match + " "`; with a space
  everything after the last space is replaced.
* Settings `console_autocomplete_mode` (`EConsoleAutoCompleteMode`: partial / full expand) exist.

So command names were always completed. What was missing was arguments.

## 2. Command manager and registry

* Getter 0x3dc0e00 (`push rbx; sub rsp,40h; mov rax,[rip+M]; test rax,rax; jnz` - unique
  pattern): lazily creates the 0xf8-byte manager. The plugin only reads `M` = exe+0x5c72730 and
  never calls the getter.
* Manager layout: `+0x00` entries (pointer), `+0x0c` count (int; `+0x08` capacity, CPdxArray),
  `+0xa8` lock (bit 0 = writer, readers `lock xadd +2`; writers `lock bts 0`, then wait until
  the value is <= 1; release `lock and ~1`), `+0xf0` byte (filters release-only commands in the
  name search).
* Entry = `SCommandData`, stride 0x210:

| offset | what |
|---|---|
| +0x00 | byte flag (1 for most) |
| +0x08 | CString name (MSVC std::string: 16-byte SSO buffer / pointer, size +0x10, capacity +0x18) |
| +0x28 | int alias count; +0x30 3 x CString aliases (lookup 0x3dc2f90 compares name and aliases with `_stricmp`) |
| +0x90 | CString description (`help <cmd>` prints it, 0x3dc1900) |
| +0xb0 | `SResult OnExecute(SResult*, const CPdxArray<CString>& args, void*)` |
| +0xb8 | argument completer `void f(const CString *previous_args, CPdxArray<CString> *out, void *user)`, or 0 |
| +0xc0 | user pointer passed to it |
| +0xc8 | int (1 for `help`, 0 for `Terrain.Regenerate` although it has an argument string; meaning not verified) |
| +0xd0 | 10 x CString argument descriptions (usage line `name [arg] [arg]`, 0x3dc0b80) |

* Registration: `CConsoleCmdManager::Register(manager, const SCommandData*)` 0x3dc2ad0, called
  under the exclusive lock from 246 sites (one small function per command, struct built on the
  stack: e.g. `sleep` 0x87f0d0). 57 more commands come from static descriptors in `.data`
  (`consolecmdimpl.cpp` dynamic initializer 0x16050; e.g. `help` at 0x5413df0, exec 0x87e320).
  `help` without arguments lists all (0x3dc1a60).
* Only ~26 commands have a completer at +0xb8. Static: `event` 0x86e290 (event names),
  `mapmode` 0x957b70 (map modes). Runtime: `research` 0x957cc0 (technologies), `ai.goal`
  0x957c70 (58 goal types), `add_ideology` 0x88b100, `Ships.*`, `spawn_centerpiece`,
  `material_debug`, `Debug.TextureMode`, `GUI.CreateWidget` and other tools. Not `tag`,
  `annex`, `own`, `conquerall`, `change_law`, `add_journal_entry`, `treatyport`, ...

## 3. Matcher 0x3dc2140

* **Signature: `(manager, CString input BY VALUE, result)`.** The caller (Autocomplete) copy-constructs
  a temporary (0x36be7a0 -> 0x678580) and passes its address; the matcher destroys it itself before
  returning (0x3dc296f `mov rcx,r15` = input; `call 0x67b890` CString dtor -> mi_free 0x43d94b0).
  Lesson (crash6): never pass the matcher a CString whose buffer is not the engine's own mimalloc
  block - mi_free on plugin memory is an access violation (seen at exe+0x43d94d6 for inputs >= 16
  chars, heap mode). Change the engine's string in place instead (shorten size, NUL inside its
  buffer, capacity/pointer untouched). Rule for every engine call: find out who destroys a CString
  argument (callee-destroyed = by value) before handing over one built in the plugin.

* No space in the input: fuzzy name search over all entries (0x3de2960, entries with +0x00 == 0
  are skipped when manager+0xf0 is set) plus console command macros (`CConsoleCommandMacroDatabase`
  0x3dc3050, names at +0x30, case-insensitive prefix), then sorted.
* With a space: name = text before the first space, rest = text after it. Entry =
  lookup(name). If entry+0xb8 is set: `previous` = rest up to its last space (empty for the
  first argument), token = after it, trimmed and lowercased. Completer call at 0x3dc24cd:
  `mov rax,[r13+0B8h]; mov r8,[r13+0C0h]; lea rdx,[rsp+70h]; lea rcx,[rbp-40h]; call rax`
  (unique pattern) = `f(&previous, &candidates, user)`. Every candidate is lowercased and kept if
  it CONTAINS the token (0x43e9950; empty token keeps all), the original spelling is pushed into
  the result, then sorted (0x3e51900 / merge sort for > 32). So `tag sok<Tab>` finds `SOK`.
* `CPdxArray<CString>` = {data, int capacity, int count, allocator*}; element 32 bytes.
  `push_back(array, const CString*)` 0x74c7f0 copy-constructs (CString copy ctor 0x678580 copies
  size+1 bytes from the source, never takes ownership). The engine's own key completers loop
  `mov rbx,[rdi+80h]; movsxd rax,[rdi+8Ch]; lea rdi,[rbx+rax*8]; ... add rdx,18h; call push`
  (0x957bba mapmode, 0x957d0a research) - that loop is the plugin's anchor for push.

## 4. Game databases (country tags, state regions, laws, ...)

* Each database `X` is held by a transactional resource
  `NPdxTransactions::CTransactionResource<std::unique_ptr<X>>`: a statically initialised `.data`
  object whose first qword is its vtable. Exactly one function references that object
  (`lea r12,[rip+obj]`): the getter, `X** get(void)` (TLS transaction scope -> slot; [slot] = X*,
  the byte at slot+8 must be set or it calls a fatal error 0x677f40). All getters share the
  prologue `mov [rsp+8],rbx ... sub rsp,30h; mov rax,gs:[58h]; mov rbx,[rax]; mov r15d,8680h`.
  The engine's own completers call them from the completer, i.e. on the main thread in the
  console's context; console_plus does the same and nothing else.
* All of the classes below derive from `CGameDatabase<X, T, CGameDatabaseAssertAccess, 1>` at
  offset 0 (RTTI), with `T* array` at +0x80 and count (int) at +0x8c. Every `T` derives from
  `CGameDatabaseObject : CPersistent` with the key CString at T+0x18 (mapmode and research
  completers read exactly these offsets).

| kind | database | getter | resource object |
|---|---|---|---|
| country (tags) | CCountryDefinitionDatabase (vtable 0x4687050) | 0x7d0f00 | 0x5439d40 |
| state_region (STATE_*) | CStateRegionTemplateDatabase | 0x79e6b0 | 0x543a4f0 |
| strategic_region | CStrategicRegionsDatabase | 0x79e530 | 0x5433820 |
| law | CLawTypeDatabase | 0x7d0180 | 0x5434240 |
| goods | CGoodsDatabase | 0x7cc400 | 0x5432bc0 |
| journal | CJournalEntryTypeDataBase | 0x7d0000 | 0x5436b20 |
| building | CBuildingDatabase | 0x7ce800 | 0x5431780 |
| production_method | CProductionMethodDataBase | 0x7d9070 | 0x54348f0 |
| culture | CCultureTypeDataBase | 0x7cc700 | 0x5439a40 |
| religion | CReligionDataBase | 0x7d9670 | 0x5436360 |
| ideology | CIdeologyDataBase | 0x7cf580 | 0x5433fc0 |
| pop_type | CPopTypeDatabase | 0x7d8440 | 0x5430620 |
| decree | CDecreeDatabase | 0x7d4c90 | 0x5434360 |
| tech | CTechnologyDatabase | 0x7d1b10 | 0x542f030 |
| movement | CPoliticalMovementTypeDatabase | 0x7d82c0 | 0x54335c0 |
| country_type | CCountryTypeDatabase | 0x7d4810 | 0x542ea00 |
| war_goal | CWarGoalTypeDatabase | 0x7ccb80 | 0x5436f10 |
| company_type | CCompanyTypeDatabase | 0x7cec80 | 0x5434bb0 |
| social_hierarchy | CSocialHierarchyDatabase | 0x7d9af0 | 0x5430760 |
| strait | CStraitDefinitionDatabase | 0x7ce680 | 0x542cd40 |
| ig_type | CInterestGroupTypeDatabase | 0x7cf700 | 0x54352e0 |
| subgoal | CObjectiveSubgoalDatabase | 0x7d79c0 | 0x5438340 |
| (event) | CEventDatabase, different layout: [db+8] array, [db+0x14] count, name +0x10 | 0x7cdd80 | 0x542eb20 |

Map modes: CMapModeDatabase getter 0x7ce200 (object 0x54347d0).

## 5. What console_plus does

In-game: on the main thread (`on_frame`, every `scan_frames` frames, only when the registry
count changed) it takes the registry writer lock with TryEnter only, checks the layout before the first write
(`help` description at +0x90, an engine completer at +0xb8 of `event`/`mapmode`). At the main
menu the registry is not complete yet (in-game smoke test: help missing, 0 completers), so a
"not yet" result is retried on every scan and nothing is written; it is OFF only if most entry
names are not readable strings, or if the check still fails 5 scans after a `load_transition`,
and for every entry with +0xb8 == 0 whose argument descriptions name something it knows, writes
+0xc0 = its own record and then +0xb8 = `cp_complete`. Descriptions are split on `,` and `> <`;
keywords decide the kind (state region before tag; "country ID"/"state ID" are numbers);
`a/b/c` and `a|b` give literal words ("all/tag", the 26 `debug` switches, `<0/1>`).
`cp_complete` (engine ABI, main thread only) counts the tokens in `previous` = argument index,
pushes the literal words, then the database keys (database vtable checked on every call), or for
`event name` calls the engine's own `event` completer. The engine filters/sorts/pages as before.
Commands with an engine completer are left alone unless `override_engine=1` or `[commands]`.
The id completion hooks nothing; display names (section 6, `names=1`) hook the matcher and Autocomplete. No engine code is patched; no game script runs.

Examples (1.15.1 descriptions): `tag` country; `annex` country; `own` state_region country;
`conquerall` country; `change_law` law country; `treatyport`, `check_pollution_level`,
`set_pollution_level` state_region; `add_journal_entry` & co journal; `create_country` country
country_type culture; `ai_evaluate_production_method` building production_method state_region;
`testevent` event country; `disable_ai`/`enable_ai` all + country; `debug` its switches;
`version` 0/1; `add_radicals`/`add_loyalists` culture; `create_political_movement` movement.

V3X console window: hook on kernel32 `ReadConsoleA` (all 5 arguments forwarded), active only on
the thread named `v3x_console_input` and only without an input-control argument. It reads with
`ReadConsoleW` + `CONSOLE_READCONSOLE_CONTROL{dwCtrlWakeupMask = 1<<'\t'}`; on Tab it rewrites the
line (cursor arithmetic on `CONOUT$`) and re-reads with `nInitialChars`. Candidates: names from
the core `help` listing (captured once via `exec_capture`, which also prints it once) and the
`a|b|c` words of each help text. The core starts its input thread only after all plugins are
loaded, so the hook is in place before the first read. (An earlier version cancelled the first
read with `CancelSynchronousIo`; that blocked the main thread forever about 1 run in 30-40 in
`test/shutdown_test.c`, so it was removed.)

## 6. Display names (`names=1`, worker namecomplete)

The engine's completer gets only the arguments BEFORE the token, and the engine keeps candidates
that CONTAIN the token, so a name like `Prús` can never reach `PRU` through the completer alone.

* **Hooks** (MinHook detours, 4 integer register args forwarded; both functions take <= 3):
  the matcher 0x3dc2140 `(manager, const CString *input, result)` and `CConsole::Autocomplete`
  0x36be560 `(this, mode)` (only to remember `this` while it runs). Pre-step on the main thread:
  input `cmd <args> <token>`, our record for `cmd`, kind of the argument at the token. If the
  folded token matches display names of that kind, the token is cut off the matcher's own (by-value,
  engine-owned) input IN PLACE (so the engine keeps every candidate) and our completer returns exactly the matching
  ids (ids that contain the token first, then name matches by rank). One id: the engine replaces
  the token with it. Several: `ID  (Name, context)` lines (first 15) are printed with the console's
  own output function, the engine shows `(found: N ...)` and Tab cycles through the ids.
  The engine's input pointer is always the one forwarded (section 3: the matcher frees it).
* **Console output**: `CConsole::AddOutput(this, const CString *text, bool scroll)` 0x36bdd90 (the
  call `mov r8b,1; lea rdx,[rsp+x]; mov rcx,r13; call` in Autocomplete, prologue `sub rsp,58h;
  vpxor`); its first call 0x36bdb20 copies the text into the ring buffer at +0xd70 (count +0xd7c,
  index +0xd88) and creates a `console_output` widget. It only copies the CString.
* **Localization** (`pdx_localize.cpp`): `CTransactionResource<CLocalizationManager>` getter
  0x3db2390 (object 0x5422af0, TLS transaction scope like the database getters, 164 callers).
  Manager +0x90 = current language table (+0x00 CString name `l_english`). Table +0x38 = hash map:
  +0x08 slot array, +0x14 mask, +0x18 max probe distance (byte); slot 0x30 bytes, +0x04 distance
  byte (0 = empty), +0x08 32-bit key hash (murmur3 0x3e806f0), +0x10 entry = key `{char*, int len,
  byte flag}` + value (same shape) at +0x20. Lookup 0x3db2180 (FNV over the hash bytes, robin hood),
  used as `call getter; mov rbx,[rax+90h]; ...; lea rcx,[rbx+38h]; call 0x3db2180`
  (e.g. 0x8fc1cf); value at entry+0x10 confirmed at 0x3dad16a (`CW_ORDINAL_NUMBER_FORMAT`).
  console_plus does not call the hash: on first use it walks all slots once and builds its own
  FNV-1a key index (rebuilt when language/slot array/mask change). Texts are raw: `$KEY$` (resolved
  2 levels), `#style ... #!`, `[Script]`, `@icon!` are removed, nothing is evaluated.
* **Name keys**: database key itself (`PRU`, `STATE_SAO_PAULO`, `law_monarchy`, `grain`, ...);
  events `<id>.t`. `CEventDatabase` getter 0x7cdd80 (`CTransactionResource<unique_ptr<...>>`, same
  prologue); layout from the engine `event` completer 0x86e290: `[db+8]` event*, `[db+14h]` count,
  name CString at event+0x10 (checked in that completer's code at run time). With names on, the
  `event` command gets our completer too; for the event argument it calls the engine's.
* **Numeric ids of live objects**: the console's own commands look objects up as
  `G = [[[exe+0x5c3ef90]+0x608]+0x120]`, slot array `{?, object*}` (16 bytes) at G+A, count at
  G+A+0xc, object = slot[id]+8 (null = free id). Decoded at run time from the OnExecute (+0xb0) of
  each command whose first argument is a numeric kind and whose other arguments name nothing:
  `invalidate_state` 0x9570d0 (A = 0x1c30), `invalidate_character` 0x956d40 (0x1300),
  `invalidate_country` 0x957460 (0x30), `invalidate_ig` 0x9577e0 (0x900), the building wage command
  0x8876b0 (0x2d80). Two commands that disagree turn the kind off. The game state is read WITHOUT
  the engine's game-state lock (0x3689380, taken by the commands when they execute): all live reads
  go through ReadProcessMemory. Fields are found at run time: the 8-aligned offset (< 0x400) that
  points into a known object set in >= 60 % of up to 256 sampled objects (country -> its
  CCountryDefinition, state -> state region template and owner country, interest group -> type and
  country, building -> type and state, character -> country); character names = the two fields that
  hold localization keys (MSVC string or `{ptr,len}`) in >= 50 % of the sample.
* **Matching** (`plugins/console_plus/cp_names.h`): UTF-8, ASCII case, Latin-1 + Latin Extended-A +
  Romanian letters folded (á->a, ç->c, ß->ss, Ł->l, ...), compact form without blanks/punctuation;
  score exact < prefix < later word starts with it < substring (2+ characters); ties: shorter name.
  `name@ctx` filters by context (owner tag, state). Blanks end the console token, so names with
  blanks are typed with `_` or without blanks (`Sao_Pa`, `saopa`); `own São Pa` lists the matches
  with a hint instead.
* **`find`**: in the GAME console type `find <text>` and press **Tab** - the matches of every kind
  are listed (Tab-driven by design: there is no engine command `find`, so Enter does nothing
  useful there; no engine command is registered). In the V3X console window `find <text>` is a
  normal V3X command and works on **Enter**; it searches only the indexes already built by an
  in-game Tab (index builds stay inside the game console's own completion call).

## 6b. Country wildcards on Enter (`wildcards=1`, worker tabfix)

In an argument the command describes as a country (kind `country`, not one of the command's own
literal words such as `disable_ai all`), one wildcard per line:

| Form | Expands to | Example |
|---|---|---|
| `*` | every country that is a subject of anyone (overlord is a live country) | `add_liberty_desire * -100` |
| `M:*` | every direct subject of the player's country | `add_liberty_desire M:* -100` |
| `TAG:*` | every direct subject of TAG (case-insensitive) | `add_liberty_desire GBR:* 20` |
| `ALL` | every live country | `add_liberty_desire ALL` |

The line is run once per matching tag through the console itself, then
`console_plus: ran <cmd> for N subjects (K ok)` is printed. No match / unknown TAG / unknown player /
two wildcards: a message and nothing runs. `add_liberty_desire <Country tag> [value, default 10]`
(OnExecute 0x88c440) finds the subject pact via G+0x1e38 (0xc1abb0) and adds value*100000, so it
works for subjects of any overlord, not only the player's.

* **Hook**: `CConsole::ExecuteLine(this, const CString &line) -> bool` 0x36bf740, found as the call in
  the queued-lines loop 0x36c0070 (`mov rdx,rbx; mov rcx,rdi; call; test al,al; jne; cmp [rdi+0D59h],al`,
  unique) and checked to call the manager getter. The line is const: the history insert 0x301a470
  copy-constructs it, the tokenizer 0x3de27e0 reads it, the caller destroys its own lines; so per-tag
  lines built in the plugin are safe here (unlike the matcher). Only the main thread; other lines pass through.
* **Overlord**: GetOverlord 0x93a030 (found via the call in the function that uses
  `"%s has no overlord. 'make_independent' requires an overlord"`): G = `[[[exe+0x5c3ef90]+0x608]+0x120]`,
  index = id & 0xffffff < [G+0x1f64], keys {int id, int slot} at [G+0x1f58], values {int overlord id,
  bool set} at [G+0x1f70]. Decoded from the code at run time (cp_wild.h `cp_decode_overlord`), read
  with ReadProcessMemory. Country full id at country+0x18 (checked against the id slot; mismatch = off).
* **Player**: `[[exe+0x5c3ef90]+0x608]+0xf8`, decoded from the OnExecute of every registry command with
  the inline "player country id" shape (TLS +0x53c check, `cp_decode_player`); 10 sites in 1.15.1, all
  agree; a disagreement turns `M:*` off.
* **Tags**: the live country display-name index (needs `names=1`).
* Not seen in game; each expanded line is a separate console history entry.

## 7. Not verified

* Nothing was run in the game: completer calls, the getters from inside the completer, the
  registry lock and the engine's display of the candidates are from static analysis only.
* Province ids are not completed.
* Display names (section 6) are offline-tested only: fake localization map / databases / game state
  in `test/console_plus_test.c` part B2, anchors and id-lookup decoding against the real exe in
  part C. Not seen in game: the hooks under the real console, the printed lines, the field
  discovery on real objects (offsets are logged once per session), character name fields,
  strategic region / event title localization keys, reading the game state while it ticks.
