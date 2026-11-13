"""One-command build for V3X plugins (zig cc; no Visual Studio needed).

    python sdk/build_plugin.py <plugin_dir> [options]

    <plugin_dir>     folder with the plugin's .c files (test_*.c / *_test.c are the offline test)
    --name NAME      DLL name (default: the .c file named like the folder, else the only .c file,
                     else the folder name)
    --out DIR        output folder (default: <repo>/build/plugins)
    --test [EXE]     also build and run the offline test; EXE = victoria3.exe to check anchors
                     against (default: <game>/binaries/victoria3.exe when found)
    --install        copy the DLL to <game>/binaries/v3x/plugins/ (and NAME.ini.example next to
                     the plugin inis in <game>/binaries/v3x/; an existing NAME.ini is never touched)
    --game-dir DIR   the "Victoria 3" folder (default: env V3X_GAME_DIR, else the parent of this
                     repo if it contains binaries/victoria3.exe)
    --cc "CMD"       compiler command (default: env V3X_CC, else "<python> -m ziglang cc")
    --debug          -O0 -g instead of -O2

Needs: pip install ziglang. Compiler caches go where zig puts them by default; set
ZIG_GLOBAL_CACHE_DIR / ZIG_LOCAL_CACHE_DIR / TEMP to move them (e.g. when C: is full).
Exit code: 0 = built (and test passed), 1 = compile or test failure, 2 = usage error.
Examples:
    python sdk/build_plugin.py templates/plugin_template --test
    python sdk/build_plugin.py plugins/hello --install
"""
import argparse
import glob
import os
import shlex
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def is_test(path):
    b = os.path.basename(path)
    return b.startswith("test_") or b.endswith("_test.c")


def find_game_dir(arg):
    for cand in (arg, os.environ.get("V3X_GAME_DIR"), os.path.dirname(REPO)):
        if cand and os.path.isfile(os.path.join(cand, "binaries", "victoria3.exe")):
            return os.path.abspath(cand)
    return None


def compiler(arg):
    cmd = arg or os.environ.get("V3X_CC")
    if cmd:
        return shlex.split(cmd, posix=(os.name != "nt"))
    return [sys.executable, "-m", "ziglang", "cc"]


def run(cmd):
    print("+ " + " ".join(cmd))
    return subprocess.call(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("plugin_dir")
    ap.add_argument("--name")
    ap.add_argument("--out")
    ap.add_argument("--test", nargs="?", const="", default=None, metavar="EXE")
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--game-dir")
    ap.add_argument("--cc")
    ap.add_argument("--debug", action="store_true")
    a = ap.parse_args()

    pdir = os.path.abspath(a.plugin_dir)
    srcs = sorted(p for p in glob.glob(os.path.join(pdir, "*.c")) if not is_test(p))
    tests = sorted(p for p in glob.glob(os.path.join(pdir, "*.c")) if is_test(p))
    if not srcs:
        print("no .c files in %s" % pdir)
        return 2
    stems = [os.path.splitext(os.path.basename(p))[0] for p in srcs]
    folder = os.path.basename(pdir.rstrip("\\/"))
    name = a.name or (folder if folder in stems else stems[0] if len(stems) == 1 else folder)
    out = os.path.abspath(a.out or os.path.join(REPO, "build", "plugins"))
    os.makedirs(out, exist_ok=True)
    dll = os.path.join(out, name + ".dll")

    cc = compiler(a.cc) + ["-target", "x86_64-windows-gnu"]
    cc += ["-O0", "-g"] if a.debug else ["-O2"]
    cc += ["-Wall", "-Wno-unused-function", "-I" + os.path.join(REPO, "include"), "-I" + os.path.join(REPO, "sdk"),
           "-I" + pdir]
    if run(cc + ["-shared", "-o", dll] + srcs):
        print("BUILD FAILED")
        return 1
    for junk in (".lib", ".pdb"):
        j = os.path.splitext(dll)[0] + junk
        if os.path.exists(j) and not a.debug:
            os.remove(j)
    print("built %s" % dll)

    game = find_game_dir(a.game_dir)
    if a.test is not None:
        if not tests:
            print("no test_*.c / *_test.c in %s" % pdir)
            return 1
        tdir = os.path.join(REPO, "build", "tests")
        os.makedirs(tdir, exist_ok=True)
        exe = os.path.join(tdir, name + "_test.exe")
        # the test #includes the plugin source itself, so only the test files are compiled
        if run(cc + ["-o", exe] + tests):
            print("TEST BUILD FAILED")
            return 1
        game_exe = a.test or (os.path.join(game, "binaries", "victoria3.exe") if game else "")
        rc = run([exe] + ([game_exe] if game_exe else []))
        print("test %s" % ("passed" if rc == 0 else "FAILED (exit %d)" % rc))
        if rc:
            return 1

    if a.install:
        if not game:
            print("--install: game folder not found; pass --game-dir or set V3X_GAME_DIR")
            return 2
        v3x = os.path.join(game, "binaries", "v3x")
        os.makedirs(os.path.join(v3x, "plugins"), exist_ok=True)
        shutil.copy2(dll, os.path.join(v3x, "plugins"))
        ex = os.path.join(pdir, name + ".ini.example")
        if os.path.exists(ex):
            shutil.copy2(ex, v3x)
        if not os.path.exists(os.path.join(game, "binaries", "version.dll")):
            print("note: binaries/version.dll (the V3X loader) is not installed; run ./build.sh install")
        print("installed into %s" % os.path.join(v3x, "plugins"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
