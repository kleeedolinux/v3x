# Frontend GUI and data binding: GUI functions registered from native code (1.15.1)

Reverse-engineered for `plugins/mod_menu` (worker modmenu). RVAs are in `binaries/victoria3.exe`
1.15.1. Offline check: `test/mod_menu_test.c` (part E reads the real exe; part C the real
`game/gui/frontend/frontend_main.gui`). "Verified" here means checked statically by those tests or by
disassembly; **nothing in this document has been run in the game yet.**

## 1. Main menu GUI (verified on the files)

* `game/gui/frontend/frontend_main.gui` holds the top-level `widget` `mainmenu_panel_bottom` (the
  title screen). The engine creates it by name; a new top-level widget in another file would never
  be instantiated, so the only way to add something is to override this file (same VFS path).
* Buttons in it call data-binding expressions: global functions without a context
  (`onclick = "[OpenWikiUrl]"`, `"[OpenAchievements]"`, `"[ToggleThemeSettings]"`), methods of the
  view (`"[FrontEndMainView.OnSettings]"`), and the GUI variable store
  `GetVariableSystem.Set/Clear/Toggle/Exists/HasValue` (string keys, session lifetime, pure GUI
  state; already used on this screen for the theme tabs). A widget may have several `onclick` lines;
  all run (vanilla lines 572-577).
* Useful vanilla building blocks (frontend/shared/*.gui): `button_checkbox` (`checked = "[expr]"`),
  `header_pattern` (blockoverride `header_text`, `button_close`), `Window_Background_Popup`,
  `raw_text` (literal text, no localization key), `Not(...)`.

## 2. How GUI global functions are registered (verified by disassembly)

* Every global function is registered by a CRT static initializer (pointer in the `.rdata`
  initializer table; for `OpenWikiUrl` the registrar is 0x2cd050, pointer at .rdata 0x45638e8).
  Shape of a void (action) function registrar:

```
lea  rcx,[rbp-19h] ; call 0x678200          ; CPdxArray ctor (argument names of the description)
vmovsd xmm0,[rip+"OpenWikiUrl"] ...         ; name std::string built in place (SSO, cap 15)
lea  rdx,[rbp-39h] ; lea rcx,[rbp+1Fh] ; call 0x794c20   ; description object {string, array}
mov [rsp+30h],rax ; mov [rsp+28h],ebx ; mov [rsp+20h],rbx ; xor r9d,r9d
lea  r8,[rip+exec] ; xor edx,edx ; lea rcx,[rbp-1] ; call 0x796350
```

* `0x796350 register(const std::string *name, void *a2 /*0*/, exec, void *a4 /*0*/, void *a5 /*0*/,
  int a6 /*0*/, desc *d)` builds a 0x80-byte entry {name, a6, type id (magic static), a2, a4, a5,
  exec, desc string, desc array}, calls 0x3d4c780 and returns its bool. It destroys `d` (the caller
  only destroys the name). 74 call sites; 0x7965f0 (40) and 0x796890 (184) are sibling registrars,
  and typed getters use others (`GetWikiUrl` goes through 0x20d9700 with the callback in rdx).
* `0x3d4c780`: if the byte at 0x5cc6c56 is 0 (data-type system not built yet) the entry is appended
  to a pending list (0x3d48b90, magic static); else it is registered live via 0x3d4c6e0 on the
  registry pointer at 0x5c40a10.
* The data-type system constructor 0x3d4a080 (called from 0x33702f0 / 0x3783de0) sets 0x5cc6c56 = 1
  and flushes the pending list (0x3d4a390 -> 0x3d4c6e0).
* exec ABI for void functions: `bool exec(void *, void *, void *args)`. OpenWikiUrl's exec 0x2090a00
  is `sub rsp,38h; mov r9,r8; lea rcx,[fn]; call 0x7492f0`; the adapter 0x7492f0 copies the argument
  array (count at `[[r9]+0xc]`), calls the real function and returns `al = 1`. 33 of the 74 exec
  callbacks of 0x796350 use this adapter.
* Strings `Global Functions`, `Global Promotes`, `DumpDataTypes` exist (data types explorer /
  dump), which is the in-game way to confirm a registration (inferred, not run).

## 3. What mod_menu does with it

1. At plugin load (main thread, before CRT startup, so before the static initializers):
   `find_string("OpenWikiUrl")` -> the unique `C5 FB 10 05 disp32` (vmovsd) reference -> function
   start -> byte checks of the four call patterns above and of the prologues of 0x678200, 0x794c20,
   0x796350 and of the void exec adapter shape (`mm_check_registrar`, plugins/mod_menu/mm_resolve.c).
   Any mismatch: nothing is hooked, vanilla menu. Then MinHook on the registrar (all 8 args forwarded).
2. When the CRT runs the registrar, the detour calls the original and then registers
   `V3XToggle<i>` for every toggleable plugin exactly like the engine does (same function, same
   context: CRT init, pending list): SSO name, description = empty string + array from the engine
   ctor, a2/a4/a5/a6 = 0. Our exec only increments a click counter and returns 1.
3. Only if every registration returned true it writes
   `binaries/v3x/content_packs/v3x_menu/gui/frontend/frontend_main.gui` = the vanilla file with two
   insertions (`mm_gen_gui`): a `V3X` button_group before the Settings button_group (anchor
   `"[FrontEndMainView.OnSettings]"`) and a window as the last child of `mainmenu_panel_bottom`.
   Both anchors must occur exactly once; the block must balance. content_packs mounts the pack when
   the VFS initializes (after static init). The stale file is deleted at every load first.
4. Checkbox: `onclick = "[GetVariableSystem.Toggle('v3x_mm_t<i>')]"` + `onclick = "[V3XToggle<i>]"`;
   `checked` = state at start XOR that variable; a `restart required` label is visible while the
   variable exists. on_frame (main thread) applies odd click counts and rewrites the `[plugins]`
   section of `binaries/v3x/v3x.ini` (temp file + rename).

Not attempted: functions with arguments or return values (would need the engine's any-type ABI and
string allocation); live plugin on/off (plugins hook at load, none can unload).

## 4. Not verified / risks

* Nothing was run in game: that the flushed entries resolve `[V3XToggle<i>]` in the GUI, that the
  engine calls exec with the assumed arguments, the generated layout and its look.
* The registrar is assumed to run on the main thread during CRT init and before the data-type system
  constructor (strongly implied by the pending-list design, not observed).
* If mod_menu.dll is removed or disabled while `content_packs/v3x_menu/` exists, the stale GUI refers
  to unregistered functions (the checkboxes do nothing; inferred to be a logged GUI error, not a
  crash). **Delete `binaries/v3x/content_packs/v3x_menu/` when removing or disabling mod_menu.**
* Mods vs the menu. My first note had this backwards: a mod is not hidden by the pack. Per
  docs/re/vfs.md, mods/DLC are mounted by dlc.cpp after content_packs mounts, and each mount goes on
  top, so **a Paradox mod with its own `gui/frontend/frontend_main.gui` wins and the V3X menu does
  not show**. Basing the generated file on the mod's version (the file the game really uses) is not
  possible, for two reasons: (a) the menu is generated during CRT static init, before the VFS exists
  (it cannot be read through the VFS then), and (b) even a merged file would still sit below the
  mod. Fixing it would mean mounting `v3x_menu` above the mods (a change in content_packs or a hook
  after dlc.cpp's mounts), which is not done. Instead, mod_menu reads the launcher's
  `Documents/Paradox Interactive/Victoria 3/content_load.json` (`enabledMods[].path`; found through
  `%USERPROFILE%`, then `%OneDrive%`). For every enabled mod that has that file it logs a WARNING,
  and `modmenu` shows it. The console `modmenu toggle <plugin>` still works then. On this machine
  none of the 23 enabled mods overrides the file (checked 2026-11-13). Two things are inferred, not
  verified: that the game reads content_load.json, and that list order means mount order.
* The core must honour `[plugins] name=0` (skip LoadLibrary) for the toggles to take effect.
