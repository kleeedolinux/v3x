---
title: "Updating V3X"
summary: ""
group: modders
order: 50
source: "docs/updating.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/updating.md"
updated: "2026-11-13"
---
V3X updates itself with the bundled `updater` plugin. It installs only releases that support **your**
game version.

## For players (English)

- About 20 seconds after the game starts, V3X checks GitHub for new releases. This runs in the
  background, at most once every 6 hours, and again right away after the game itself was patched.
  The game never waits for it.
- A V3X release says which game builds it supports. If your game is still 1.15.1, a V3X release made only
  for 1.15.2 is **not** offered. The console then says *"V3X x.y.z exists but does NOT support your
  game"*. Once Steam updates your game to 1.15.2, that release is installed automatically.
- If the game was patched and your V3X does not know the new build yet, most plugins switch themselves
  off (safe mode). The updater still runs. When a V3X release for the new build exists, it tells you so
  and installs it.
- Updates are never installed while you play. The new `V3X-Setup.exe` is downloaded to
  `binaries/v3x/update/` and its SHA-256 checksum is checked. When you quit the game, the setup starts and
  swaps the files after the game has closed. Your settings (`*.ini`) are kept. The next start runs the
  new version.
- V3X never downgrades. If the download or the checksum check fails, nothing is installed and one line
  goes to `binaries/v3x/v3x.log`. With no internet you get a single log line and nothing else happens.
- If an automatic install fails 3 times for the same version, V3X stops retrying.
  `binaries/v3x/install.log` has the reason, and you can run `binaries/v3x/update/V3X-Setup.exe` by hand.

Console (the V3X console window):

| command | what it does |
|---|---|
| `update` | status: installed version, game build, what is available and why |
| `update check` | check now (at most once per minute) |
| `update now` | with `auto = 0`: download the offered update and install it when you quit |
| `update cancel` | install nothing when the game exits (this session only) |

Settings, in `binaries/v3x/v3x.ini`:

```ini
[update]
check = 1         ; 0 = no automatic checks (only `update check`)
auto = 1          ; 0 = only tell me; install with `update now`
channel = stable  ; beta = pre-releases too (e.g. 0.7.0-rc1)
```

## Para jogadores (português do Brasil)

- Uns 20 segundos depois de o jogo abrir, o V3X procura novas versões no GitHub. Isso roda em segundo
  plano, no máximo uma vez a cada 6 horas, e de novo logo depois de o próprio jogo ser atualizado. O jogo
  nunca espera por isso.
- Cada versão do V3X diz quais builds do jogo ela suporta. Se o seu jogo ainda está na 1.15.1, uma versão
  do V3X feita só para a 1.15.2 **não** é oferecida, e o console avisa que ela existe mas não suporta o
  seu jogo. Quando a Steam atualizar o jogo para a 1.15.2, essa versão é instalada sozinha.
- Se o jogo foi atualizado e o seu V3X ainda não conhece o novo build, a maioria dos plugins se desliga
  (modo seguro). O atualizador continua funcionando: quando sair uma versão do V3X para o novo build, ele
  avisa e instala.
- Nada é instalado durante a partida. O `V3X-Setup.exe` novo é baixado para `binaries/v3x/update/` e o
  checksum SHA-256 dele é conferido. Quando você fecha o jogo, o instalador abre e troca os arquivos depois
  que o jogo terminou de fechar. As suas configurações (`*.ini`) são mantidas.
- O V3X nunca volta para uma versão mais antiga. Se o download ou a conferência falhar, nada é instalado e
  só uma linha vai para `binaries/v3x/v3x.log`. Sem internet também fica só uma linha no log.
- Comandos no console: `update` (situação), `update check` (procurar agora), `update now` (instalar ao
  sair, quando `auto = 0`), `update cancel` (não instalar nada ao sair).
- Configuração em `binaries/v3x/v3x.ini`, seção `[update]`: `check = 0` desliga a busca automática,
  `auto = 0` só avisa sem instalar, `channel = beta` inclui versões de teste.

## For release makers: `compat.json`

Every GitHub release must have a `compat.json` asset. `installer/make_dist.py` writes it from
`installer/builds.txt` and `include/v3x.h`:

```json
{"v3x_version": "0.6.0", "loader_semver": "0.6.0", "api_version": 5,
 "game_builds": [{"branch": "release/1.15.2", "build_id": "<api->game_build_id()>"}],
 "assets": {"setup": "V3X-Setup.exe", "zip": "v3x-0.6.0.zip", "install_ps1": "install.ps1",
            "sha256": {"V3X-Setup.exe": "<64 hex>", "v3x-0.6.0.zip": "<64 hex>", "install.ps1": "<64 hex>"}}}
```

How the updater (`plugins/updater/up_logic.c` `up_select`) picks a release:

- It considers non-draft releases whose tag is a semantic version (`v0.6.0`). Pre-releases (a `-` in the
  version, or GitHub's pre-release flag) count only with `channel = beta`.
- The version must be newer than the running loader (`api->loader_version()`). Equal or older is never
  offered.
- `compat.json` must parse, its `v3x_version` must equal the tag, and its `api_version` must be at least
  the running loader's API version.
- A `game_builds` entry with a `build_id` matches only that exact build. An entry with just a `branch`
  matches every build of that branch, including hotfixes.
- The setup asset (`assets.setup`) needs a SHA-256: from `assets.sha256`, otherwise from
  `SHA256SUMS.txt`.
- Of all releases that pass, the newest one wins.

Only URLs under `https://github.com/kleeedolinux/v3x/releases/download/` and
`https://api.github.com/repos/kleeedolinux/v3x/` are ever requested.

At exit the updater runs
`V3X-Setup.exe --update --silent --game-dir "<...>\Victoria 3" --wait-pid <game pid>`. On the next start
it reads the last `update-result:` line of `binaries/v3x/install.log`.

Plugins can subscribe to the `update_available` event (`plugins/updater/updater.h`, `V3X_UpdateInfo`;
main thread).
