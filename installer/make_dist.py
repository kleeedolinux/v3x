"""Builds the player installer: build/dist/V3X-Setup.exe (one file, no admin rights needed).

    python installer/make_dist.py [--no-build] [--out DIR] [--version VER]

    --no-build     package the existing build/ instead of running ./build.sh first
    --out DIR      output folder (default: <repo>/build/dist)
    --version VER  version shown by the installer (default: dev-YYYYMMDD)
    --cc "CMD"     compiler (default: env V3X_CC, else "<python> -m ziglang cc")
    --trust-key HEX  ALSO trust this Ed25519 public key in V3X-Setup.exe --verify-release (dev dry
                   runs and tests with a dev key; tools/sign_release.py). Never for a published
                   release: CI does not pass it; the exe then says "DEV TEST key" when it verifies.
    --add-sig DIR  release step (no build, no compiler): DIR holds the files of an earlier run plus
                   plugins.json.sig (tools/sign_release.py sign DIR/plugins.json). Adds the signature
                   to the payload of DIR/V3X-Setup.exe and to the zip as v3x/plugins.json.sig, then
                   rewrites the asset hashes in DIR/compat.json and DIR/SHA256SUMS.txt. Refuses if the
                   plugins.json inside the setup is not byte-identical to DIR/plugins.json. compat.json
                   must be signed AFTER this step (release.yml does).

Outputs in --out: V3X-Setup.exe, v3x-<version>.zip (the same files for a manual install: copy into
the game's binaries folder; + install.ps1 + README.txt), install.ps1 (the one-line installer) and
SHA256SUMS.txt (all four), compat.json (version, loader API, supported game builds from
installer/builds.txt, asset names + SHA-256) for the in-game updater, and plugins.json (the official
plugin list: name, file, sha256, version of every shipped plugin DLL; the same bytes are installed as
binaries/v3x/plugins.json; the loader trusts it only with a valid plugins.json.sig). The release workflow then
signs compat.json (tools/sign_release.py -> compat.json.sig); install.ps1 and the updater verify that
signature and take every hash from the signed compat.json.

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
import json
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


def read_define(rel, name):
    """value of '#define <name> <value>' in a repo source file (quotes stripped)"""
    for line in open(os.path.join(REPO, rel), encoding="utf-8"):
        p = line.split()
        if len(p) >= 3 and p[0] == "#define" and p[1] == name:
            return p[2].strip('"')
    die("%s: #define %s not found" % (rel, name))


def compat_json(version, builds, out_dir, names):
    """compat.json release asset for the in-game updater (plugins/updater; format agreed in mem chat
    #4420/#4425): what this release supports. game_builds come from installer/builds.txt (the single
    source of truth, also used by V3X-Setup.exe); build_id is the same string as the loader's
    api->game_build_id(). UTF-8, no BOM."""
    assets = []
    for n in names:
        data = open(os.path.join(out_dir, n), "rb").read()
        assets.append({"name": n, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    doc = {
        "schema": 1,
        "v3x_version": version,
        "loader_semver": read_define("src/core.c", "V3X_LOADER_SEMVER"),
        "api_version": int(read_define("include/v3x.h", "V3X_API_VERSION")),
        "game_builds": [{"branch": br, "build_id": bid} for bid, br in builds],
        "assets": assets,
    }
    return json.dumps(doc, indent=2) + "\n"


def plugin_version(n, fallback):
    """version string of V3X_PLUGIN_MANIFEST(name, version, ...) in plugins/<n>/*.c (a string literal or a
    macro #defined as one in that folder); fallback if not found"""
    import re
    d = os.path.join(REPO, "plugins", n)
    texts = [open(os.path.join(d, f), encoding="utf-8", errors="replace").read()
             for f in sorted(os.listdir(d)) if f.endswith((".c", ".h"))]
    for t in texts:
        m = re.search(r'V3X_PLUGIN_MANIFEST(?:_[A-Z]+)?\(\s*(?:"[^"]*"|\w+)\s*,\s*("([^"]+)"|(\w+))', t)
        if not m:
            continue
        if m.group(2):
            return m.group(2)
        for t2 in texts:
            m2 = re.search(r'#define\s+%s\s+"([^"]+)"' % re.escape(m.group(3)), t2)
            if m2:
                return m2.group(1)
    return fallback


def plugins_json(version, plugin_files):
    """plugins.json (format agreed with the loader, mem chat #4483/#4493): the official plugins of this
    release. plugin_files: [(name, dll bytes)]. 'file' is relative to binaries/v3x/. UTF-8, no BOM, LF."""
    doc = {
        "schema": 1,
        "v3x_version": version,
        "plugins": [{"name": n, "file": "plugins/%s.dll" % n, "sha256": hashlib.sha256(data).hexdigest(),
                     "version": plugin_version(n, version)} for n, data in sorted(plugin_files)],
    }
    return (json.dumps(doc, indent=2) + "\n").encode("utf-8")


def read_payload(blob):
    """(stub_end, [(kind, off, size, sha, rel)]) of a V3X-Setup.exe; dies on a bad trailer"""
    if len(blob) < 64:
        die("not a V3X-Setup.exe (too short)")
    magic, ioff, isz, cnt, ish, end = struct.unpack("<8sQII32s8s", blob[-64:])
    if magic != b"V3XPAYL1" or end != b"V3XEND\0\0" or ioff + isz + 64 != len(blob):
        die("not a V3X-Setup.exe payload trailer")
    idx = blob[ioff:ioff + isz]
    if hashlib.sha256(idx).digest() != ish:
        die("payload index hash mismatch")
    ents = []
    for line in idx.decode("utf-8").splitlines():
        k, off, size, h, rel = line.split()
        ents.append((k, int(off), int(size), h, rel))
    if len(ents) != cnt:
        die("payload index count mismatch")
    return ioff, ents


def write_sums_and_compat(out_dir, zname, compat_text=None):
    """SHA256SUMS.txt over setup, zip, install.ps1, compat.json; compat.json asset hashes refreshed
    (compat_text: a new compat.json, else the existing one is updated in place)"""
    names = ["V3X-Setup.exe", zname, "install.ps1"]
    if compat_text is None:
        doc = json.load(open(os.path.join(out_dir, "compat.json"), encoding="utf-8"))
        by = {a["name"]: a for a in doc.get("assets", [])}
        for n in names:
            data = open(os.path.join(out_dir, n), "rb").read()
            if n not in by:
                die("compat.json has no asset %s" % n)
            by[n]["size"] = len(data)
            by[n]["sha256"] = hashlib.sha256(data).hexdigest()
        compat_text = json.dumps(doc, indent=2) + "\n"
    with open(os.path.join(out_dir, "compat.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(compat_text)
    sums = [(hashlib.sha256(open(os.path.join(out_dir, n), "rb").read()).hexdigest(), n) for n in names]
    sums.append((hashlib.sha256(compat_text.encode("utf-8")).hexdigest(), "compat.json"))
    with open(os.path.join(out_dir, "SHA256SUMS.txt"), "w", newline="\n") as f:
        f.write("".join("%s  %s\n" % x for x in sums))


def zip_add(z, rel, data):
    zi = zipfile.ZipInfo(rel, date_time=(2020, 1, 1, 0, 0, 0))  # fixed time: same input, same zip
    zi.compress_type = zipfile.ZIP_DEFLATED
    zi.external_attr = 0o644 << 16
    z.writestr(zi, data)


def add_sig(d):
    """--add-sig DIR: put DIR/plugins.json.sig into the setup payload and the zip (see the docstring)"""
    setup = os.path.join(d, "V3X-Setup.exe")
    pj = os.path.join(d, "plugins.json")
    ps = os.path.join(d, "plugins.json.sig")
    for f in (setup, pj, ps, os.path.join(d, "compat.json"), os.path.join(d, "install.ps1")):
        if not os.path.isfile(f):
            die("--add-sig: missing " + f)
    sig = open(ps, "rb").read()
    if not sig.startswith(b"v3x-sig 1 ") or len(sig) > 200 or b"\0" in sig:
        die("--add-sig: plugins.json.sig is not a 'v3x-sig 1 ...' line")
    pjb = open(pj, "rb").read()
    blob = bytearray(open(setup, "rb").read())
    ioff, ents = read_payload(blob)
    rels = {e[4]: e for e in ents}
    if "v3x/plugins.json.sig" in rels:
        die("--add-sig: the setup already contains v3x/plugins.json.sig")
    e = rels.get("v3x/plugins.json")
    if not e or bytes(blob[e[1]:e[1] + e[2]]) != pjb:
        die("--add-sig: plugins.json in the setup differs from %s (sign the file make_dist wrote)" % pj)
    new = bytearray(blob[:ioff])
    soff = len(new)
    new += sig
    lines = ["%s %d %d %s %s" % x for x in ents]
    lines.append("bin %d %d %s v3x/plugins.json.sig" % (soff, len(sig), hashlib.sha256(sig).hexdigest()))
    idx = ("\n".join(lines) + "\n").encode("utf-8")
    nioff = len(new)
    new += idx
    new += struct.pack("<8sQII32s8s", b"V3XPAYL1", nioff, len(idx), len(lines), hashlib.sha256(idx).digest(), b"V3XEND\0\0")
    zips = [f for f in os.listdir(d) if f.startswith("v3x-") and f.endswith(".zip")]
    if len(zips) != 1:
        die("--add-sig: expected exactly one v3x-*.zip in " + d)
    zpath = os.path.join(d, zips[0])
    with zipfile.ZipFile(zpath) as z:
        items = [(i.filename, z.read(i.filename)) for i in z.infolist()]
    if dict(items).get("v3x/plugins.json") != pjb:
        die("--add-sig: plugins.json in the zip differs from " + pj)
    tmpz = zpath + ".tmp"
    with zipfile.ZipFile(tmpz, "w", zipfile.ZIP_DEFLATED) as z:
        for rel, data in items:
            zip_add(z, rel, data)
            if rel == "v3x/plugins.json":
                zip_add(z, "v3x/plugins.json.sig", sig)
    with open(setup + ".tmp", "wb") as f:
        f.write(new)
    os.replace(setup + ".tmp", setup)
    os.replace(tmpz, zpath)
    write_sums_and_compat(d, zips[0])
    print("make_dist: added plugins.json.sig to %s and %s; compat.json + SHA256SUMS.txt updated (sign compat.json now)"
          % (setup, zpath))
    return 0


def zip_readme(version, prof):
    on = " ".join(n for n, e, _ in prof if e == "1")
    off = " ".join(n for n, e, _ in prof if e == "0")
    return "\r\n".join([
        "V3X %s for Victoria 3 1.15.1 - manual install" % version,
        "",
        "Easiest: use V3X-Setup.exe or the one-line installer instead (PowerShell):",
        "    irm https://v3extender.pages.dev/install.ps1 | iex",
        "Verifiable two-step install: download install.ps1 of this release, compare its SHA-256",
        "(Get-FileHash .\\install.ps1) with the install.ps1 entry of the signed compat.json (also in the",
        "release notes), then run: powershell -ExecutionPolicy Bypass -File .\\install.ps1",
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
    ap.add_argument("--trust-key", default="")
    ap.add_argument("--add-sig", default="", metavar="DIR")
    a = ap.parse_args()
    if a.add_sig:
        return add_sig(a.add_sig)
    if a.trust_key and (len(a.trust_key) != 64 or any(c not in "0123456789abcdef" for c in a.trust_key)):
        die("--trust-key must be 64 lowercase hex digits (an Ed25519 public key)")
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
    plist = plugins_json(a.version, [(os.path.basename(rel)[:-4], data) for kind, rel, data in files
                                     if kind == "bin" and rel.startswith("v3x/plugins/")])
    files.append(("bin", "v3x/plugins.json", plist))
    files.append(("v3xini", "v3x/v3x.ini", v3x_ini(prof)))
    meta = ["version " + a.version] + ["build %s %s" % b for b in builds]
    meta += ["plugin %s %s" % (n, e) for n, e, _ in prof if e in ("0", "1")]
    files.append(("meta", "meta.txt", ("\n".join(meta) + "\n").encode("utf-8")))

    os.makedirs(a.out, exist_ok=True)
    stub = os.path.join(a.out, "V3X-Setup.stub.exe")
    cmd = shlex.split(a.cc) + ["-target", "x86_64-windows-gnu", "-O2", "-Wall", "-municode",
                               "-I" + os.path.join(REPO, "include"), "-I" + os.path.join(REPO, "sdk")]
    if a.trust_key:
        print("make_dist: NOTE: V3X-Setup.exe also trusts the DEV TEST key %s... (not for publishing)" % a.trust_key[:16])
        cmd += ['-DV3X_RELEASE_KEY_DEVTEST="%s"' % a.trust_key]
    cmd += ["setup.c", "setup.rc", "-o", stub, "-Wl,--subsystem,windows",
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
            zip_add(z, rel, data)
        for kind, rel, data in files:
            if kind != "meta":
                add(rel, data)
        add("install.ps1", ps1)
        add("README.txt", zip_readme(a.version, prof).encode("utf-8"))
    with open(os.path.join(a.out, "install.ps1"), "wb") as f:
        f.write(ps1)
    for f in ("plugins.json.sig",):
        if os.path.exists(os.path.join(a.out, f)):
            os.remove(os.path.join(a.out, f))
    with open(os.path.join(a.out, "plugins.json"), "wb") as f:   # the exact bytes in the payload (to be signed)
        f.write(plist)
    compat = compat_json(a.version, builds, a.out, ["V3X-Setup.exe", zname, "install.ps1"])
    write_sums_and_compat(a.out, zname, compat)
    on = [n for n, e, _ in prof if e == "1"]
    off = [n for n, e, _ in prof if e == "0"]
    print("make_dist: %s (%d bytes, version %s, sha256 %s)" % (out, len(blob), a.version, digest))
    print("make_dist: %s, install.ps1, compat.json, plugins.json, SHA256SUMS.txt" % os.path.join(a.out, zname))
    print("make_dist: on : " + " ".join(on))
    print("make_dist: off: " + " ".join(off))
    return 0


if __name__ == "__main__":
    sys.exit(main())
