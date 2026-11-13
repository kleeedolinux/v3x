# V3X one-line installer for Victoria 3 (Windows PowerShell 5.1 and PowerShell 7). No admin needed.
#
#   irm https://v3extender.pages.dev/install.ps1 | iex
#   irm https://github.com/kleeedolinux/v3x/releases/latest/download/install.ps1 | iex
#
# What it does: finds Victoria 3 (Steam registry + libraryfolders.vdf, appid 529340, or asks),
# waits until the game is closed, downloads V3X-Setup.exe + compat.json + compat.json.sig from the
# latest GitHub release and checks them BEFORE running anything:
#   1. compat.json.sig must be a valid Ed25519 signature over compat.json by one of the release keys
#      below ($ReleaseKeyCurrent / $ReleaseKeyNext = installer/release_keys.h), checked here in
#      PowerShell (Test-Ed25519), so a swapped download cannot vouch for itself;
#   2. V3X-Setup.exe's size and SHA-256 must be the ones the signed compat.json names;
#   3. "V3X-Setup.exe --verify-release <folder>" must agree (the signed exe checks the same chain).
# Only then it runs "V3X-Setup.exe --silent --game-dir <game>" (the tested installer does the real
# work: installer/setup.c) and explains the result in plain words. An unsigned release, an unknown
# key or any mismatch: nothing is run and nothing is changed.
# Run it again to update, repair or uninstall.
#
# iex cannot pass parameters, so options are environment variables:
#   V3X_ACTION   install | repair | uninstall | status | doctor   (default: install; asks when V3X is installed)
#                doctor = V3X-Setup.exe --doctor (offline checks), then offers Repair when it helps
#   V3X_LANG     en | pt                                  (default: from the Windows display language)
#   V3X_GAME_DIR the "Victoria 3" folder (or its binaries folder); skips the search
#   V3X_SOURCE   where to download from: a local folder or a base URL holding V3X-Setup.exe,
#                compat.json and compat.json.sig (default: the latest GitHub release);
#                V3X_VERSION=v0.5.0 = that tag. The signature check applies to every source.
#   V3X_PURGE=1  with uninstall: also delete binaries\v3x (logs, settings)
#   V3X_YES=1    never ask; take the default answer (for scripts)
#   V3X_ANSWERS  answers for the questions, separated by ';' (for tests)
#   V3X_STEAM_PATH  use this Steam folder instead of the registry (for tests)
# Exit code (only when run as a file, e.g. powershell -File install.ps1; after "irm | iex" it is in
# $v3xInstallRc): 0 ok, 1 error,
# 2 unsupported game (not forced), 3 game running, 4 game not found, 5 not installed,
# 8 release signature / checksum not verified, 10 stopped.
# This file is ASCII on purpose: "irm | iex" on PowerShell 5.1 may decode a download as Latin-1, so
# accented letters are written as \u00XX and decoded at run time.

$v3xInstallRc = & {
    Set-StrictMode -Version 2
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is many times slower with the progress bar

    $Repo = 'kleeedolinux/v3x'
    # Release signing keys (Ed25519 public keys, 64 hex digits; '' = slot unused). MUST equal
    # V3X_RELEASE_KEY_CURRENT / V3X_RELEASE_KEY_NEXT in installer/release_keys.h (test_installer.py checks).
    $ReleaseKeyCurrent = ''
    $ReleaseKeyNext = ''
    $OneLiner = 'irm https://v3extender.pages.dev/install.ps1 | iex'

    # ------------------------------------------------------------------ language
    $lang = 'en'
    $want = "$env:V3X_LANG"
    if (-not $want) {
        try { $want = (Get-UICulture).Name } catch { }
        if (-not $want) { try { $want = (Get-Culture).Name } catch { } }
    }
    if ("$want" -match '^pt') { $lang = 'pt' }

    $T = @{
        en = @{
            title        = 'V3X for Victoria 3 - easy installer'
            spNote       = 'V3X is for single player only. Do not use it in multiplayer.'
            closeFirst   = 'Close Victoria 3 first.'
            searching    = 'Looking for Victoria 3...'
            found        = 'Found your game at:'
            isRight      = 'Is this right? [Y/n]'
            notFound     = 'I could not find Victoria 3 by myself.'
            howFind      = 'In Steam: right-click Victoria 3 > Manage > Browse local files. That window shows the game folder.'
            askFolder    = 'Paste the game folder here (or just press Enter to choose it in a window)'
            askFolderNP  = 'Paste the game folder here (or type q to stop)'
            pickTitle    = 'Choose your "Victoria 3" game folder (the one with the "binaries" folder inside).'
            badFolder    = 'That is not the Victoria 3 folder (there is no binaries\victoria3.exe inside). Try again.'
            gaveUp       = 'No game folder chosen. Nothing was changed.'
            usingDir     = 'Game folder:'
            running      = 'Victoria 3 is open right now. Close the game, then press Enter. (Type q to stop.)'
            stillRunning = 'Victoria 3 is still open. Nothing was changed. Close the game and run this again.'
            installed    = 'V3X is already installed here. What do you want to do?'
            menu1        = '  1  Update or repair V3X'
            menu2        = '  2  Uninstall V3X'
            menu3        = '  3  Stop, change nothing'
            menu4        = '  4  Check my install (finds missing files, conflicts, problems)'
            choose       = 'Type 1, 2, 3 or 4 [1]'
            checking2    = 'Checking your install...'
            drOK         = 'OK'
            drWARN       = 'WARNING'
            drPROBLEM    = 'PROBLEM'
            drTodo       = '    What to do: '
            drNone       = 'No problems found.'
            drSome       = '{0} problem(s), {1} warning(s).'
            askRepair    = 'Repair now? (copies missing or damaged V3X files back) [Y/n]'
            downloading  = 'Downloading V3X...'
            checking     = 'Checking the download...'
            dlFail       = 'The download failed: {0}'
            dlFail2      = 'Check your internet connection and try again. Nothing was changed.'
            hashBad      = 'The download is damaged (the checksum does not match). Nothing was changed. Try again later.'
            hashMissing  = 'compat.json does not list V3X-Setup.exe. Nothing was changed.'
            sigBad       = 'This download is NOT signed by the V3X developers ({0}). Nothing was run and nothing was changed.'
            sigBad2      = 'Do not run files from this download. Get V3X only from https://github.com/kleeedolinux/v3x/releases'
            sigOk        = 'Signature OK (V3X release key {0}).'
            verifyFail   = 'V3X-Setup.exe could not confirm the release ({0}). Nothing was changed.'
            installing   = 'Installing V3X...'
            uninstalling = 'Uninstalling V3X...'
            okInstall    = 'Done! V3X {0} is installed.'
            next1        = 'Start Victoria 3 as usual (Steam or the Paradox launcher).'
            next2        = 'Most V3X features are installed but OFF. To turn them on: main menu > V3X button.'
            next3        = 'A small "V3X console" window opens next to the game. That is normal.'
            howUninst    = 'To uninstall later: run the same command again and choose Uninstall:'
            okUninstall  = 'Done! V3X is uninstalled. The game starts without V3X now.'
            keptFiles    = 'Your V3X logs and changed settings stay in binaries\v3x. You can delete that folder.'
            askPurge     = 'Also delete V3X logs and settings? [y/N]'
            unsupported  = 'Your game version is not the one this V3X was made for (Victoria 3 1.15.1).'
            unsupported2 = 'It may still work, but nobody tested it. Waiting for a V3X update is the safe choice.'
            askForce     = 'Install anyway? [y/N]'
            notForced    = 'OK, nothing was changed.'
            notInst      = 'V3X was not installed here by the V3X installer. Nothing was changed.'
            notInst2     = 'To remove a manual install: delete binaries\version.dll and the binaries\v3x folder.'
            setupFail    = 'Something went wrong (code {0}). What the installer said:'
            setupFail2   = 'Close the game and run this again. If it keeps failing, send this text to the V3X developers.'
            cantRun      = 'Could not start V3X-Setup.exe: {0}'
            cantRun2     = 'Your antivirus may have blocked it.'
            stopped      = 'Stopped. Nothing was changed.'
            error        = 'Error: {0}'
            yes          = '^(y|yes|s|sim)$'
            no           = '^(n|no|nao|n\u00e3o)$'
        }
        pt = @{
            title        = 'V3X para Victoria 3 - instalador f\u00e1cil'
            spNote       = 'O V3X \u00e9 s\u00f3 para um jogador. N\u00e3o use no multijogador.'
            closeFirst   = 'Feche o Victoria 3 primeiro.'
            searching    = 'Procurando o Victoria 3...'
            found        = 'Achei seu jogo em:'
            isRight      = 'Est\u00e1 certo? [S/n]'
            notFound     = 'N\u00e3o consegui achar o Victoria 3 sozinho.'
            howFind      = 'Na Steam: clique com o bot\u00e3o direito em Victoria 3 > Gerenciar > Ver arquivos locais. Essa janela mostra a pasta do jogo.'
            askFolder    = 'Cole aqui a pasta do jogo (ou s\u00f3 aperte Enter para escolher numa janela)'
            askFolderNP  = 'Cole aqui a pasta do jogo (ou digite q para parar)'
            pickTitle    = 'Escolha a pasta do jogo "Victoria 3" (a que tem a pasta "binaries" dentro).'
            badFolder    = 'Essa n\u00e3o \u00e9 a pasta do Victoria 3 (n\u00e3o tem binaries\victoria3.exe dentro). Tente de novo.'
            gaveUp       = 'Nenhuma pasta escolhida. Nada foi alterado.'
            usingDir     = 'Pasta do jogo:'
            running      = 'O Victoria 3 est\u00e1 aberto agora. Feche o jogo e aperte Enter. (Digite q para parar.)'
            stillRunning = 'O Victoria 3 ainda est\u00e1 aberto. Nada foi alterado. Feche o jogo e rode de novo.'
            installed    = 'O V3X j\u00e1 est\u00e1 instalado aqui. O que voc\u00ea quer fazer?'
            menu1        = '  1  Atualizar ou reparar o V3X'
            menu2        = '  2  Desinstalar o V3X'
            menu3        = '  3  Parar, n\u00e3o mudar nada'
            menu4        = '  4  Verificar instala\u00e7\u00e3o (acha arquivos faltando, conflitos, problemas)'
            choose       = 'Digite 1, 2, 3 ou 4 [1]'
            checking2    = 'Verificando sua instala\u00e7\u00e3o...'
            drOK         = 'OK'
            drWARN       = 'AVISO'
            drPROBLEM    = 'PROBLEMA'
            drTodo       = '    O que fazer: '
            drNone       = 'Nenhum problema encontrado.'
            drSome       = '{0} problema(s), {1} aviso(s).'
            askRepair    = 'Reparar agora? (copia de volta os arquivos do V3X que faltam ou est\u00e3o danificados) [S/n]'
            downloading  = 'Baixando o V3X...'
            checking     = 'Conferindo o download...'
            dlFail       = 'O download falhou: {0}'
            dlFail2      = 'Confira sua internet e tente de novo. Nada foi alterado.'
            hashBad      = 'O download veio com defeito (o checksum n\u00e3o bate). Nada foi alterado. Tente mais tarde.'
            hashMissing  = 'O compat.json n\u00e3o lista o V3X-Setup.exe. Nada foi alterado.'
            sigBad       = 'Este download N\u00c3O tem a assinatura dos desenvolvedores do V3X ({0}). Nada foi executado nem alterado.'
            sigBad2      = 'N\u00e3o rode arquivos deste download. Pegue o V3X s\u00f3 em https://github.com/kleeedolinux/v3x/releases'
            sigOk        = 'Assinatura OK (chave de release do V3X {0}).'
            verifyFail   = 'O V3X-Setup.exe n\u00e3o confirmou o release ({0}). Nada foi alterado.'
            installing   = 'Instalando o V3X...'
            uninstalling = 'Desinstalando o V3X...'
            okInstall    = 'Pronto! O V3X {0} est\u00e1 instalado.'
            next1        = 'Abra o Victoria 3 como sempre (Steam ou launcher da Paradox).'
            next2        = 'A maioria das fun\u00e7\u00f5es do V3X vem instalada, mas DESLIGADA. Para ligar: menu principal > bot\u00e3o V3X.'
            next3        = 'Uma janelinha "V3X console" abre ao lado do jogo. Isso \u00e9 normal.'
            howUninst    = 'Para desinstalar depois: rode o mesmo comando de novo e escolha Desinstalar:'
            okUninstall  = 'Pronto! O V3X foi desinstalado. Agora o jogo abre sem o V3X.'
            keptFiles    = 'Seus logs e configura\u00e7\u00f5es alteradas do V3X ficam em binaries\v3x. Pode apagar essa pasta.'
            askPurge     = 'Apagar tamb\u00e9m os logs e configura\u00e7\u00f5es do V3X? [s/N]'
            unsupported  = 'Sua vers\u00e3o do jogo n\u00e3o \u00e9 a que este V3X foi feito para usar (Victoria 3 1.15.1).'
            unsupported2 = 'Pode at\u00e9 funcionar, mas ningu\u00e9m testou. Esperar uma atualiza\u00e7\u00e3o do V3X \u00e9 o mais seguro.'
            askForce     = 'Instalar mesmo assim? [s/N]'
            notForced    = 'Certo, nada foi alterado.'
            notInst      = 'O V3X n\u00e3o foi instalado aqui pelo instalador do V3X. Nada foi alterado.'
            notInst2     = 'Para remover uma instala\u00e7\u00e3o manual: apague binaries\version.dll e a pasta binaries\v3x.'
            setupFail    = 'Algo deu errado (c\u00f3digo {0}). O que o instalador disse:'
            setupFail2   = 'Feche o jogo e rode de novo. Se continuar falhando, mande este texto para quem faz o V3X.'
            cantRun      = 'N\u00e3o consegui abrir o V3X-Setup.exe: {0}'
            cantRun2     = 'Talvez o antiv\u00edrus tenha bloqueado.'
            stopped      = 'Parado. Nada foi alterado.'
            error        = 'Erro: {0}'
            yes          = '^(s|sim|y|yes)$'
            no           = '^(n|nao|n\u00e3o|no)$'
        }
    }

    # pt-BR text per doctor check id: title, what to do. English uses V3X-Setup's own lines.
    $DrPt = @{
        D001 = @('Vers\u00e3o do jogo', 'Espere uma atualiza\u00e7\u00e3o do V3X para esta vers\u00e3o do jogo, ou desinstale o V3X at\u00e9 l\u00e1.')
        D002 = @('Arquivos do V3X', 'Escolha Reparar: ele copia de volta os arquivos que faltam ou est\u00e3o danificados.')
        D003 = @('Outros carregadores de mods', 'Se o jogo travar, tire esses arquivos da pasta binaries e teste de novo. Um version.dll de outra ferramenta: escolha Reparar.')
        D009 = @('Mem\u00f3ria do Windows (arquivo de pagina\u00e7\u00e3o)', 'Deixe o Windows gerenciar o arquivo de pagina\u00e7\u00e3o (Sistema > Configura\u00e7\u00f5es avan\u00e7adas > Desempenho > Mem\u00f3ria virtual).')
        D010 = @('Espa\u00e7o livre em disco', 'Libere espa\u00e7o (pelo menos 5 GB).')
        D012 = @('\u00daltima abertura do jogo', 'Desligue esse plugin (menu principal > V3X, ou <plugin>=0 no v3x.ini) e mande o binaries\v3x\v3x.log para quem faz o V3X.')
        D013 = @('\u00daltimo travamento', 'Mande o binaries\v3x\crash.log para quem faz o V3X. Se o m\u00f3dulo for um plugin do V3X, desligue esse plugin.')
        D014 = @('Arquivo v3x.ini', 'Escolha Reparar, ou corrija o binaries\v3x\v3x.ini em [plugins] (uma linha por plugin, como keep_history=1).')
        D015 = @('Jogo aberto', 'Feche o jogo antes de Reparar, Atualizar ou Desinstalar.')
    }

    function Dec([string]$s) {
        [regex]::Replace($s, '\\u([0-9a-fA-F]{4})', { param($m) [string][char][Convert]::ToInt32($m.Groups[1].Value, 16) })
    }
    function S([string]$key) {
        $s = Dec $T[$lang][$key]
        if ($args.Count) { $s = $s -f $args }
        $s
    }
    function Say([string]$text, [string]$color = '') {
        if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
    }

    # ------------------------------------------------------------------ questions
    $ctx = @{ answers = $null; scripted = ($env:V3X_YES -eq '1') }
    if ($null -ne $env:V3X_ANSWERS) { $ctx.answers = New-Object System.Collections.Queue; foreach ($a in ("$env:V3X_ANSWERS" -split ';')) { $ctx.answers.Enqueue($a) }; $ctx.scripted = $true }

    function Ask([string]$prompt) {
        if ($null -ne $ctx.answers) {
            $a = ''
            if ($ctx.answers.Count) { $a = [string]$ctx.answers.Dequeue() }
            Write-Host ("{0}: {1}" -f $prompt, $a)
            return $a.Trim()
        }
        if ($ctx.scripted) { Write-Host ("{0}: " -f $prompt); return '' }
        return ([string](Read-Host -Prompt $prompt)).Trim()
    }
    function AskYes([string]$key, [bool]$default) {
        $a = Ask (S $key)
        if (-not $a) { return $default }
        if ($a -match (S 'yes')) { return $true }
        if ($a -match (S 'no')) { return $false }
        return $default
    }

    # ------------------------------------------------------------------ finding the game
    function Resolve-GameDir([string]$p) {
        if (-not $p) { return $null }
        $p = $p.Trim().Trim('"').Trim("'").Trim()
        if (-not $p) { return $null }
        try { $p = [IO.Path]::GetFullPath($p) } catch { return $null }
        if ($p.Length -gt 3) { $p = $p.TrimEnd('\', '/') }
        if ($p -match '[\\/]victoria3\.exe$') { $p = Split-Path -Path $p -Parent }
        if (Test-Path -LiteralPath (Join-Path $p 'binaries\victoria3.exe') -PathType Leaf) { return $p }
        if ((Split-Path -Path $p -Leaf) -ieq 'binaries') {
            $q = Split-Path -Path $p -Parent
            if ($q -and (Test-Path -LiteralPath (Join-Path $q 'binaries\victoria3.exe') -PathType Leaf)) { return $q }
        }
        return $null
    }

    function Get-SteamRoots {
        $roots = @()
        if ($env:V3X_STEAM_PATH) { return @($env:V3X_STEAM_PATH) }
        foreach ($k in @(@('HKCU:\Software\Valve\Steam', 'SteamPath'),
                         @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath'),
                         @('HKLM:\SOFTWARE\Valve\Steam', 'InstallPath'))) {
            try {
                $v = (Get-ItemProperty -LiteralPath $k[0] -Name $k[1] -ErrorAction Stop).($k[1])
                if ($v) { $roots += ($v -replace '/', '\') }
            } catch { }
        }
        return $roots
    }

    # Steam: every library in steamapps\libraryfolders.vdf; the one with appmanifest_529340.acf
    # (Victoria 3) first, using its "installdir"; then any library with common\Victoria 3.
    function Find-Game {
        $libs = New-Object System.Collections.ArrayList
        foreach ($r in Get-SteamRoots) {
            if ($libs -notcontains $r) { [void]$libs.Add($r) }
            $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf -PathType Leaf) {
                $txt = [IO.File]::ReadAllText($vdf)
                foreach ($m in [regex]::Matches($txt, '"path"\s+"((?:[^"\\]|\\.)*)"')) {
                    $lib = $m.Groups[1].Value -replace '\\\\', '\'
                    if ($libs -notcontains $lib) { [void]$libs.Add($lib) }
                }
            }
        }
        foreach ($lib in $libs) {
            $acf = Join-Path $lib 'steamapps\appmanifest_529340.acf'
            if (Test-Path -LiteralPath $acf -PathType Leaf) {
                $dir = 'Victoria 3'
                $m = [regex]::Match([IO.File]::ReadAllText($acf), '"installdir"\s+"([^"]+)"')
                if ($m.Success) { $dir = $m.Groups[1].Value }
                $g = Resolve-GameDir (Join-Path $lib ('steamapps\common\' + $dir))
                if ($g) { return $g }
            }
        }
        foreach ($lib in $libs) {
            $g = Resolve-GameDir (Join-Path $lib 'steamapps\common\Victoria 3')
            if ($g) { return $g }
        }
        return $null
    }

    function Pick-Folder {
        if ($ctx.scripted) { return $null }
        try {
            if ([Threading.Thread]::CurrentThread.GetApartmentState() -ne 'STA') { return $null }
            Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
            $d = New-Object System.Windows.Forms.FolderBrowserDialog
            $d.Description = S 'pickTitle'
            $d.ShowNewFolderButton = $false
            $owner = New-Object System.Windows.Forms.Form
            $owner.TopMost = $true
            $r = $d.ShowDialog($owner)
            $owner.Dispose()
            if ("$r" -eq 'OK') { return $d.SelectedPath }
        } catch { }
        return $null
    }

    function Ask-GameDir {
        Say (S 'howFind')
        $picker = -not $ctx.scripted
        for ($i = 0; $i -lt 5; $i++) {
            $a = Ask (S $(if ($picker) { 'askFolder' } else { 'askFolderNP' }))
            if ($a -eq 'q' -or $a -eq 'Q') { return $null }
            if (-not $a) {
                if (-not $picker) { if ($ctx.scripted) { return $null } else { continue } }
                $a = Pick-Folder
                if (-not $a) { $picker = $false; continue }
            }
            $g = Resolve-GameDir $a
            if ($g) { return $g }
            Say (S 'badFolder') 'Yellow'
        }
        return $null
    }

    function Test-GameRunning([string]$game) {
        $exe = [IO.Path]::GetFullPath((Join-Path $game 'binaries\victoria3.exe'))
        foreach ($p in @(Get-Process -Name 'victoria3' -ErrorAction SilentlyContinue)) {
            $path = $null
            try { $path = $p.Path } catch { }
            if (-not $path) { return $true }   # cannot read its path: assume it is this one (safe side)
            if ([string]::Equals($path, $exe, [StringComparison]::OrdinalIgnoreCase)) { return $true }
        }
        return $false
    }

    # ------------------------------------------------------------------ release signature (Ed25519, RFC 8032)
    # PowerShell 5.1 has no Ed25519, so this is a small verifier over System.Numerics.BigInteger.
    # It only ever handles public data (public key, compat.json, its signature): no secret, so
    # constant time does not matter. Same rules as installer/release_sig.h: S < L, the signature's
    # R must equal the canonical encoding of [S]B - [k]A, k = SHA-512(R || A || message) mod L.
    function Test-Ed25519([byte[]]$pub, [byte[]]$msg, [byte[]]$sig) {
        if ($null -eq $pub -or $null -eq $sig -or $pub.Length -ne 32 -or $sig.Length -ne 64) { return $false }
        try { Add-Type -AssemblyName System.Numerics -ErrorAction SilentlyContinue } catch { }
        $B = [System.Numerics.BigInteger]
        $p = $B::Subtract($B::Pow(2, 255), 19)
        $L = $B::Add($B::Pow(2, 252), $B::Parse('27742317777372353535851937790883648493'))
        $d = $B::Remainder($B::Multiply($B::Subtract($p, 121665), $B::ModPow(121666, $B::Subtract($p, 2), $p)), $p)
        $d2 = $B::Remainder($B::Multiply($d, 2), $p)
        $sqrtm1 = $B::ModPow(2, $B::Divide($B::Subtract($p, 1), 4), $p)
        $zero = $B::Zero; $one = $B::One

        function EdFromLE([byte[]]$b) { $x = New-Object byte[] ($b.Length + 1); [Array]::Copy($b, $x, $b.Length); New-Object System.Numerics.BigInteger (, [byte[]]$x) }
        function EdMul($a, $b) { $B::Remainder($B::Multiply($a, $b), $p) }
        function EdAdd($a, $b) { $B::Remainder($B::Add($a, $b), $p) }
        function EdSub($a, $b) { $B::Remainder($B::Add($B::Subtract($a, $b), $p), $p) }
        function EdPAdd($P1, $Q) {
            $A1 = EdMul (EdSub $P1[1] $P1[0]) (EdSub $Q[1] $Q[0])
            $B1 = EdMul (EdAdd $P1[1] $P1[0]) (EdAdd $Q[1] $Q[0])
            $C1 = EdMul (EdMul $P1[3] $Q[3]) $d2
            $D1 = EdMul $P1[2] (EdAdd $Q[2] $Q[2])
            $E = EdSub $B1 $A1; $F = EdSub $D1 $C1; $G = EdAdd $D1 $C1; $H = EdAdd $B1 $A1
            , @((EdMul $E $F), (EdMul $G $H), (EdMul $F $G), (EdMul $E $H))
        }
        function EdEncode($P1) {
            $zi = $B::ModPow($P1[2], $B::Subtract($p, 2), $p)
            $x = EdMul $P1[0] $zi; $y = EdMul $P1[1] $zi
            $raw = $y.ToByteArray(); $out = New-Object byte[] 32
            [Array]::Copy($raw, $out, [Math]::Min(32, $raw.Length))
            if (-not $x.IsEven) { $out[31] = $out[31] -bor 0x80 }
            , $out
        }
        # decompress the public key A (RFC 8032 5.1.3)
        $yb = [byte[]]$pub.Clone(); $sign = ($yb[31] -shr 7) -band 1; $yb[31] = $yb[31] -band 0x7f
        $y = EdFromLE $yb
        if ($y.CompareTo($p) -ge 0) { return $false }
        $yy = EdMul $y $y
        $x2 = EdMul (EdSub $yy $one) ($B::ModPow((EdAdd (EdMul $d $yy) $one), $B::Subtract($p, 2), $p))
        $x = $B::ModPow($x2, $B::Divide($B::Add($p, 3), 8), $p)
        if (-not (EdSub (EdMul $x $x) $x2).IsZero) { $x = EdMul $x $sqrtm1 }
        if (-not (EdSub (EdMul $x $x) $x2).IsZero) { return $false }
        if ($x.IsZero -and $sign -eq 1) { return $false }
        if ([int](-not $x.IsEven) -ne $sign) { $x = EdSub $p $x }
        $negA = @((EdSub $zero $x), $y, $one, (EdSub $zero (EdMul $x $y)))
        $s = EdFromLE ([byte[]]$sig[32..63])
        if ($s.CompareTo($L) -ge 0) { return $false }
        $sha = [Security.Cryptography.SHA512]::Create()
        $h = $sha.ComputeHash([byte[]]($sig[0..31] + $pub + $msg))
        $k = $B::Remainder((EdFromLE $h), $L)
        $gy = EdMul 4 ($B::ModPow(5, $B::Subtract($p, 2), $p))
        $gx = $B::Parse('15112221349535400772501151409588531511454012693041857206046113283949847762202')
        $G0 = @($gx, $gy, $one, (EdMul $gx $gy))
        # Straus: [s]B + [k](-A) with one shared doubling chain
        $sb = $s.ToByteArray(); $kb = $k.ToByteArray()
        $tab = @($null, $G0, $negA, (EdPAdd $G0 $negA))
        $Q = @($zero, $one, $one, $zero)
        for ($i = 255; $i -ge 0; $i--) {
            $Q = EdPAdd $Q $Q
            $j = $i -shr 3; $m = 1 -shl ($i -band 7)
            $bi = 0
            if ($j -lt $sb.Length -and ($sb[$j] -band $m)) { $bi += 1 }
            if ($j -lt $kb.Length -and ($kb[$j] -band $m)) { $bi += 2 }
            if ($bi) { $Q = EdPAdd $Q $tab[$bi] }
        }
        $enc = EdEncode $Q
        for ($i = 0; $i -lt 32; $i++) { if ($enc[$i] -ne $sig[$i]) { return $false } }
        return $true
    }

    # 'ok:<key id>' or 'fail:<reason>'; compat.json.sig = 'v3x-sig 1 <key id> <base64 signature>'
    function Test-ReleaseSignature([string]$compatPath, [string]$sigPath) {
        $keys = @(@($ReleaseKeyCurrent, $ReleaseKeyNext) | Where-Object { $_ -match '^[0-9a-f]{64}$' })
        if (-not $keys.Count) { return 'fail:this installer has no release key yet' }
        if (-not (Test-Path -LiteralPath $sigPath -PathType Leaf)) { return 'fail:the release has no compat.json.sig' }
        $line = [IO.File]::ReadAllText($sigPath)
        $m = [regex]::Match($line, '^v3x-sig 1 ([0-9a-f]{16}) ([A-Za-z0-9+/]{86}==)\r?\n?$')
        if (-not $m.Success) { return 'fail:malformed compat.json.sig' }
        $sig = [Convert]::FromBase64String($m.Groups[2].Value)
        $msg = [IO.File]::ReadAllBytes($compatPath)
        $sha = [Security.Cryptography.SHA512]::Create()
        foreach ($k in $keys) {
            $pub = [byte[]]( ($k -split '(..)' | Where-Object { $_ }) | ForEach-Object { [Convert]::ToByte($_, 16) } )
            $kid = -join ($sha.ComputeHash($pub)[0..7] | ForEach-Object { $_.ToString('x2') })
            if ($kid -ne $m.Groups[1].Value) { continue }
            if (Test-Ed25519 $pub $msg $sig) { return ('ok:' + $kid) }
            return 'fail:BAD signature'
        }
        return ('fail:signed with an unknown key ' + $m.Groups[1].Value)
    }

    # ------------------------------------------------------------------ download + run
    function Get-ReleaseFile([string]$name, [string]$dest) {
        $src = "$env:V3X_SOURCE"
        if ($src -and (Test-Path -LiteralPath $src -PathType Container)) {
            Copy-Item -LiteralPath (Join-Path $src $name) -Destination $dest -Force
            return
        }
        if (-not $src) {
            if ($env:V3X_VERSION) { $src = "https://github.com/$Repo/releases/download/$env:V3X_VERSION" }
            else { $src = "https://github.com/$Repo/releases/latest/download" }
        }
        Invoke-WebRequest -Uri ($src.TrimEnd('/') + '/' + $name) -OutFile $dest -UseBasicParsing
    }

    function Invoke-Setup([string]$exe, [string]$argline) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = $argline
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
        $p = [Diagnostics.Process]::Start($psi)
        $out = $p.StandardOutput.ReadToEnd()
        $p.WaitForExit()
        return @{ rc = $p.ExitCode; out = $out }
    }

    # V3X-Setup --doctor output -> plain lines; returns @{ problems; warnings; repair }
    function Show-Doctor([string]$out) {
        $res = @{ problems = 0; warnings = 0; repair = $false }
        $lines = @($out -split "`r?`n")
        if ($lang -eq 'en') {
            foreach ($l in $lines) {
                if ($l -match '^(OK|WARN|PROBLEM)\s+(.*?)(\s+What to do: (.*))?$') {
                    $w = $Matches[1]; $c = @{ OK = 'Green'; WARN = 'Yellow'; PROBLEM = 'Red' }[$w]
                    Say ('[' + (S ('dr' + $w)) + '] ' + $Matches[2]) $c
                    if ($Matches[4]) { Say ((S 'drTodo') + $Matches[4]) }
                }
            }
        }
        foreach ($l in $lines) {
            if ($l -match '^doctor: (D\d+) (\S+) (OK|WARN|PROBLEM) ?(.*)$') {
                $id = $Matches[1]; $w = $Matches[3]; $det = $Matches[4]
                if ($w -eq 'PROBLEM') { $res.problems++ } elseif ($w -eq 'WARN') { $res.warnings++ }
                if ($lang -ne 'en') {
                    $c = @{ OK = 'Green'; WARN = 'Yellow'; PROBLEM = 'Red' }[$w]
                    $title = $id; $todo = ''
                    if ($DrPt.ContainsKey($id)) { $title = Dec $DrPt[$id][0]; $todo = Dec $DrPt[$id][1] }
                    Say ('[' + (S ('dr' + $w)) + '] ' + $title + ': ' + $det) $c
                    if ($w -ne 'OK' -and $todo) { Say ((S 'drTodo') + $todo) }
                }
            } elseif ($l -match '^doctor: result .*repair=1') { $res.repair = $true }
        }
        Say ''
        if ($res.problems -or $res.warnings) { Say (S 'drSome' $res.problems $res.warnings) $(if ($res.problems) { 'Red' } else { 'Yellow' }) }
        else { Say (S 'drNone') 'Green' }
        return $res
    }

    function Show-Details([string]$out) {
        foreach ($l in ($out -split "`r?`n")) { if ($l.Trim()) { Say ('    ' + $l) 'DarkGray' } }
    }

    # ------------------------------------------------------------------ main
    function Main {
        try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
        Say ''
        Say (S 'title') 'Cyan'
        Say (S 'spNote')
        Say ''

        $action = ("$env:V3X_ACTION").Trim().ToLowerInvariant()
        if ($action -and $action -notin @('install', 'repair', 'uninstall', 'status', 'doctor')) { Say (S 'error' "V3X_ACTION=$action (install, repair, uninstall, status, doctor)") 'Red'; return 1 }

        # 1. the game folder
        $game = $null
        if ($env:V3X_GAME_DIR) {
            $game = Resolve-GameDir $env:V3X_GAME_DIR
            if (-not $game) { Say (S 'badFolder') 'Yellow' }
            else { Say (S 'usingDir'); Say ('    ' + $game) 'Green' }
        } else {
            Say (S 'searching')
            $game = Find-Game
            if ($game) {
                Say (S 'found'); Say ('    ' + $game) 'Green'
                if (-not (AskYes 'isRight' $true)) { $game = $null }
            } else {
                Say (S 'notFound') 'Yellow'
            }
        }
        if (-not $game) { $game = Ask-GameDir }
        if (-not $game) { Say (S 'gaveUp') 'Yellow'; return 4 }

        $bin = Join-Path $game 'binaries'
        $isInstalled = Test-Path -LiteralPath (Join-Path $bin 'v3x\install_manifest.txt') -PathType Leaf

        # 2. the game must be closed
        if ($action -ne 'status' -and $action -ne 'doctor') {
            $tries = 0
            while (Test-GameRunning $game) {
                $tries++
                if ($ctx.scripted -and -not ($ctx.answers -and $ctx.answers.Count)) { Say (S 'stillRunning') 'Yellow'; return 3 }
                if ($tries -gt 5) { Say (S 'stillRunning') 'Yellow'; return 3 }
                Say ''
                $a = Ask (S 'running')
                if ($a -eq 'q' -or $a -eq 'Q') { Say (S 'stopped'); return 10 }
            }
        }

        # 3. what to do
        if (-not $action) {
            $action = 'install'
            if ($isInstalled) {
                Say ''
                Say (S 'installed')
                Say (S 'menu1'); Say (S 'menu2'); Say (S 'menu3'); Say (S 'menu4')
                $c = Ask (S 'choose')
                if ($c -eq '2') { $action = 'uninstall' }
                elseif ($c -eq '4') { $action = 'doctor' }
                elseif ($c -eq '3' -or $c -eq 'q') { Say (S 'stopped'); return 10 }
                else { $action = 'repair' }
            }
        }
        $purge = $false
        if ($action -eq 'uninstall') {
            if ($env:V3X_PURGE -eq '1') { $purge = $true } else { $purge = AskYes 'askPurge' $false }
        }

        # 4. download and check
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ('v3x-install-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Item -ItemType Directory -Path $tmp -Force | Out-Null
        try {
            Say ''
            Say (S 'downloading')
            $exe = Join-Path $tmp 'V3X-Setup.exe'
            $compat = Join-Path $tmp 'compat.json'
            $csig = Join-Path $tmp 'compat.json.sig'
            try {
                Get-ReleaseFile 'compat.json' $compat
                Get-ReleaseFile 'V3X-Setup.exe' $exe
            } catch {
                Say (S 'dlFail' $_.Exception.Message) 'Red'; Say (S 'dlFail2'); return 1
            }
            try { Get-ReleaseFile 'compat.json.sig' $csig } catch { }   # missing = unsigned, refused below
            Say (S 'checking')
            # (1) signature over compat.json, verified here (H1)
            $sv = Test-ReleaseSignature $compat $csig
            if (-not $sv.StartsWith('ok:')) { Say (S 'sigBad' $sv.Substring(5)) 'Red'; Say (S 'sigBad2'); return 8 }
            Say (S 'sigOk' $sv.Substring(3)) 'Green'
            # (2) the setup must be the one the signed compat.json names (size + SHA-256)
            $cj = $null
            try { $cj = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($compat)) | ConvertFrom-Json } catch { }
            $entry = $null
            if ($cj -and ($cj.PSObject.Properties.Name -contains 'assets')) { $entry = @($cj.assets | Where-Object { $_.name -eq 'V3X-Setup.exe' }) | Select-Object -First 1 }
            if (-not $entry -or -not ("$($entry.sha256)" -match '^[0-9a-f]{64}$')) { Say (S 'hashMissing') 'Red'; return 8 }
            $got = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($got -ne "$($entry.sha256)" -or (Get-Item -LiteralPath $exe).Length -ne [long]$entry.size) { Say (S 'hashBad') 'Red'; return 8 }
            # (3) the (now trusted) setup re-checks the whole folder with its embedded keys
            try { $vr = Invoke-Setup $exe ('--verify-release "' + $tmp + '"') } catch { Say (S 'cantRun' $_.Exception.Message) 'Red'; Say (S 'cantRun2'); return 1 }
            if ($vr.rc -ne 0) {
                $why = ($vr.out -split "`r?`n" | Where-Object { $_ -match 'FAILED' } | Select-Object -First 1)
                Say (S 'verifyFail' "$why") 'Red'; return 8
            }

            # 5. run the tested installer
            $gd = '--game-dir "' + $game + '"'
            if ($action -eq 'status') { $argline = '--status ' + $gd }
            elseif ($action -eq 'doctor') { $argline = '--doctor ' + $gd }
            elseif ($action -eq 'uninstall') { $argline = '--silent --uninstall ' + $gd; if ($purge) { $argline += ' --purge' } }
            elseif ($action -eq 'repair') { $argline = '--silent --repair ' + $gd }
            else { $argline = '--silent ' + $gd }
            Say $(if ($action -eq 'uninstall') { S 'uninstalling' } elseif ($action -eq 'doctor') { S 'checking2' } else { S 'installing' })
            try { $r = Invoke-Setup $exe $argline } catch { Say (S 'cantRun' $_.Exception.Message) 'Red'; Say (S 'cantRun2'); return 1 }

            if ($action -eq 'status') { Show-Details $r.out; return $r.rc }
            if ($action -eq 'doctor') {
                Say ''
                $d = Show-Doctor $r.out
                if (-not $d.repair) { return $r.rc }
                Say ''
                if (-not (AskYes 'askRepair' $true)) { return $r.rc }
                if (Test-GameRunning $game) { Say (S 'stillRunning') 'Yellow'; return 3 }
                $action = 'repair'
                $argline = '--silent --repair ' + $gd
                Say (S 'installing')
                $r = Invoke-Setup $exe $argline
            }
            if ($r.rc -eq 2) {
                Say ''
                Say (S 'unsupported') 'Yellow'
                Say (S 'unsupported2')
                if (-not (AskYes 'askForce' $false)) { Say (S 'notForced'); return 2 }
                Say (S 'installing')
                $r = Invoke-Setup $exe ($argline + ' --force')
            }
            Say ''
            switch ($r.rc) {
                0 {
                    if ($action -eq 'uninstall') {
                        Say (S 'okUninstall') 'Green'
                        if (-not $purge -and (Test-Path -LiteralPath (Join-Path $bin 'v3x'))) { Say (S 'keptFiles') }
                    } else {
                        $ver = '?'
                        $m = [regex]::Match($r.out, 'V3X (\S+) is installed')
                        if ($m.Success) { $ver = $m.Groups[1].Value }
                        Say (S 'okInstall' $ver) 'Green'
                        Say ('  1. ' + (S 'next1'))
                        Say ('  2. ' + (S 'next2'))
                        Say ('  3. ' + (S 'next3'))
                        Say ''
                        Say (S 'howUninst')
                        Say ('    ' + $OneLiner) 'Cyan'
                    }
                }
                3 { Say (S 'stillRunning') 'Yellow' }
                5 { Say (S 'notInst') 'Yellow'; Say (S 'notInst2') }
                default { Say (S 'setupFail' $r.rc) 'Red'; Show-Details $r.out; Say (S 'setupFail2') }
            }
            return $r.rc
        } finally {
            Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    try { Main } catch { Say (S 'error' $_.Exception.Message) 'Red'; 1 }
}
# "irm | iex" must never close the player's window, so only a script run as a file sets the exit code
if ($PSCommandPath -and $MyInvocation.InvocationName -ne '.') { exit ([int]($v3xInstallRc | Select-Object -Last 1)) }
