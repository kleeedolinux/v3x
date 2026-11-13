"""Builds the player installer: build/dist/V3X-Setup.exe (one file, no admin rights needed).

    python installer/make_dist.py [--no-build] [--out DIR] [--version VER]

    --no-build     package the existing build/ instead of running ./build.sh first
    --out DIR      output folder (default: <repo>/build/dist)
    --version VER  version shown by the installer (default: dev-YYYYMMDD)
    --cc "CMD"     compiler (default: env V3X_CC, else "<python> -m ziglang cc")

Outputs in --out: V3X-Setup.exe, v3x-<version>.zip (the same files for a manual install: copy into
the game's binaries folder; + install.ps1 + README.txt), install.ps1 (the one-line installer) and
SHA256SUMS.txt (all three). install.ps1 checks V3X-Setup.exe against SHA256SUMS.txt.

Steps: ./build.sh (loader + every plugin into build/), compile installer/setup.c with zig into a
stub exe, then append the payload: build/version.dll, the plugins marked 1 or 0 in
installer/profile.txt, their plugins/<p>/<p>.ini.example as v3x/<p>.ini, a generated v3x/v3x.ini
([plugins] <p>=0 for the ones shipped off), and meta.txt (version, installer/builds.txt, profile).
Payload layout and checks: see the header of installer/setup.c. Needs: pip install ziglang, Git Bash.
Exit code 0 = built, 1 = error.
"""
import argparse
import datetime
import hashlib
import os
import shlex
import shutil
import struct
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def die(msg):
    print("make_dist: " + msg, file=sys.stderr)
    sys.exit(1)


def read_profile():
    prof = []
    for ln, line in enumerate(open(os.path.join(HERE, "profile.txt"), encoding="utf-8"), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 2 or parts[1] not in ("0", "1", "-"):
            die("profile.txt:%d: expected '<name> <1|0|-> <reason>'" % ln)
        prof.append((parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    return prof


def read_builds():
    out = []
    for line in open(os.path.join(HERE, "builds.txt"), encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            p = line.split()
            out.append((p[0], p[1] if len(p) > 1 else ""))
    if not out:
        die("builds.txt lists no supported build")
    return out


def v3x_ini(prof):
    lines = [
        "# V3X settings, created by V3X-Setup. Setup never overwrites this file.",
        "# console=0 hides the V3X console window that opens next to the game.",
        "console=1",
        "",
        "[plugins]",
        "# <plugin>=0: not loaded at the next start, 1: loaded. The main-menu V3X button changes these",
        "# for you. Comments must be on their own line.",
    ]
    lines += ["%s=%s" % (n, e) for n, e, _ in prof if e in ("0", "1")]
    return ("\r\n".join(lines) + "\r\n").encode("utf-8")


def zip_readme(version, prof):
    on = " ".join(n for n, e, _ in prof if e == "1")
    off = " ".join(n for n, e, _ in prof if e == "0")
    return "\r\n".join([
        "V3X %s for Victoria 3 1.15.1 - manual install" % version,
        "",
        "Easiest: use V3X-Setup.exe or the one-line installer instead (PowerShell):",
        "    irm https://v3x.pages.dev/install.ps1 | iex",
        "",
        "Manual install: close Victoria 3, then copy version.dll and the v3x folder from this zip into",
        "    <Steam>\\steamapps\\common\\Victoria 3\\binaries",
        "Do not overwrite your own v3x\\*.ini files if you already have them.",
        "Manual uninstall: delete binaries\\version.dll and the binaries\\v3x folder.",
        "",
        "On by default: " + on,
        "Installed but off (turn on in the main menu > V3X button): " + off,
        "Single player only. Help: https://github.com/kleeedolinux/v3x/blob/main/docs/install.md",
        ""])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "dist"))
    ap.add_argument("--version", default="dev-" + datetime.date.today().strftime("%Y%m%d"))
    ap.add_argument("--cc", default=os.environ.get("V3X_CC", "%s -m ziglang cc" % shlex.quote(sys.executable)))
    a = ap.parse_args()
    if " " in a.version:
        die("--version must not contain spaces")

    prof = read_profile()
    builds = read_builds()
    names = {n for n, _, _ in prof}
    missing = sorted(d for d in os.listdir(os.path.join(REPO, "plugins"))
                     if os.path.isdir(os.path.join(REPO, "plugins", d)) and d not in names)
    if missing:
        die("plugins not in installer/profile.txt (add them as 1, 0 or -): " + ", ".join(missing))

    if not a.no_build:
        bash = shutil.which("bash") or r"C:\Program Files\Git\bin\bash.exe"
        print("make_dist: running build.sh")
        if subprocess.call([bash, "build.sh"], cwd=REPO):
            die("build.sh failed")

    build = os.path.join(REPO, "build")
    files = []  # (kind, rel, bytes)
    files.append(("bin", "version.dll", open(os.path.join(build, "version.dll"), "rb").read()))
    for n, e, _ in prof:
        if e == "-":
            continue
        dll = os.path.join(build, "plugins", n + ".dll")
        if not os.path.isfile(dll):
            die("missing %s (run without --no-build)" % dll)
        files.append(("bin", "v3x/plugins/%s.dll" % n, open(dll, "rb").read()))
        ex = os.path.join(REPO, "plugins", n, n + ".ini.example")
        if os.path.isfile(ex):
            files.append(("ini", "v3x/%s.ini" % n, open(ex, "rb").read()))
    stale = []
    for kind, rel, _ in files:
        if kind != "bin":
            continue
        if rel == "version.dll":
            dll, srcs = os.path.join(build, "version.dll"), ["src", "include"]
        else:
            n = os.path.basename(rel)[:-4]
            dll, srcs = os.path.join(build, "plugins", n + ".dll"), [os.path.join("plugins", n), "include", "sdk"]
        newest = max((os.path.getmtime(os.path.join(REPO, d, f)) for d in srcs
                      for f in os.listdir(os.path.join(REPO, d)) if f.endswith((".c", ".h"))), default=0)
        if newest > os.path.getmtime(dll):
            stale.append(rel)
    if stale:
        print("make_dist: WARNING: source newer than the packaged DLL (rebuild with ./build.sh): " + ", ".join(stale))
    files.append(("v3xini", "v3x/v3x.ini", v3x_ini(prof)))
    meta = ["version " + a.version] + ["build %s %s" % b for b in builds]
    meta += ["plugin %s %s" % (n, e) for n, e, _ in prof if e in ("0", "1")]
    files.append(("meta", "meta.txt", ("\n".join(meta) + "\n").encode("utf-8")))

    os.makedirs(a.out, exist_ok=True)
    stub = os.path.join(a.out, "V3X-Setup.stub.exe")
    cmd = shlex.split(a.cc) + ["-target", "x86_64-windows-gnu", "-O2", "-Wall", "-municode",
                               "setup.c", "setup.rc", "-o", stub, "-Wl,--subsystem,windows",
                               "-lcomctl32", "-lbcrypt", "-lshell32", "-lole32", "-ladvapi32", "-luser32"]
    print("make_dist: compiling installer stub")
    if subprocess.call(cmd, cwd=HERE):
        die("compiling installer/setup.c failed")
    for f in os.listdir(a.out):
        if f.endswith((".pdb", ".lib")):
            os.remove(os.path.join(a.out, f))

    blob = bytearray(open(stub, "rb").read())
    os.remove(stub)
    index = []
    for kind, rel, data in files:
        index.append("%s %d %d %s %s" % (kind, len(blob), len(data), hashlib.sha256(data).hexdigest(), rel))
        blob += data
    idx = ("\n".join(index) + "\n").encode("utf-8")
    ioff = len(blob)
    blob += idx
    blob += struct.pack("<8sQII32s8s", b"V3XPAYL1", ioff, len(idx), len(files), hashlib.sha256(idx).digest(), b"V3XEND\0\0")
    out = os.path.join(a.out, "V3X-Setup.exe")
    with open(out, "wb") as f:
        f.write(blob)
    digest = hashlib.sha256(blob).hexdigest()

    # manual-install zip: the payload's files at their binaries/-relative paths + install.ps1 + README
    ps1 = open(os.path.join(HERE, "install.ps1"), "rb").read()
    for f in os.listdir(a.out):
        if f.startswith("v3x-") and f.endswith(".zip"):
            os.remove(os.path.join(a.out, f))
    zname = "v3x-%s.zip" % a.version
    with zipfile.ZipFile(os.path.join(a.out, zname), "w", zipfile.ZIP_DEFLATED) as z:
        def add(rel, data):
            zi = zipfile.ZipInfo(rel, date_time=(2020, 1, 1, 0, 0, 0))  # fixed time: same input, same zip
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)
        for kind, rel, data in files:
            if kind != "meta":
                add(rel, data)
        add("install.ps1", ps1)
        add("README.txt", zip_readme(a.version, prof).encode("utf-8"))
    with open(os.path.join(a.out, "install.ps1"), "wb") as f:
        f.write(ps1)
    sums = [(digest, "V3X-Setup.exe")]
    for n in (zname, "install.ps1"):
        sums.append((hashlib.sha256(open(os.path.join(a.out, n), "rb").read()).hexdigest(), n))
    with open(os.path.join(a.out, "SHA256SUMS.txt"), "w", newline="\n") as f:
        f.write("".join("%s  %s\n" % x for x in sums))
    on = [n for n, e, _ in prof if e == "1"]
    off = [n for n, e, _ in prof if e == "0"]
    print("make_dist: %s (%d bytes, version %s, sha256 %s)" % (out, len(blob), a.version, digest))
    print("make_dist: %s, install.ps1, SHA256SUMS.txt" % os.path.join(a.out, zname))
    print("make_dist: on : " + " ".join(on))
    print("make_dist: off: " + " ".join(off))
    return 0


if __name__ == "__main__":
    sys.exit(main())
