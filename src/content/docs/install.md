---
title: "Installing V3X (players)"
summary: "V3X-Setup.exe: download, install, what it changes, repair, uninstall."
group: players
order: 1
source: "docs/install.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/install.md"
updated: "2026-11-13"
---
V3X adds engine fixes and quality-of-life features to Victoria 3 **1.15.1**. It needs no admin
rights and changes no game file. Em português: [mais abaixo](#português-pt-br).

> **Single player only.** Do not use V3X in multiplayer. Before you play online, uninstall it, or
> rename `binaries\version.dll` (for example to `version.dll.off`) and rename it back afterwards.

## 1. The easy way: one line

1. Close Victoria 3.
2. Press the **Windows key**, type `powershell`, press **Enter**. A blue or black window opens.
3. Copy this line, paste it into that window (right-click pastes), press **Enter**:

   ```powershell
   irm https://v3extender.pages.dev/install.ps1 | iex
   ```

   (Same thing from GitHub: `irm https://github.com/kleeedolinux/v3x/releases/latest/download/install.ps1 | iex`.)
4. It says `Found your game at ... Is this right? [Y/n]`. Press **Enter** if the folder is right.
   If it cannot find the game, paste the game folder (or press Enter and pick it in a window). In
   Steam: right-click Victoria 3 > **Manage** > **Browse local files** shows that folder.
5. Wait for `Done! V3X ... is installed.` Start the game as usual.

What it does: it finds the game through Steam, downloads `V3X-Setup.exe` from the latest
[release](https://github.com/kleeedolinux/v3x/releases), checks its SHA-256 against
`SHA256SUMS.txt`, and runs it. Nothing is changed if the download is damaged or the game is open.
It speaks English or Portuguese, following your Windows language (`$env:V3X_LANG="en"` or `"pt"`
before the line overrides it).

**Update, repair or uninstall:** run the same line again. When V3X is already installed it asks:
`1 Update or repair`, `2 Uninstall`, `3 Stop`, `4 Check my install`.

**Something wrong? Check my install:** choose `4` there, or the **Check my install** button in
`V3X-Setup.exe`. It changes nothing. It looks for missing or damaged V3X files, other mod loaders
next to the game (`dxgi.dll`, `dinput8.dll`, ...), a game version V3X does not know, low disk space
(also on C:), a too small Windows page file, mistakes in `v3x.ini`, a plugin that stopped the last
start, and the last crash. Each line says OK, WARNING or PROBLEM plus what to do; if files are
missing it offers **Repair**. The result is also saved in `binaries\v3x\doctor_offline.txt`.

## 2. The other way: V3X-Setup.exe

Get `V3X-Setup.exe` from the [Releases page](https://github.com/kleeedolinux/v3x/releases) (or from
whoever sent it to you). The release also lists its SHA-256 checksum (`SHA256SUMS.txt`) if you want
to check the file.

Windows SmartScreen or your antivirus may warn, because the file is new and not signed: choose
**More info > Run anyway** if you trust the source. The [FAQ](/docs/faq/) explains why tools like V3X
get flagged.

1. Close Victoria 3 (setup refuses to run while the game is open).
2. Double-click `V3X-Setup.exe`. It finds Victoria 3 through Steam. If it cannot, it asks you for
   the folder: in Steam, right-click Victoria 3 > **Manage** > **Browse local files**, and pick that
   `Victoria 3` folder.
3. Check that the window says `Game version: release/1.15.1 (supported)`, then click
   **Install V3X**. (The window is in Portuguese when Windows is.)
4. Start the game as usual (Steam or the Paradox launcher). A small **V3X console** window opens
   next to the game and lists what loaded. Hide it with `console=0` in `binaries\v3x\v3x.ini`.

If the game version is not supported (a newer patch), setup says so and installs only if you
click **Install anyway**. Plugins switch themselves off where the game's code changed, but that
combination was never tested: waiting for a V3X update is the safe choice.

The release also has `v3x-<version>.zip` for a manual install: copy `version.dll` and the `v3x`
folder from it into the game's `binaries` folder. Manual uninstall: delete them again.

## 3. Everything is installed, most of it is off

V3X installs all its player plugins, but only four are **on**:

| on | what it does |
|---|---|
| `crash_logger` | writes `v3x\crash.log` when the game crashes, so you can send it with a bug report |
| `content_packs` | loads extra game files from `v3x\content_packs\`; does nothing when that folder is empty |
| `mod_menu` | the **V3X** button on the main menu, where you turn the others on and off |
| `script_ext` | lets plugins add game script and text; does nothing by itself |

Everything else is installed but **off**. To turn something on: start the game, click **V3X** on
the main menu, switch it on, restart the game. (Or edit `binaries\v3x\v3x.ini`: `[plugins]`,
`keep_history=1`.) Changes apply at the next start.

| off (turn on if you want it) | what it does |
|---|---|
| `keep_history` | keeps a country's graphs when you switch to another country |
| `ironman_console` | the developer console also works in ironman (can matter for achievements) |
| `log_filter` | drops noisy lines from the game's log files |
| `frame_pacer` | smoother frame cap, less CPU use when the game is idle or in the background |
| `cpu_sched` | puts the game on the fast cores of Intel hybrid CPUs (does nothing on others) |
| `gfx_driver` | fewer graphics driver threads, can help weak / integrated GPUs |
| `mem_tuner` | uses less memory and survives one out-of-memory moment |
| `fast_load` | measures loading time; other plugins use it |
| `console_plus` | Tab completion in the developer console |
| `loc_fallback` | shows English text where a mod forgot to translate |
| `enhance` | extra single-player options (see its ini file) |
| `asset_stream` | streams textures instead of loading all at once |
| `lazy_mesh` | loads 3D models only when needed (also needs `mode=lazy` in `lazy_mesh.ini`) |
| `sim_opt` | game-speed experiments; back up your saves first |
| `map_cache` | measures (and, with `mode=on`, avoids) re-reading map textures |
| `mod_loader` | measures (and, with `mode=on`, cuts) the loading cost of mods |
| `par_load` | measures the game's start-up steps |
| `lazy_gfx` | measures texture loading |
| `fast_lookup` | measures (and, with `mode=on`, speeds up) some engine lookups |
| `tick_par` | measures how the game's daily tick uses your CPU cores |
| `ai_par` | measures AI and economy work per tick |

The "measures" ones only write reports into `binaries\v3x\` unless you change their ini file.
Details for each: [plugins.md](/docs/plugins/). Developer tools (`devtools`, `devconsole`, `devbridge`,
`tick_profiler`, `alloc_tracker`, `hello`) are not installed.

## 4. What it changes

Everything goes into the game's `binaries` folder:

| file | what it is |
|---|---|
| `binaries\version.dll` | the V3X loader. Windows loads it with the game; it loads the plugins |
| `binaries\v3x\plugins\*.dll` | the plugins |
| `binaries\v3x\*.ini` | settings. Created only if missing: setup never overwrites your settings |
| `binaries\v3x\install_manifest.txt` | the list of files setup installed (with checksums), used by repair and uninstall |
| `binaries\v3x\install.log` | what setup did |
| `binaries\v3x\backup\version.dll` | only if another tool's `version.dll` was there before: it is kept here and put back on uninstall |

While you play, V3X writes logs and reports into `binaries\v3x\` (`v3x.log`, `crash.log`, ...).
Steam's "Verify integrity of game files" does not remove V3X.

An update keeps your `v3x.ini`: plugins you turned on stay on. A plugin that is new to you is
added as off.

## 5. Repair, update, uninstall

Run the one line again (choose 1 or 2), or run `V3X-Setup.exe` again:

* **Repair / update V3X** copies back any V3X file that is missing or damaged (or replaces them
  with the newer version). Your settings are kept.
* **Uninstall V3X** removes exactly the files setup installed and puts back a `version.dll` that
  was there before. Logs, reports and settings you changed stay in `binaries\v3x\`; tick
  *Also delete logs, reports and settings* (or answer `y` in PowerShell) to remove that folder too.

Lost the setup file? Delete `binaries\version.dll` and the `binaries\v3x` folder by hand. That is
the whole uninstall.

## 6. Troubleshooting

| problem | what to do |
|---|---|
| "Victoria 3 is running" / "is open right now" | close the game (check the task manager for `victoria3.exe`), then try again |
| "Victoria 3 was not found" | pick the `Victoria 3` folder (the one with the `binaries` folder) |
| "The download is damaged" / "setup file is damaged" | download again; the file is checked before anything is written |
| the PowerShell line prints red errors | send a screenshot, and use `V3X-Setup.exe` instead |
| "could not write ... is it in use?" | close the game and any tool that may hold the file, then run **Repair** |
| the game does not start or crashes right away | rename `binaries\version.dll` to start without V3X. If that helps, turn plugins off one by one in the V3X menu or `v3x.ini`, or uninstall |
| something else | send `binaries\v3x\v3x.log`, `crash.log`, `crash.prev.log` and `install.log`. Copy them before you restart: the next start overwrites the logs |

More: [troubleshooting.md](/docs/troubleshooting/), [faq.md](/docs/faq/).

## For scripts and testing

`V3X-Setup.exe --silent [--game-dir "<...>\Victoria 3"] [--repair | --uninstall [--purge]] [--force] [--status]`
prints to stdout (always English) and returns 0 ok, 1 error, 2 unsupported game build, 3 game
running, 4 game not found, 5 not installed, 6 damaged setup file, 7 bad arguments.

* `--doctor` runs the offline checks (ids `D001`... shared with the in-game doctor plugin): one
  English line per check plus a machine line `doctor: <id> <name> OK|WARN|PROBLEM <detail>` and
  `doctor: result problems=N warnings=N repair=0|1`. Exit code = number of PROBLEMs.
* `--update [--wait-pid <pid>]` is what the in-game updater runs: wait (up to 10 minutes) for the
  game process to exit, then a silent repair / upgrade that keeps every ini; never forced onto an
  unsupported game build. Exit 0 ok, 1 anything else; the last line of `binaries\v3x\install.log` is
  `update-result: ok version=<ver>` or `update-result: failed code=<n> <reason>`.
* Each release also has `compat.json` (version, loader API, supported game builds from
  `installer/builds.txt`, asset sizes and SHA-256) for the updater.

`install.ps1` takes environment variables instead of parameters (`iex` cannot pass any):
`V3X_ACTION=install|repair|uninstall|status|doctor`, `V3X_GAME_DIR`, `V3X_LANG=en|pt`, `V3X_PURGE=1`,
`V3X_YES=1` (never ask), `V3X_SOURCE=<folder or URL>` (where to download from), `V3X_VERSION=v0.5.0`
(a specific release). Example: `$env:V3X_ACTION="uninstall"; irm https://v3extender.pages.dev/install.ps1 | iex`.
Details: [installer/README.md](https://github.com/kleeedolinux/v3x/blob/main/installer/README.md).

---

## Português (pt-BR)

O V3X traz correções do motor do jogo e melhorias para o Victoria 3 **1.15.1**. Não precisa de
administrador e não altera nenhum arquivo do jogo.

> **Só para um jogador.** Não use o V3X no multijogador. Antes de jogar online, desinstale, ou
> renomeie `binaries\version.dll` (por exemplo para `version.dll.off`) e volte o nome depois.

### Instalar com uma linha

1. Feche o Victoria 3.
2. Aperte a **tecla Windows**, digite `powershell`, aperte **Enter**. Abre uma janela azul ou preta.
3. Copie esta linha, cole nessa janela (botão direito cola) e aperte **Enter**:

   ```powershell
   irm https://v3extender.pages.dev/install.ps1 | iex
   ```

4. Ele diz `Achei seu jogo em ... Está certo? [S/n]`. Aperte **Enter** se a pasta estiver certa.
   Se ele não achar o jogo, cole a pasta do jogo (ou aperte Enter e escolha numa janela). Na Steam:
   botão direito em Victoria 3 > **Gerenciar** > **Ver arquivos locais** mostra essa pasta.
5. Espere aparecer `Pronto! O V3X ... está instalado.` Abra o jogo como sempre.

Ele acha o jogo pela Steam, baixa o `V3X-Setup.exe` da última versão no GitHub, confere o SHA-256
e roda. Se o download vier com defeito ou o jogo estiver aberto, nada é alterado. O texto sai em
português quando o Windows está em português.

**Atualizar, reparar ou desinstalar:** rode a mesma linha de novo e escolha `1 Atualizar ou
reparar` ou `2 Desinstalar`. Outra forma: rode o `V3X-Setup.exe` (a janela também fica em
português).

**Algo errado? Verificar instalação:** escolha `4` nesse menu, ou o botão **Verificar instalação**
no `V3X-Setup.exe`. Não muda nada. Procura arquivos do V3X faltando ou danificados, outros
carregadores de mods ao lado do jogo, versão do jogo desconhecida, pouco espaço em disco (também no
C:), arquivo de paginação pequeno, erros no `v3x.ini`, um plugin que parou a última abertura e o
último travamento. Cada linha diz OK, AVISO ou PROBLEMA e o que fazer; se faltar arquivo, oferece
**Reparar**.

### Tudo vem instalado, quase tudo desligado

Só quatro plugins vêm **ligados**: `crash_logger` (grava `v3x\crash.log` quando o jogo trava, para
mandar junto com um relato de bug), `content_packs` (carrega arquivos extras de
`v3x\content_packs\`; sem nada lá, não faz nada), `mod_menu` (o botão **V3X** no menu principal,
onde você liga e desliga os outros) e `script_ext` (deixa plugins adicionarem script e texto; sozinho
não faz nada).

O resto vem instalado, mas **desligado**. Para ligar: abra o jogo, clique em **V3X** no menu
principal, ligue o que quiser e reinicie o jogo. (Ou edite `binaries\v3x\v3x.ini`: `[plugins]`,
`keep_history=1`.)

| desligado (ligue se quiser) | o que faz |
|---|---|
| `keep_history` | mantém os gráficos de um país quando você troca de país |
| `ironman_console` | o console de desenvolvedor funciona também no ironman (pode afetar conquistas) |
| `log_filter` | tira linhas inúteis dos logs do jogo |
| `frame_pacer` | limite de FPS mais suave, menos CPU com o jogo parado ou em segundo plano |
| `cpu_sched` | põe o jogo nos núcleos rápidos de CPUs Intel híbridas (nos outros não faz nada) |
| `gfx_driver` | menos threads do driver de vídeo, ajuda GPUs fracas / integradas |
| `mem_tuner` | usa menos memória e aguenta um momento de falta de memória |
| `fast_load` | mede o tempo de carregamento; outros plugins usam |
| `console_plus` | completar com Tab no console de desenvolvedor |
| `loc_fallback` | mostra o texto em inglês onde um mod esqueceu de traduzir |
| `enhance` | opções extras para um jogador (veja o arquivo ini dele) |
| `asset_stream` | carrega texturas aos poucos em vez de tudo de uma vez |
| `lazy_mesh` | carrega modelos 3D só quando precisa (precisa também de `mode=lazy` em `lazy_mesh.ini`) |
| `sim_opt` | experimentos de velocidade do jogo; faça backup dos saves antes |
| `map_cache`, `mod_loader`, `par_load`, `lazy_gfx`, `fast_lookup`, `tick_par`, `ai_par` | medem partes lentas do jogo e gravam relatórios em `binaries\v3x\`; alguns aceleram com `mode=on` no ini |

Ferramentas de desenvolvedor (`devtools`, `devconsole`, `devbridge`, `tick_profiler`,
`alloc_tracker`, `hello`) não são instaladas. Uma atualização mantém seu `v3x.ini`: o que você
ligou continua ligado.

### Problemas

| problema | o que fazer |
|---|---|
| "O Victoria 3 está aberto" | feche o jogo (veja no gerenciador de tarefas se `victoria3.exe` sumiu) e tente de novo |
| "Não consegui achar o Victoria 3" | escolha a pasta `Victoria 3` (a que tem a pasta `binaries`) |
| "O download veio com defeito" | tente de novo mais tarde; nada foi alterado |
| a linha do PowerShell mostra erros em vermelho | mande uma captura de tela e use o `V3X-Setup.exe` |
| o jogo não abre ou trava logo | renomeie `binaries\version.dll` para abrir sem o V3X; se resolver, desligue plugins um por um no menu V3X, ou desinstale |
| outra coisa | mande `binaries\v3x\v3x.log`, `crash.log`, `crash.prev.log` e `install.log` (copie antes de abrir o jogo de novo) |

Desinstalar sem o instalador: apague `binaries\version.dll` e a pasta `binaries\v3x`.
