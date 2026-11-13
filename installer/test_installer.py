"""End-to-end test of V3X-Setup.exe against fake game folders (never the real game).

    python installer/test_installer.py [--exe build/dist/V3X-Setup.exe] [--root D:/v3x_installer_test/run]
                                       [--real-exe <game>/binaries/victoria3.exe] [--synthetic] [--keep]

A fake game = <root>/<case>/Victoria 3/{caligula_branch.txt, binaries/victoria3.exe}, where the exe is
the first 4 KB (PE headers only) of the real victoria3.exe, so its build id is the real one; the
real exe is only read. Without the real exe (CI) a synthetic 4 KB PE header carrying the first build
id of installer/builds.txt is used instead (--synthetic forces that). The "game running" case compiles a sleeper with zig and starts it as the
fake folder's victoria3.exe. <root> is deleted at the end unless --keep. Exit 0 = all passed.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import threading
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


# installer/profile.txt policy (lead, 2026-11-13): everything is preinstalled except pure developer
# tools; only these are on. Keep in sync with profile.txt.
EXPECT_ON = {"crash_logger", "content_packs", "mod_menu", "script_ext"}
EXPECT_UNSHIPPED = {"devtools", "devbridge", "devconsole", "tick_profiler", "hello", "alloc_tracker"}
# plugins still being built (profile line "-" or 0 until the CTO decides): allowed either way
PENDING = {"updater", "doctor"}


def synthetic_header():
    """4 KB PE header whose TimeDateStamp-SizeOfImage is the first id in builds.txt (setup.c pe_build_id)"""
    bid = next(l.split()[0] for l in open(os.path.join(HERE, "builds.txt")) if l.strip() and not l.startswith("#"))
    ts, soi = (int(x, 16) for x in bid.split("-"))
    h = bytearray(4096)
    h[0:2] = b"MZ"
    struct.pack_into("<I", h, 0x3c, 0x80)
    h[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", h, 0x84, 0x8664, 0)
    struct.pack_into("<I", h, 0x88, ts)
    struct.pack_into("<H", h, 0x80 + 24, 0x20b)
    struct.pack_into("<I", h, 0x80 + 24 + 56, soi)
    return bytes(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "dist", "V3X-Setup.exe"))
    ap.add_argument("--root", default="D:/v3x_installer_test/run")
    ap.add_argument("--real-exe", default=os.path.join(os.path.dirname(REPO), "binaries", "victoria3.exe"))
    ap.add_argument("--synthetic", action="store_true", help="use a synthetic PE header even if the real exe exists")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe)
    root = os.path.abspath(a.root)
    if os.path.commonpath([root.lower(), os.path.dirname(REPO).lower()]) == os.path.dirname(REPO).lower():
        sys.exit("refusing: --root must not be inside the game folder")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    if a.synthetic or not os.path.isfile(a.real_exe):
        print("note: using a synthetic victoria3.exe header (%s)" % ("--synthetic" if a.synthetic else "no " + a.real_exe))
        header = synthetic_header()
    else:
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
    # profile policy: every plugin in plugins/ except the developer tools is shipped; only EXPECT_ON is on
    all_plugins = {d for d in os.listdir(os.path.join(REPO, "plugins")) if os.path.isdir(os.path.join(REPO, "plugins", d))}
    shipped = {os.path.basename(r)[:-4] for r, _ in bins if r.startswith("v3x/plugins/")}
    want = all_plugins - EXPECT_UNSHIPPED - PENDING
    check("profile: shipped = every plugin except the developer tools", want <= shipped <= want | PENDING,
          "extra %s missing %s" % (sorted(shipped - want - PENDING), sorted(want - shipped)))
    check("profile: no developer tool shipped", not (shipped & EXPECT_UNSHIPPED), str(sorted(shipped & EXPECT_UNSHIPPED)))
    kv = dict(l.strip().split("=", 1) for l in v3xini.splitlines() if "=" in l and not l.startswith("#") and l.split("=")[0] in shipped)
    check("profile: v3x.ini has a line for every shipped plugin", set(kv) == shipped, str(sorted(shipped - set(kv))))
    check("profile: on = exactly crash_logger content_packs mod_menu script_ext",
          {n for n, v in kv.items() if v == "1"} - PENDING == EXPECT_ON, str(sorted(n for n, v in kv.items() if v == "1")))
    check("profile: every other shipped plugin is off (=0)", all(kv[n] == "0" for n in shipped - EXPECT_ON - PENDING))
    check("profile: keep_history and fast_load ship off", kv.get("keep_history") == "0" and kv.get("fast_load") == "0")
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

    # 11. --update (in-game updater) + --wait-pid
    def lastlog(gd):
        lp = os.path.join(gd, "binaries", "v3x", "install.log")
        return open(lp, encoding="utf-8").read().strip().splitlines()[-1] if os.path.isfile(lp) else ""
    g7 = make_game(root, "update", header)
    B7 = os.path.join(g7, "binaries")
    rc, out = run(exe, "--game-dir", g7)
    fp7 = os.path.join(B7, "v3x", "frame_pacer.ini")
    open(fp7, "a").write("\nidle_fps=9\n")
    user7 = open(fp7, "rb").read()
    os.remove(os.path.join(B7, "v3x", "plugins", "keep_history.dll"))
    waiter = subprocess.Popen([sl_exe])        # the "game" that the updater waits for (any exe works)
    killer = threading.Timer(2.0, waiter.kill)
    killer.start()
    t0 = time.time()
    try:
        p = subprocess.run([exe, "--update", "--silent", "--game-dir", B7, "--wait-pid", str(waiter.pid)],
                           capture_output=True, timeout=120)
    finally:
        killer.cancel(); waiter.kill(); waiter.wait()
    took = time.time() - t0
    out = p.stdout.decode("utf-8", "replace")
    check("update: --wait-pid waited for the process to exit (>= 1.5 s)", took >= 1.5, "%.2f s" % took)
    check("update: rc=0, deleted plugin restored, user ini kept", p.returncode == 0 and os.path.isfile(os.path.join(B7, "v3x", "plugins", "keep_history.dll"))
          and open(fp7, "rb").read() == user7, out)
    check("update: install.log last line 'update-result: ok version=...'", lastlog(g7).startswith("update-result: ok version="), lastlog(g7))
    rc = subprocess.run([exe, "--update", "--game-dir", g7, "--wait-pid", "4000000000"], capture_output=True, timeout=120).returncode
    check("update: --wait-pid of a process that is gone does not wait, rc=0", rc == 0)
    waiter = subprocess.Popen([sl_exe])
    try:
        env = dict(os.environ, V3X_WAIT_TIMEOUT_SEC="1")
        p = subprocess.run([exe, "--update", "--game-dir", g7, "--wait-pid", str(waiter.pid)], capture_output=True, timeout=120, env=env)
    finally:
        waiter.kill(); waiter.wait()
    check("update: wait timeout -> rc=1, 'update-result: failed code=3 timeout'", p.returncode == 1 and lastlog(g7) == "update-result: failed code=3 timeout",
          lastlog(g7) + p.stdout.decode("utf-8", "replace"))
    rc = subprocess.run([exe, "--update", "--game-dir", g4], capture_output=True, timeout=120).returncode   # g4: --force installed, unsupported
    check("update: unsupported build -> rc=1, never forced, 'failed code=2 unsupported'", rc == 1 and lastlog(g4) == "update-result: failed code=2 unsupported", lastlog(g4))
    proc = subprocess.Popen([os.path.join(g5, "binaries", "victoria3.exe")])
    time.sleep(0.5)
    try:
        p = subprocess.run([exe, "--update", "--game-dir", g5], capture_output=True, timeout=120)
    finally:
        proc.kill(); proc.wait()
    check("update: game running -> rc=1", p.returncode == 1 and "update-result: failed code=3 running" in p.stdout.decode("utf-8", "replace"))

    # 12. --doctor (offline checks; ids shared with plugins/doctor)
    def doctor(gd):
        p = subprocess.run([exe, "--doctor", "--game-dir", gd], capture_output=True, timeout=120)
        o = p.stdout.decode("utf-8", "replace")
        m = {}
        for l in o.splitlines():
            q = l.split(None, 4)
            if len(q) >= 4 and q[0] == "doctor:" and q[1].startswith("D"):
                m[q[1]] = (q[3], q[4] if len(q) > 4 else "")
        return p.returncode, o, m
    g8 = make_game(root, "doctor", header)
    B8 = os.path.join(g8, "binaries")
    rc, out, m = doctor(g8)
    check("doctor: not installed -> D002 PROBLEM, rc = number of PROBLEMs", m.get("D002", ("",))[0] == "PROBLEM"
          and rc == sum(v[0] == "PROBLEM" for v in m.values()), out)
    run(exe, "--game-dir", g8)
    rc, out, m = doctor(g8)
    ids = ["D001", "D002", "D003", "D009", "D010", "D012", "D013", "D014", "D015"]
    check("doctor: every offline check reports (D001 D002 D003 D009 D010 D012 D013 D014 D015)", all(i in m for i in ids), out)
    check("doctor: clean install -> D001/D002/D003/D012/D013/D014/D015 OK", all(m[i][0] == "OK" for i in ["D001", "D002", "D003", "D012", "D013", "D014", "D015"]), out)
    check("doctor: rc = number of PROBLEMs (disk/pagefile may vary)", rc == sum(v[0] == "PROBLEM" for v in m.values()), out)
    check("doctor: human lines have 'What to do' for non-OK and a result line",
          "doctor: result problems=" in out and all(("What to do:" in l) for l in out.splitlines() if l.startswith(("WARN", "PROBLEM"))), out)
    check("doctor: writes binaries\\v3x\\doctor_offline.txt", os.path.isfile(os.path.join(B8, "v3x", "doctor_offline.txt")))
    pl8 = os.path.join(B8, "v3x", "plugins")
    os.remove(os.path.join(pl8, "log_filter.dll"))
    open(os.path.join(pl8, "frame_pacer.dll"), "wb").write(b"junk")
    open(os.path.join(B8, "dxgi.dll"), "wb").write(b"MZ overlay")
    open(os.path.join(B8, "v3x", "loading.marker"), "w").write("keep_history")
    open(os.path.join(B8, "v3x", "crash.log"), "w").write("crash_logger started\nUNHANDLED EXCEPTION 0xc0000005 at fake_mod.dll+0x1234 - process is going down\n")
    vi8 = os.path.join(B8, "v3x", "v3x.ini")
    open(vi8, "a").write("bogus_plugin=1\nthis line is wrong\nlog_filter=1\n")
    rc, out, m = doctor(g8)
    check("doctor: missing + damaged plugin -> D002 PROBLEM naming both, repair=1",
          m["D002"][0] == "PROBLEM" and "missing=1" in m["D002"][1] and "changed=1" in m["D002"][1] and "repair=1" in out, out)
    check("doctor: dxgi.dll next to the exe -> D003 WARN", m["D003"][0] == "WARN" and "dxgi.dll" in m["D003"][1], out)
    check("doctor: loading.marker -> D012 PROBLEM plugin=keep_history", m["D012"] == ("PROBLEM", "plugin=keep_history"), str(m.get("D012")))
    check("doctor: crash.log -> D013 WARN module=fake_mod.dll", m["D013"][0] == "WARN" and "module=fake_mod.dll" in m["D013"][1], str(m.get("D013")))
    check("doctor: v3x.ini log_filter=1 without DLL -> D014 PROBLEM; unknown name + line without '=' reported",
          m["D014"][0] == "PROBLEM" and "enabled_missing=log_filter" in m["D014"][1] and "unknown=bogus_plugin" in m["D014"][1]
          and "no_equals=1" in m["D014"][1], str(m.get("D014")))
    check("doctor: rc = 3 PROBLEMs here (+ disk/pagefile if any)", rc == sum(v[0] == "PROBLEM" for v in m.values()) and rc >= 3, out)
    check("doctor: changes nothing (marker still there, plugin still missing)",
          os.path.isfile(os.path.join(B8, "v3x", "loading.marker")) and not os.path.exists(os.path.join(pl8, "log_filter.dll")))
    run(exe, "--repair", "--game-dir", g8)
    rc, out, m = doctor(g8)
    check("doctor: after Repair D002 OK and D014 has no missing DLL", m["D002"][0] == "OK" and "enabled_missing=none" in m["D014"][1], out)
    open(os.path.join(B8, "version.dll"), "wb").write(b"MZ another proxy")
    rc, out, m = doctor(g8)
    check("doctor: foreign version.dll -> D003 PROBLEM", m["D003"][0] == "PROBLEM" and "version.dll(not V3X)" in m["D003"][1], out)
    proc = subprocess.Popen([os.path.join(g5, "binaries", "victoria3.exe")])
    time.sleep(0.5)
    try:
        rc, out, m = doctor(g5)
    finally:
        proc.kill(); proc.wait()
    check("doctor: game running -> D015 WARN", m.get("D015", ("",))[0] == "WARN", out)

    fails = [n for n, ok in RESULTS if not ok]
    print("\n%d/%d passed%s" % (len(RESULTS) - len(fails), len(RESULTS), "" if not fails else "; FAILED: " + "; ".join(fails)))
    if not a.keep:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
