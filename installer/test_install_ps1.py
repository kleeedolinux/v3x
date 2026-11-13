"""Tests installer/install.ps1 (the one-line installer) against fake game folders and a local release.

    python installer/test_install_ps1.py [--dist build/dist] [--root D:/v3x_tmp/install_ps1] [--shell powershell,pwsh]
                                         [--real-exe <game>/binaries/victoria3.exe] [--keep]

The release folder (--dist, from make_dist.py) must hold install.ps1, V3X-Setup.exe,
SHA256SUMS.txt and compat.json; its files are checked as release assets. The install cases need a
SIGNED release that the tested install.ps1 trusts, so the test builds one: make_dist.py --trust-key
<public TEST key of tools/sign_release.py> into <root>/sdist, signs its compat.json with the test
key, and runs a copy of installer/install.ps1 whose $ReleaseKeyNext slot holds the test key (the
repo file itself never trusts it). Signature cases: tampered compat.json / signature / setup, no
signature, unknown key, and the unmodified install.ps1 (no release key yet) are all refused, rc=8.
Every case runs once per shell found (Windows PowerShell 5.1 = powershell, 7 = pwsh).
Questions are answered through V3X_ANSWERS, Steam is faked with V3X_STEAM_PATH (a folder with
steamapps/libraryfolders.vdf + appmanifest_529340.acf), downloads come from V3X_SOURCE (a folder,
or a local HTTP server for the real "irm <url> | iex" path). Never touches the real game.
Exit 0 = all passed.
"""
import argparse
import functools
import http.server
import os
import shutil
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import test_installer as ti  # noqa: E402  (make_game, synthetic_header, sha)

RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print("%s  %s%s" % ("ok  " if cond else "FAIL", name, ("  -- " + detail[-1500:]) if (detail and not cond) else ""))


class Handler(http.server.SimpleHTTPRequestHandler):
    ctype = "application/octet-stream"   # what GitHub release downloads send

    def guess_type(self, path):
        return self.ctype

    def log_message(self, *a):
        pass


def serve(directory, ctype):
    h = type("H", (Handler,), {"ctype": ctype})
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(h, directory=directory))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", default=os.path.join(REPO, "build", "dist"))
    ap.add_argument("--root", default="D:/v3x_tmp/install_ps1")
    ap.add_argument("--shell", default="powershell,pwsh")
    ap.add_argument("--real-exe", default=os.path.join(os.path.dirname(REPO), "binaries", "victoria3.exe"))
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    dist = os.path.abspath(a.dist)
    root = os.path.abspath(a.root)
    if os.path.commonpath([root.lower(), os.path.dirname(REPO).lower()]) == os.path.dirname(REPO).lower():
        sys.exit("refusing: --root must not be inside the game folder")
    for f in ("install.ps1", "V3X-Setup.exe", "SHA256SUMS.txt"):
        if not os.path.isfile(os.path.join(dist, f)):
            sys.exit("missing %s (run python installer/make_dist.py)" % os.path.join(dist, f))
    ps1_dist = os.path.join(dist, "install.ps1")
    check("dist install.ps1 is the repo's installer/install.ps1",
          open(ps1_dist, "rb").read() == open(os.path.join(HERE, "install.ps1"), "rb").read())
    check("install.ps1 is pure ASCII (irm | iex on PowerShell 5.1 may decode as Latin-1)",
          all(b < 128 for b in open(ps1_dist, "rb").read()))
    sums = open(os.path.join(dist, "SHA256SUMS.txt")).read()
    check("SHA256SUMS.txt lists V3X-Setup.exe, the zip, install.ps1 and compat.json with the right hashes",
          all(("%s  %s" % (ti.sha(os.path.join(dist, n)), n)) in sums
              for n in ["V3X-Setup.exe", "install.ps1", "compat.json"] + [f for f in os.listdir(dist) if f.startswith("v3x-") and f.endswith(".zip")])
          and any(f.startswith("v3x-") and f.endswith(".zip") for f in os.listdir(dist)), sums)

    # compat.json (in-game updater, format of mem chat #4420/#4425): builds from builds.txt, asset hashes
    import json
    cj = json.loads(open(os.path.join(dist, "compat.json"), "rb").read().decode("utf-8"))   # no BOM allowed
    builds = [l.split() for l in open(os.path.join(HERE, "builds.txt")) if l.strip() and not l.startswith("#")]
    check("compat.json: schema 1, api_version, game_builds = installer/builds.txt",
          cj.get("schema") == 1 and isinstance(cj.get("api_version"), int)
          and [(b["build_id"], b["branch"]) for b in cj["game_builds"]] == [(b[0], b[1]) for b in builds], str(cj))
    check("compat.json: every asset's size and sha256 match the file",
          {x["name"] for x in cj["assets"]} >= {"V3X-Setup.exe", "install.ps1"} and all(
              os.path.getsize(os.path.join(dist, x["name"])) == x["size"] and ti.sha(os.path.join(dist, x["name"])) == x["sha256"]
              for x in cj["assets"]), str(cj["assets"]))

    shells = [s for s in a.shell.split(",") if shutil.which(s)]
    check("at least one PowerShell found (%s)" % ",".join(shells), shells)
    if os.path.isfile(a.real_exe):
        header = open(a.real_exe, "rb").read(4096)
    else:
        print("note: using a synthetic victoria3.exe header")
        header = ti.synthetic_header()

    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    tmp = os.path.join(root, "tmp")
    os.makedirs(tmp)
    # sleeper that stands in for a running victoria3.exe
    sleeper = os.path.join(root, "sleeper.exe")
    src = os.path.join(root, "sleeper.c")
    open(src, "w").write("#include <windows.h>\nint main(void){Sleep(120000);return 0;}\n")
    have_sleeper = subprocess.call([sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", src, "-o", sleeper]) == 0
    # the signed test release (see the docstring) and the install.ps1 copy that trusts its key
    sd = ti.signed_dist(root)
    web = os.path.join(root, "web")
    shutil.copytree(sd, web)
    src_ps1 = open(os.path.join(HERE, "install.ps1"), "rb").read()
    patched = src_ps1.replace(b"$ReleaseKeyNext = ''", b"$ReleaseKeyNext = '" + ti.TEST_PUB.encode() + b"'")
    check("test copy of install.ps1 trusts the test key (one line changed)", patched != src_ps1 and patched.count(ti.TEST_PUB.encode()) == 1)
    open(os.path.join(web, "install.ps1"), "wb").write(patched)
    ps1 = os.path.join(web, "install.ps1")
    plain_ps1 = os.path.join(root, "plain_install.ps1")
    open(plain_ps1, "wb").write(src_ps1)

    def variant(name, fn):
        d = os.path.join(root, "rel_" + name)
        shutil.copytree(web, d)
        fn(d)
        return d

    def flip(path, off):
        bb = bytearray(open(path, "rb").read())
        bb[off] ^= 1
        open(path, "wb").write(bb)
    bad_releases = [
        ("tampered compat.json", variant("compat", lambda d: flip(os.path.join(d, "compat.json"), 30)), "NOT signed", "BAD signature"),
        ("tampered signature", variant("sig", lambda d: flip(os.path.join(d, "compat.json.sig"), 40)), "NOT signed", ""),
        ("no signature", variant("nosig", lambda d: os.remove(os.path.join(d, "compat.json.sig"))), "NOT signed", "no compat.json.sig"),
        ("unknown key", variant("unknown", lambda d: ti.sign_dir(d, seed=os.urandom(32))), "NOT signed", "unknown key"),
        ("tampered setup", variant("setup", lambda d: flip(os.path.join(d, "V3X-Setup.exe"), 0x50)), "damaged", ""),
        ("other setup", variant("other", lambda d: shutil.copy(os.path.join(dist, "V3X-Setup.exe"), d)), "damaged", ""),
    ]
    srv_bin = serve(web, "application/octet-stream")
    srv_txt = serve(web, "text/plain")

    def run(shell, env_extra, iex_url=None, script=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith("V3X_")}
        env.update({"TEMP": tmp, "TMP": tmp, "V3X_SOURCE": web, "V3X_LANG": "en"})
        env.update(env_extra)
        if iex_url:
            cmd = ("[Console]::OutputEncoding=[Text.Encoding]::UTF8; irm '%s' | iex; "
                   "Write-Output ('STILL-ALIVE rc=' + $v3xInstallRc)" % iex_url)
        else:
            cmd = "[Console]::OutputEncoding=[Text.Encoding]::UTF8; & '%s'; exit $LASTEXITCODE" % (script or ps1).replace("'", "''")
        p = subprocess.run([shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", cmd],
                           capture_output=True, timeout=180, env=env, stdin=subprocess.DEVNULL)
        return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")

    for sh in shells:
        base = os.path.join(root, sh)
        V = lambda g: os.path.join(g, "binaries", "version.dll")  # noqa: E731

        # 1. Steam detection: libraryfolders.vdf -> second library with appmanifest_529340.acf
        steam = os.path.join(base, "Steam")
        lib = os.path.join(base, "Lib Two")
        os.makedirs(os.path.join(steam, "steamapps"))
        g = ti.make_game(os.path.join(lib, "steamapps"), "common", header)   # <lib>/steamapps/common/Victoria 3
        open(os.path.join(steam, "steamapps", "libraryfolders.vdf"), "w").write(
            '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t\t"apps"\n\t\t{\n\t\t\t"529340"\t\t"1"\n\t\t}\n\t}\n}\n'
            % (steam.replace("\\", "\\\\"), lib.replace("\\", "\\\\")))
        open(os.path.join(lib, "steamapps", "appmanifest_529340.acf"), "w").write(
            '"AppState"\n{\n\t"appid"\t\t"529340"\n\t"installdir"\t\t"Victoria 3"\n}\n')
        rc, out = run(sh, {"V3X_STEAM_PATH": steam, "V3X_ANSWERS": "y"})
        check("[%s] steam: found via libraryfolders.vdf, confirmed, installed rc=0" % sh,
              rc == 0 and "Found your game at:" in out and g in out and "Done! V3X" in out and os.path.isfile(V(g)), out)
        check("[%s] steam: tells how to uninstall" % sh, "choose Uninstall" in out and "irm https://v3extender.pages.dev/install.ps1 | iex" in out, out)
        check("[%s] install: installed file equals the payload's version.dll" % sh,
              ti.sha(V(g)) == dict((r, h) for k, r, h, _ in ti.payload(os.path.join(sd, "V3X-Setup.exe")) if k == "bin")["version.dll"])
        check("[%s] install: temp download folder removed" % sh, not [d for d in os.listdir(tmp) if d.startswith("v3x-install-")])

        # 2. installed -> menu: 1 = repair
        rc, out = run(sh, {"V3X_STEAM_PATH": steam, "V3X_ANSWERS": "y;1"})
        check("[%s] menu: already installed -> 1 repairs rc=0" % sh, rc == 0 and "already installed" in out and "Done! V3X" in out, out)
        # 3. menu: 3 = stop
        rc, out = run(sh, {"V3X_STEAM_PATH": steam, "V3X_ANSWERS": "y;3"})
        check("[%s] menu: 3 stops, rc=10" % sh, rc == 10 and "Nothing was changed" in out, out)

        # 4. Portuguese
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "repair", "V3X_LANG": "pt"})
        check("[%s] pt-BR: text in Portuguese with accents decoded" % sh,
              rc == 0 and "Pronto! O V3X" in out and "est\u00e1 instalado" in out and "\\u00" not in out, out)

        # 4b. menu 4 = check my install (V3X-Setup --doctor): a deleted plugin is a PROBLEM, Repair offered and done
        os.remove(os.path.join(g, "binaries", "v3x", "plugins", "keep_history.dll"))
        rc, out = run(sh, {"V3X_STEAM_PATH": steam, "V3X_ANSWERS": "y;4;y"})
        check("[%s] menu 4: doctor shows a PROBLEM with 'What to do', offers Repair, 'y' repairs" % sh,
              "[PROBLEM]" in out and "What to do:" in out and "Repair now?" in out and "Done! V3X" in out
              and os.path.isfile(os.path.join(g, "binaries", "v3x", "plugins", "keep_history.dll")), out)
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "doctor", "V3X_YES": "1"})
        check("[%s] V3X_ACTION=doctor after repair: files OK, no Repair question" % sh,
              "[OK] All " in out and "Repair now?" not in out and ("problem(s)" in out or "No problems found." in out), out)
        os.remove(os.path.join(g, "binaries", "v3x", "plugins", "log_filter.dll"))
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "doctor", "V3X_LANG": "pt", "V3X_ANSWERS": "n"})
        check("[%s] doctor pt-BR: Portuguese titles + 'O que fazer', 'n' keeps the file missing" % sh,
              "[PROBLEMA] Arquivos do V3X" in out and "O que fazer:" in out and "Reparar agora?" in out
              and not os.path.exists(os.path.join(g, "binaries", "v3x", "plugins", "log_filter.dll")) and rc >= 1, out)
        run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "repair"})

        # 5. game running: refused (scripted), and 'q' stops
        if have_sleeper:
            exe = os.path.join(g, "binaries", "victoria3.exe")
            keep = open(exe, "rb").read()
            shutil.copy(sleeper, exe)
            proc = subprocess.Popen([exe])
            try:
                before = ti.sha(V(g))
                rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_YES": "1"})
                check("[%s] running: refused rc=3" % sh, rc == 3 and "still open" in out, out)
                rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ANSWERS": "q"})
                check("[%s] running: 'q' stops rc=10" % sh, rc == 10 and "open right now" in out, out)
                check("[%s] running: nothing changed" % sh, ti.sha(V(g)) == before)
            finally:
                proc.kill(); proc.wait()
            open(exe, "wb").write(keep)
        else:
            check("[%s] running: sleeper built" % sh, False)

        # 6. damaged download
        for what, d, want, why in bad_releases:
            rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_SOURCE": d, "V3X_ACTION": "uninstall", "V3X_YES": "1"})
            check("[%s] %s: refused rc=8 ('%s'), setup not run, nothing removed" % (sh, what, want),
                  rc == 8 and want in out and why in out and "Uninstalling" not in out and os.path.isfile(V(g)), out)
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "uninstall", "V3X_YES": "1"}, script=plain_ps1)
        check("[%s] the repo install.ps1 (no release key yet) refuses even a test-signed release rc=8" % sh,
              rc == 8 and "no release key yet" in out and os.path.isfile(V(g)), out)
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "status"})
        check("[%s] good signed release: 'Signature OK' then the setup runs" % sh, rc == 0 and "Signature OK" in out and "game folder:" in out, out)
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_SOURCE": os.path.join(root, "nowhere"), "V3X_YES": "1"})
        check("[%s] missing download: rc=1, 'download failed'" % sh, rc == 1 and "download failed" in out, out)

        # 7. uninstall through the menu (keep logs), then again: not installed
        rc, out = run(sh, {"V3X_STEAM_PATH": steam, "V3X_ANSWERS": "y;2;n"})
        check("[%s] menu: 2 uninstalls rc=0" % sh, rc == 0 and "uninstalled" in out and not os.path.exists(V(g)), out)
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "uninstall", "V3X_YES": "1"})
        check("[%s] uninstall when not installed: rc=5, plain words" % sh, rc == 5 and "was not installed here" in out, out)

        # 8. not found -> asked; a wrong folder, then the binaries folder is accepted
        g2 = ti.make_game(base, "ask", header)
        rc, out = run(sh, {"V3X_STEAM_PATH": os.path.join(base, "no_steam"),
                           "V3X_ANSWERS": "%s;\"%s\"" % (os.path.join(base, "nope"), os.path.join(g2, "binaries"))})
        check("[%s] not found: asks, rejects a wrong folder, accepts the quoted binaries folder rc=0" % sh,
              rc == 0 and "could not find Victoria 3" in out and "not the Victoria 3 folder" in out and os.path.isfile(V(g2)), out)
        # found but 'n' -> asked
        rc, out = run(sh, {"V3X_STEAM_PATH": steam, "V3X_ANSWERS": "n;%s" % g2, "V3X_ACTION": "uninstall", "V3X_PURGE": "1"})
        check("[%s] found but 'n': asks for the folder; uninstall with V3X_PURGE removes binaries\\v3x" % sh,
              rc == 0 and not os.path.exists(os.path.join(g2, "binaries", "v3x")) and os.path.exists(V(g)) is False, out)
        rc, out = run(sh, {"V3X_STEAM_PATH": os.path.join(base, "no_steam"), "V3X_YES": "1"})
        check("[%s] not found and no answer: rc=4" % sh, rc == 4 and "No game folder" in out, out)
        rc, out = run(sh, {"V3X_GAME_DIR": g, "V3X_ACTION": "bogus"})
        check("[%s] bad V3X_ACTION: rc=1" % sh, rc == 1, out)

        # 9. unsupported game build: default no, 'y' forces
        bad = bytearray(header)
        import struct
        lf = struct.unpack_from("<I", bad, 0x3c)[0]
        struct.pack_into("<I", bad, lf + 8, struct.unpack_from("<I", bad, lf + 8)[0] + 1)
        g3 = ti.make_game(base, "unsupported", bytes(bad), branch="release/1.15.2")
        rc, out = run(sh, {"V3X_GAME_DIR": g3, "V3X_YES": "1"})
        check("[%s] unsupported: default answer no -> rc=2, nothing written" % sh,
              rc == 2 and "not the one this V3X was made for" in out and not os.path.exists(V(g3)), out)
        rc, out = run(sh, {"V3X_GAME_DIR": g3, "V3X_ANSWERS": "y"})
        check("[%s] unsupported: 'y' installs anyway rc=0" % sh, rc == 0 and os.path.isfile(V(g3)), out)

        # 10. the real one-liner: irm <url> | iex, download over HTTP; the window must stay open
        g4 = ti.make_game(base, "iex", header)
        for srv, ct in ((srv_bin, "octet-stream"), (srv_txt, "text/plain")):
            url = "http://127.0.0.1:%d" % srv.server_address[1]
            rc, out = run(sh, {"V3X_GAME_DIR": g4, "V3X_SOURCE": url, "V3X_ACTION": "install"}, iex_url=url + "/install.ps1")
            check("[%s] irm | iex (%s): installs over HTTP, session not closed" % (sh, ct),
                  rc == 0 and "Done! V3X" in out and "STILL-ALIVE rc=0" in out and os.path.isfile(V(g4)), out)
        rc, out = run(sh, {"V3X_GAME_DIR": g4, "V3X_SOURCE": "http://127.0.0.1:%d" % srv_bin.server_address[1],
                           "V3X_ACTION": "uninstall", "V3X_YES": "1", "V3X_LANG": "pt"},
                      iex_url="http://127.0.0.1:%d/install.ps1" % srv_bin.server_address[1])
        check("[%s] irm | iex pt-BR uninstall: accents survive the download" % sh,
              "desinstalado" in out and "s\u00f3 para um jogador" in out and not os.path.exists(V(g4)), out)

    srv_bin.shutdown(); srv_txt.shutdown()
    fails = [n for n, ok in RESULTS if not ok]
    print("\n%d/%d passed%s" % (len(RESULTS) - len(fails), len(RESULTS), "" if not fails else "; FAILED: " + "; ".join(fails)))
    if not a.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
