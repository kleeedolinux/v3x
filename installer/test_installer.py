"""End-to-end test of V3X-Setup.exe against fake game folders (never the real game).

    python installer/test_installer.py [--exe build/dist/V3X-Setup.exe] [--root D:/v3x_installer_test/run]
                                       [--real-exe <game>/binaries/victoria3.exe] [--keep]

A fake game = <root>/<case>/Victoria 3/{caligula_branch.txt, binaries/victoria3.exe}, where the exe is
the first 4 KB (PE headers only) of the real victoria3.exe, so its build id is the real one; the
real exe is only read. The "game running" case compiles a sleeper with zig and starts it as the
fake folder's victoria3.exe. <root> is deleted at the end unless --keep. Exit 0 = all passed.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
RESULTS = []


def check(name, cond, detail=""):
    RESULTS.append((name, bool(cond)))
    print("%s  %s%s" % ("ok  " if cond else "FAIL", name, ("  -- " + detail) if (detail and not cond) else ""))


def sha(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def payload(exe):
    """index entries [(kind, rel, sha)] read the same way setup.c does"""
    b = open(exe, "rb").read()
    ft = b[-64:]
    magic, ioff, isz, cnt, ish, end = struct.unpack("<8sQII32s8s", ft)
    assert magic == b"V3XPAYL1" and end == b"V3XEND\0\0"
    out = []
    for line in b[ioff:ioff + isz].decode().splitlines():
        k, off, size, h, rel = line.split()
        out.append((k, rel, h, b[int(off):int(off) + int(size)]))
    return out


def run(exe, *args):
    p = subprocess.run([exe, "--silent"] + list(args), capture_output=True, timeout=120)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def make_game(root, case, header, branch="release/1.15.1"):
    g = os.path.join(root, case, "Victoria 3")
    os.makedirs(os.path.join(g, "binaries"), exist_ok=True)
    open(os.path.join(g, "binaries", "victoria3.exe"), "wb").write(header)
    open(os.path.join(g, "caligula_branch.txt"), "w").write(branch)
    return g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "dist", "V3X-Setup.exe"))
    ap.add_argument("--root", default="D:/v3x_installer_test/run")
    ap.add_argument("--real-exe", default=os.path.join(os.path.dirname(REPO), "binaries", "victoria3.exe"))
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    root = os.path.abspath(a.root)
    if os.path.commonpath([root.lower(), os.path.dirname(REPO).lower()]) == os.path.dirname(REPO).lower():
        sys.exit("refusing: --root must not be inside the game folder")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    header = open(a.real_exe, "rb").read(4096)
    ents = payload(exe)
    bins = [(rel, h) for k, rel, h, _ in ents if k == "bin"]
    inis = [(rel, h) for k, rel, h, _ in ents if k in ("ini", "v3xini")]

    # 1. fresh install
    g = make_game(root, "main", header)
    B = os.path.join(g, "binaries")
    rc, out = run(exe, "--game-dir", g)
    check("fresh install rc=0", rc == 0, out)
    check("fresh install: every program file present with the payload hash",
          all(os.path.isfile(os.path.join(B, r)) and sha(os.path.join(B, r)) == h for r, h in bins))
    check("fresh install: every settings file created", all(os.path.isfile(os.path.join(B, r)) for r, _ in inis))
    man = os.path.join(B, "v3x", "install_manifest.txt")
    mtxt = open(man).read() if os.path.isfile(man) else ""
    check("fresh install: manifest lists every file", all((" %s %s" % (h, r)) in mtxt for r, h in bins))
    v3xini = open(os.path.join(B, "v3x", "v3x.ini")).read()
    check("fresh install: v3x.ini has console_plus=0 and crash_logger=1", "console_plus=0" in v3xini and "crash_logger=1" in v3xini)
    check("fresh install: no dev plugin shipped", not os.path.exists(os.path.join(B, "v3x", "plugins", "devtools.dll")))
    check("fresh install: no backup (no previous version.dll)", "backup " not in mtxt)
    rc, out = run(exe, "--status", "--game-dir", g)
    check("status rc=0 and says installed + supported", rc == 0 and "installed:   V3X" in out and "- supported" in out, out)

    # 2. re-install: user settings preserved byte for byte, plugin key removed by the user re-added as =0 only if missing
    fp = os.path.join(B, "v3x", "frame_pacer.ini")
    open(fp, "a").write("\nidle_fps=7\n")
    user_fp = open(fp, "rb").read()
    vi = os.path.join(B, "v3x", "v3x.ini")
    user_v3x = "console=0\r\n[plugins]\r\ncrash_logger=0\r\nconsole_plus=1\r\n"
    open(vi, "w", newline="").write(user_v3x)
    user_cfg = os.path.join(B, "v3x", "my_own.ini")
    open(user_cfg, "w").write("x=1\n")
    rc, out = run(exe, "--game-dir", g)
    check("re-install rc=0", rc == 0, out)
    check("re-install: modified frame_pacer.ini unchanged", open(fp, "rb").read() == user_fp)
    now = open(vi, newline="").read()
    check("re-install: v3x.ini keeps user lines (console=0, crash_logger=0, console_plus=1)",
          now.startswith("console=0\r\n[plugins]\r\n") and "crash_logger=0" in now and "console_plus=1" in now
          and "console_plus=0" not in now, repr(now))
    check("re-install: v3x.ini got missing off-lines (sim_opt=0) under [plugins]", "[plugins]\r\nsim_opt=0" in now or "sim_opt=0\r\n" in now, repr(now))
    rc, out2 = run(exe, "--game-dir", g)
    check("re-install twice: v3x.ini not touched again", open(vi, newline="").read() == now and rc == 0)
    check("re-install twice: nothing written", "0 file(s) written" in out2, out2)

    # 3. repair: deleted plugin + corrupted plugin come back
    pl = os.path.join(B, "v3x", "plugins")
    os.remove(os.path.join(pl, "keep_history.dll"))
    open(os.path.join(pl, "log_filter.dll"), "wb").write(b"garbage")
    rc, out = run(exe, "--repair", "--game-dir", g)
    check("repair rc=0", rc == 0, out)
    check("repair: deleted keep_history.dll restored", os.path.isfile(os.path.join(pl, "keep_history.dll"))
          and sha(os.path.join(pl, "keep_history.dll")) == dict(bins)["v3x/plugins/keep_history.dll"])
    check("repair: corrupted log_filter.dll replaced", sha(os.path.join(pl, "log_filter.dll")) == dict(bins)["v3x/plugins/log_filter.dll"])
    check("repair: exactly 2 files written", "2 file(s) written" in out, out)

    # 4. a file of an older V3X that this payload no longer ships is removed on update
    old = os.path.join(pl, "old_plugin.dll")
    open(old, "wb").write(b"old v3x plugin")
    with open(man, "a") as f:
        f.write("file bin %s v3x/plugins/old_plugin.dll\n" % sha(old))
    rc, out = run(exe, "--game-dir", g)
    check("update: stale V3X file removed", rc == 0 and not os.path.exists(old), out)

    # 5. uninstall (keep): logs + changed settings stay, unchanged created settings go, user files stay
    log = os.path.join(B, "v3x", "v3x.log")
    open(log, "w").write("log\n")
    rc, out = run(exe, "--uninstall", "--game-dir", g)
    check("uninstall rc=0", rc == 0, out)
    check("uninstall: version.dll and all plugins gone", not os.path.exists(os.path.join(B, "version.dll"))
          and not any(os.path.exists(os.path.join(B, r)) for r, _ in bins))
    check("uninstall: log kept", os.path.isfile(log))
    check("uninstall: changed frame_pacer.ini and v3x.ini kept", os.path.isfile(fp) and os.path.isfile(vi))
    check("uninstall: unchanged created ini removed (mem_tuner.ini)", not os.path.exists(os.path.join(B, "v3x", "mem_tuner.ini")))
    check("uninstall: user's own file kept", os.path.isfile(user_cfg))
    check("uninstall: manifest removed", not os.path.exists(man))
    check("uninstall: game exe untouched", open(os.path.join(B, "victoria3.exe"), "rb").read() == header)
    rc, out = run(exe, "--uninstall", "--game-dir", g)
    check("uninstall again (no manifest) rc=5", rc == 5, out)

    # 6. foreign version.dll: backed up, restored byte-identical on uninstall; --purge removes binaries/v3x
    g2 = make_game(root, "foreign", header)
    B2 = os.path.join(g2, "binaries")
    foreign = b"MZ foreign proxy dll from another tool" + os.urandom(64)
    open(os.path.join(B2, "version.dll"), "wb").write(foreign)
    rc, out = run(exe, "--game-dir", g2)
    bk = os.path.join(B2, "v3x", "backup", "version.dll")
    check("foreign: install rc=0 and backup made", rc == 0 and os.path.isfile(bk) and open(bk, "rb").read() == foreign, out)
    check("foreign: version.dll is now V3X's", sha(os.path.join(B2, "version.dll")) == dict(bins)["version.dll"])
    rc, out = run(exe, "--game-dir", g2)
    check("foreign: re-install makes no second backup", rc == 0 and not os.path.exists(os.path.join(B2, "v3x", "backup", "version.1.dll")))
    rc, out = run(exe, "--uninstall", "--purge", "--game-dir", g2)
    check("foreign: uninstall --purge rc=0", rc == 0, out)
    check("foreign: original version.dll restored byte-identical", open(os.path.join(B2, "version.dll"), "rb").read() == foreign)
    check("foreign: binaries/v3x deleted by --purge", not os.path.exists(os.path.join(B2, "v3x")))

    # 7. a manual V3X install (version.dll from build.sh, no manifest) is not backed up as foreign
    g3 = make_game(root, "manual", header)
    # an older self-built loader: V3X but not byte-identical to the payload's
    open(os.path.join(g3, "binaries", "version.dll"), "wb").write(open(os.path.join(REPO, "build", "version.dll"), "rb").read() + b"older")
    rc, out = run(exe, "--game-dir", g3)
    check("manual V3X: install rc=0, no backup", rc == 0 and not os.path.exists(os.path.join(g3, "binaries", "v3x", "backup")), out)

    # 8. unsupported build refused, nothing written; --force installs
    bad = bytearray(header)
    lfanew = struct.unpack_from("<I", bad, 0x3c)[0]
    struct.pack_into("<I", bad, lfanew + 8, struct.unpack_from("<I", bad, lfanew + 8)[0] + 1)
    g4 = make_game(root, "unsupported", bytes(bad), branch="release/1.15.2")
    rc, out = run(exe, "--game-dir", g4)
    check("unsupported: rc=2", rc == 2, out)
    check("unsupported: nothing written", sorted(os.listdir(os.path.join(g4, "binaries"))) == ["victoria3.exe"])
    rc, out = run(exe, "--status", "--game-dir", g4)
    check("unsupported: status says NOT supported", "NOT supported" in out, out)
    rc, out = run(exe, "--force", "--game-dir", g4)
    check("unsupported: --force installs rc=0", rc == 0 and os.path.isfile(os.path.join(g4, "binaries", "version.dll")), out)

    # 9. game running: refused for install and uninstall
    g5 = make_game(root, "running", header)
    src = os.path.join(root, "sleeper.c")
    open(src, "w").write("#include <windows.h>\nint main(void){Sleep(120000);return 0;}\n")
    sl_exe = os.path.join(g5, "binaries", "victoria3.exe")
    cc = [sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", src, "-o", sl_exe]
    built = subprocess.call(cc) == 0
    for f in os.listdir(os.path.join(g5, "binaries")):
        if f.endswith((".pdb", ".lib")):
            os.remove(os.path.join(g5, "binaries", f))
    check("running: sleeper built", built)
    proc = subprocess.Popen([sl_exe])
    time.sleep(0.5)
    try:
        rc, out = run(exe, "--force", "--game-dir", g5)
        check("running: install refused rc=3", rc == 3, out)
        check("running: nothing written", sorted(os.listdir(os.path.join(g5, "binaries"))) == ["victoria3.exe"])
    finally:
        proc.kill(); proc.wait()
    rc, out = run(exe, "--force", "--game-dir", g5)
    check("running: after the game exits install works rc=0", rc == 0, out)
    proc = subprocess.Popen([sl_exe])
    time.sleep(0.5)
    try:
        rc, out = run(exe, "--uninstall", "--game-dir", g5)
        check("running: uninstall refused rc=3", rc == 3 and os.path.isfile(os.path.join(g5, "binaries", "version.dll")), out)
    finally:
        proc.kill(); proc.wait()
    rc, out = run(exe, "--uninstall", "--game-dir", g5)
    check("running: uninstall after exit rc=0", rc == 0, out)

    # 10. damaged setup file, missing game, bad usage
    dmg = os.path.join(root, "damaged.exe")
    b = bytearray(open(exe, "rb").read())
    b[len(b) - 64 - 5000] ^= 0xff
    open(dmg, "wb").write(b)
    g6 = make_game(root, "damaged", header)
    rc, out = run(dmg, "--game-dir", g6)
    check("damaged payload: rc=6, nothing written", rc == 6 and sorted(os.listdir(os.path.join(g6, "binaries"))) == ["victoria3.exe"], out)
    rc, out = run(exe, "--game-dir", os.path.join(root, "nope"))
    check("missing game: rc=4", rc == 4, out)
    rc, out = run(exe, "--bogus")
    check("bad usage: rc=7", rc == 7, out)
    rc, out = run(exe, "--status", "--game-dir", os.path.join(g, "binaries"))
    check("game dir given as binaries folder is accepted", rc == 0 and "game folder:" in out, out)

    fails = [n for n, ok in RESULTS if not ok]
    print("\n%d/%d passed%s" % (len(RESULTS) - len(fails), len(RESULTS), "" if not fails else "; FAILED: " + "; ".join(fails)))
    if not a.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
