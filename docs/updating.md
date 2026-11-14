# Updating V3X

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
- Every V3X release is **signed** by the V3X developers (Ed25519, file `compat.json.sig`). The updater
  installs a release on its own only if that signature is valid; the checksum it then checks comes from
  the signed file. A release without a valid signature is only announced (the console says
  *"NOT signed"*); it is installed only if you type `update now` yourself.
- Updates are never installed while you play. The new `V3X-Setup.exe` is downloaded to
  `binaries/v3x/update/` and its SHA-256 checksum is checked. When you quit the game, the checksum is
  checked again and the setup starts and swaps the files after the game has closed. Your settings
  (`*.ini`) are kept. The next start runs the new version.
- V3X never downgrades: `binaries/v3x/update/floor.txt` remembers the newest version that ran here, and
  nothing older is ever offered. If the download or the checksum check fails, nothing is installed and one line
  goes to `binaries/v3x/v3x.log`. With no internet you get a single log line and nothing else happens.
- If an automatic install fails 3 times for the same version, V3X stops retrying.
  `binaries/v3x/install.log` has the reason, and you can run `binaries/v3x/update/V3X-Setup.exe` by hand.

Console (the V3X console window):

| command | what it does |
|---|---|
| `update` | status: installed version, game build, what is available and why |
| `update check` | check now (at most once per minute) |
| `update now` | download the offered update and install it when you quit (needed with `auto = 0`, and for a release that is not signed) |
| `update cancel` | install nothing when the game exits (this session only) |

Settings, in `binaries/v3x/v3x.ini`:

```ini
[update]
check = 1         ; 0 = no automatic checks (only `update check`)
auto = 1          ; install SIGNED updates when I quit; 0 = only tell me; install with `update now`
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
- Toda versão do V3X é **assinada** pelos desenvolvedores (Ed25519, arquivo `compat.json.sig`). O
  atualizador só instala sozinho uma versão com assinatura válida; o checksum conferido vem do arquivo
  assinado. Uma versão sem assinatura válida só é anunciada (o console diz *"NOT signed"*) e só é
  instalada se você digitar `update now`.
- Nada é instalado durante a partida. O `V3X-Setup.exe` novo é baixado para `binaries/v3x/update/` e o
  checksum SHA-256 dele é conferido. Quando você fecha o jogo, o instalador abre e troca os arquivos depois
  que o jogo terminou de fechar. As suas configurações (`*.ini`) são mantidas.
- O V3X nunca volta para uma versão mais antiga (`binaries/v3x/update/floor.txt` guarda a versão mais
  nova que já rodou aqui). Se o download ou a conferência falhar, nada é instalado e
  só uma linha vai para `binaries/v3x/v3x.log`. Sem internet também fica só uma linha no log.
- Comandos no console: `update` (situação), `update check` (procurar agora), `update now` (instalar ao
  sair, quando `auto = 0`), `update cancel` (não instalar nada ao sair).
- Configuração em `binaries/v3x/v3x.ini`, seção `[update]`: `check = 0` desliga a busca automática,
  `auto = 0` só avisa sem instalar, `channel = beta` inclui versões de teste.

## For release makers: `compat.json` and `compat.json.sig`

Every GitHub release must have a `compat.json` asset and its signature `compat.json.sig`.
`installer/make_dist.py` writes `compat.json` from `installer/builds.txt` and `include/v3x.h`; the
`publish` job of `.github/workflows/release.yml` signs it with `tools/sign_release.py`:

```json
{"schema": 1, "v3x_version": "0.6.0", "loader_semver": "0.6.0", "api_version": 5,
 "game_builds": [{"branch": "release/1.15.2", "build_id": "<api->game_build_id()>"}],
 "assets": [{"name": "V3X-Setup.exe", "size": 123456, "sha256": "<64 hex>"},
            {"name": "v3x-0.6.0.zip", "size": 123, "sha256": "<64 hex>"},
            {"name": "install.ps1", "size": 12, "sha256": "<64 hex>"}]}
```

`compat.json.sig` is one ASCII line, `v3x-sig 1 <key id> <base64 Ed25519 signature>`, over the exact
bytes of `compat.json` (key id = first 8 bytes of SHA-512 of the public key, hex). The trusted public
keys are in `installer/release_keys.h` (CURRENT + NEXT rotation slot), compiled into V3X-Setup.exe
(`--verify-release`) and the updater (`plugins/updater/up_sig.c`, Monocypher), and copied into
`installer/install.ps1` (`$ReleaseKeyCurrent` / `$ReleaseKeyNext`, checked in PowerShell).

How the updater (`plugins/updater/up_logic.c` `up_select`) picks a release. The signature comes
first: nothing in `compat.json` (builds, API, hash, size) is trusted before `compat.json.sig` verified.

- If the release lists a `compat.json.sig`, it must verify first (bad signature, unknown key or a `.sig`
  that cannot be downloaded = the release is rejected). Then the setup's SHA-256 and size come from the
  signed `compat.json` only.
- It considers non-draft releases whose tag is a semantic version (`v0.6.0`). Pre-releases (a `-` in the
  version, or GitHub's pre-release flag) count only with `channel = beta`.
- The version must be newer than the running loader (`api->loader_version()`). Equal or older is never
  offered.
- `compat.json` must parse, its `v3x_version` must equal the tag, and its `api_version` must be at least
  the running loader's API version.
- A `game_builds` entry with a `build_id` matches only that exact build. An entry with just a `branch`
  matches every build of that branch, including hotfixes.
- The version must not be below `update/floor.txt` (the highest version that ran here or that the
  updater started).
- Of all releases that pass, the newest one wins.

> **Unsafe — when you really need it:** a release without `compat.json.sig` is *unsigned*: its SHA-256
> may also come from `SHA256SUMS.txt` (same server, so it only catches broken downloads, not a swapped
> release). The updater announces it but never installs it on its own (`auto = 1` never does); only an
> explicit `update now` installs it, with a warning. What can go wrong: whoever can change the release
> files can run code on your machine. Contain it: publish only signed releases; as a player, install an
> unsigned one only when you built it yourself.

The cache (`binaries/v3x/update_cache.json`) keeps a signed `compat.json` and its signature verbatim and
verifies them again on every read; a cache entry never makes a release "signed" by itself, and the setup
file name is always `V3X-Setup.exe`. At exit the verified file is opened without write/delete sharing,
hashed again through that handle and started while the handle is held. WinHTTP is loaded from System32
only.

Only URLs under `https://github.com/kleeedolinux/v3x/releases/download/` and
`https://api.github.com/repos/kleeedolinux/v3x/` are ever requested.

At exit the updater runs
`V3X-Setup.exe --update --silent --game-dir "<...>\Victoria 3" --wait-pid <game pid>`. On the next start
it reads the last `update-result:` line of `binaries/v3x/install.log`.

Plugins can subscribe to the `update_available` event (`plugins/updater/updater.h`, `V3X_UpdateInfo`;
main thread).

## Release signing (for the lead)

One-time setup. The private key never enters the repository or a checkout.

1. Generate the key on a trusted machine, outside any git checkout (the tool refuses a folder inside
   the repo):

   ```
   python tools/sign_release.py keygen --out-dir <offline folder> --name v3x-release
   ```

   It writes `v3x-release.seed.b64` (PRIVATE, base64 of the 32-byte seed) and `v3x-release.pub.txt`,
   and prints the public key and key id. Keep an offline backup of the seed file (for example on two
   USB sticks): losing it means a key rotation, leaking it means every player trusts the thief.
2. Put the public key (64 hex digits) into `installer/release_keys.h`
   (`#define V3X_RELEASE_KEY_CURRENT "<hex>"`) and the same value into `installer/install.ps1`
   (`$ReleaseKeyCurrent = '<hex>'`). `python installer/test_installer.py` fails if the two differ.
   Commit both (public data). Until then the release workflow stops at "Release key configured".
3. GitHub, repository Settings > Environments > New environment `release`:
   - Deployment branches and tags: *Selected branches and tags*, add the tag rule `v*`.
   - Required reviewers: yourself (every release waits for your approval before it is signed).
   - Environment secret `V3X_SIGNING_KEY` = the content of `v3x-release.seed.b64` (one line).
   Do not create it as a repository secret: only the `publish` job, which runs in this environment,
   may read it.
4. Settings > Rules > Rulesets > New tag ruleset, target `v*`: restrict creations, updates and
   deletions (bypass: you / repository admin), so nobody else can push or move a release tag.
   Settings > Actions > General: Workflow permissions = "Read repository contents" (the workflows ask
   for more only where needed). Optionally Settings > General > Releases > "Enable release immutability".
5. Check a published release: `python tools/sign_release.py verify compat.json` and
   `python tools/sign_release.py verify plugins.json` (use `release_keys.h`),
   `V3X-Setup.exe --verify-release <folder with the downloaded assets>`, and
   `gh attestation verify V3X-Setup.exe --repo kleeedolinux/v3x` (build provenance).

What the `publish` job signs, in this order (`.github/workflows/release.yml`):
`plugins.json` (the official plugin list: name, file, SHA-256, version of every shipped DLL; the
loader treats a plugin as official only when `binaries/v3x/plugins.json.sig` verifies), then
`python installer/make_dist.py --add-sig dist` puts `plugins.json.sig` into the setup payload and the
zip (refused if `plugins.json` is not byte-identical to the payload's) and rewrites the asset hashes,
then `compat.json`, which names those final hashes. The release notes print the SHA-256 of
`install.ps1` for the verifiable two-step install (installer/README.md "Verifiable install").

Key rotation: generate the next key, put its public key into the NEXT slots (`V3X_RELEASE_KEY_NEXT`,
`$ReleaseKeyNext`) and publish at least one release signed with the CURRENT key, so installed updaters
already trust the next key. Then replace the secret, move the key to CURRENT and empty NEXT.

Dry run with a development key (never for a published release):
`python tools/sign_release.py keygen --out-dir D:/v3x_keys --name v3x-dev`, then
`python installer/make_dist.py --no-build --out <dir> --version 0.6.0 --trust-key <dev public key>`,
`V3X_SIGNING_KEY=<seed> python tools/sign_release.py sign <dir>/plugins.json --allow-key <dev public key>`,
`python installer/make_dist.py --add-sig <dir>`,
`V3X_SIGNING_KEY=<seed> python tools/sign_release.py sign <dir>/compat.json --allow-key <dev public key>`
and `<dir>/V3X-Setup.exe --verify-release <dir>`. The installer tests do the same with the public TEST
key of `tools/sign_release.py` (`TEST_SEED`), which no release build trusts.
