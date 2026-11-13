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

    python sdk/build_plugin.py check <plugin.dll> [--game-dir DIR] [--exe EXE] [--loader DLL]
                     [--unknown-build safe|load] [--json]
                     validates a built DLL WITHOUT loading it: x64 DLL, v3x_plugin_load export, manifest
                     (struct_size, min_api vs the loader's API version read from the installed
                     version.dll export v3x_loader_api, else include/v3x.h), game_builds vs the
                     installed victoria3.exe (game_version + build id) incl. REQUIRE_BUILD and the
                     loader's unknown-build gate, debug-CRT imports. Exit 0 = compatible, 1 = the
                     loader would refuse/skip it, 2 = usage. --json for installers.

Needs: pip install ziglang (not for check). Compiler caches go where zig puts them by default; set
ZIG_GLOBAL_CACHE_DIR / ZIG_LOCAL_CACHE_DIR / TEMP to move them (e.g. when C: is full).
Exit code: 0 = built (and test passed), 1 = compile or test failure, 2 = usage error.
Examples:
    python sdk/build_plugin.py check build/plugins/hello.dll
    python sdk/build_plugin.py templates/plugin_template --test
    python sdk/build_plugin.py plugins/hello --install
"""
import argparse
import glob
import json
import os
import re
import shlex
import shutil
import struct
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


# ---------------------------------------------------------------- check mode (no compiler needed)

class PE:
    """Minimal read-only PE32+ reader: sections, exports, imports, data at an RVA."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError("not a PE file (no MZ header)")
        o = struct.unpack_from("<I", d, 0x3C)[0]
        if d[o:o + 4] != b"PE\0\0":
            raise ValueError("not a PE file (no PE signature)")
        self.machine, nsec = struct.unpack_from("<HH", d, o + 4)
        self.timestamp = struct.unpack_from("<I", d, o + 8)[0]
        opt_size, self.characteristics = struct.unpack_from("<HH", d, o + 20)
        opt = o + 24
        self.magic = struct.unpack_from("<H", d, opt)[0]
        if self.magic != 0x20B:
            raise ValueError("not a 64-bit (PE32+) image")
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        s = opt + opt_size
        for i in range(nsec):
            name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, s + 8)
            self.sections.append((name, va, max(vsize, rsize), raw, rsize))
            s += 40

    def off(self, rva):
        for _name, va, size, raw, rsize in self.sections:
            if va <= rva < va + size:
                return raw + (rva - va) if rva - va < rsize else None
        return None

    def cstr(self, rva, limit=512):
        o = self.off(rva)
        if o is None:
            return None
        e = self.d.find(b"\0", o, o + limit)
        return self.d[o:e if e >= 0 else o + limit].decode("utf-8", "replace")

    def u32(self, rva):
        o = self.off(rva)
        return None if o is None else struct.unpack_from("<I", self.d, o)[0]

    def u64(self, rva):
        o = self.off(rva)
        return None if o is None else struct.unpack_from("<Q", self.d, o)[0]

    def ptr_str(self, va):
        """A string through an absolute pointer stored in the image (relative to the preferred base)."""
        if not va:
            return None
        return self.cstr(va - self.image_base)

    def exports(self):
        if len(self.dirs) < 1 or not self.dirs[0][0]:
            return {}
        rva = self.dirs[0][0]
        o = self.off(rva)
        if o is None:
            return {}
        nfun, nnames, afun, anames, aord = struct.unpack_from("<IIIII", self.d, o + 20)
        out = {}
        for i in range(nnames):
            nrva = self.u32(anames + 4 * i)
            o2 = self.off(aord + 2 * i)
            if nrva is None or o2 is None:
                continue
            ordinal = struct.unpack_from("<H", self.d, o2)[0]
            out[self.cstr(nrva)] = self.u32(afun + 4 * ordinal)
        return out

    def imports(self):
        if len(self.dirs) < 2 or not self.dirs[1][0]:
            return []
        out, rva = [], self.dirs[1][0]
        while True:
            o = self.off(rva)
            if o is None:
                break
            ilt, _ts, _fw, name, iat = struct.unpack_from("<IIIII", self.d, o)
            if not name:
                break
            out.append(self.cstr(name))
            rva += 20
        return out


def api_version_from_header():
    try:
        with open(os.path.join(REPO, "include", "v3x.h"), encoding="utf-8") as f:
            m = re.search(r"#define\s+V3X_API_VERSION\s+(\d+)", f.read())
        return int(m.group(1)) if m else None
    except OSError:
        return None


def build_list_matches(lst, version, build_id, strict=False):
    """Same rules as src/core.c build_matches_ex."""
    if not lst:
        return not strict
    for e in lst.split(";"):
        e = e.strip()
        if not e:
            continue
        if e.endswith("*"):
            p = e[:-1].lower()
            if version.lower().startswith(p) or build_id.lower().startswith(p):
                return True
        elif (not strict and e.lower() == version.lower()) or e.lower() == build_id.lower():
            return True
    return False


DEBUG_CRT = ("ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll", "msvcr120d.dll")


def check(argv):
    ap = argparse.ArgumentParser(prog="build_plugin.py check",
                                 description="Validate a built plugin DLL against the installed (or given) loader and game.")
    ap.add_argument("dll")
    ap.add_argument("--game-dir", help='the "Victoria 3" folder (default: env V3X_GAME_DIR, else the parent of this repo)')
    ap.add_argument("--exe", help="victoria3.exe to check game_builds against (default: <game>/binaries/victoria3.exe)")
    ap.add_argument("--loader", help="version.dll (V3X loader) to check min_api against (default: the installed one, "
                                     "else include/v3x.h)")
    ap.add_argument("--unknown-build", choices=("safe", "load"), default="safe",
                    help="the v3x.ini unknown_build setting to assume (default safe)")
    ap.add_argument("--json", action="store_true", help="print one JSON object instead of text")
    a = ap.parse_args(argv)
    res = {"dll": os.path.abspath(a.dll), "ok": [], "warn": [], "fail": [], "info": {}}
    ok, warn, fail, info = res["ok"].append, res["warn"].append, res["fail"].append, res["info"]

    try:
        pe = PE(a.dll)
    except (OSError, ValueError) as e:
        fail("cannot read %s: %s" % (a.dll, e))
        return report(res, a.json)
    if pe.machine != 0x8664:
        fail("machine 0x%x is not x64 (0x8664)" % pe.machine)
    if not pe.characteristics & 0x2000:
        fail("not a DLL (IMAGE_FILE_DLL not set)")
    ex = pe.exports()
    if "v3x_plugin_load" in ex:
        ok("exports v3x_plugin_load")
    else:
        fail("no v3x_plugin_load export: the loader skips this DLL (use V3X_EXPORT int v3x_plugin_load(const V3X_API *))")
    name = None
    if "v3x_plugin_name" in ex:
        name = pe.ptr_str(pe.u64(ex["v3x_plugin_name"]) or 0)
        info["name_export"] = name
    imports = [i.lower() for i in pe.imports() if i]
    info["imports"] = imports
    bad = [i for i in imports if i in DEBUG_CRT]
    if bad:
        warn("imports the debug CRT (%s): players do not have it; build a release DLL" % ", ".join(bad))

    # loader
    loader_api, loader_semver, known = None, None, None
    game = find_game_dir(a.game_dir)
    lpath = a.loader or (os.path.join(game, "binaries", "version.dll") if game else None)
    if lpath and os.path.isfile(lpath):
        try:
            lpe = PE(lpath)
            lex = lpe.exports()
            if "v3x_loader_api" in lex:
                loader_api = lpe.u32(lex["v3x_loader_api"])
                loader_semver = lpe.cstr(lex.get("v3x_loader_semver", 0) or 0)
                known = lpe.cstr(lex["v3x_loader_known_builds"]) if "v3x_loader_known_builds" in lex else None
                info["loader"] = "%s (api v%s, %s)" % (lpath, loader_api, loader_semver)
            elif "v3x_api" in lex:
                loader_api = 4
                info["loader"] = "%s (V3X before 0.5.0: api v4 or older, no version export)" % lpath
                warn("installed loader predates 0.5.0 (no v3x_loader_api export): assuming API v4; update it")
            else:
                warn("%s is not a V3X loader (no v3x_api export)" % lpath)
        except (OSError, ValueError) as e:
            warn("cannot read loader %s: %s" % (lpath, e))
    if loader_api is None:
        loader_api = api_version_from_header()
        info["loader"] = "include/v3x.h (api v%s; no installed loader found)" % loader_api
    info["loader_api"] = loader_api

    # game
    version, build_id = "unknown", None
    exe = a.exe or (os.path.join(game, "binaries", "victoria3.exe") if game else None)
    if exe and os.path.isfile(exe):
        try:
            gpe = PE(exe)
            build_id = "%08x-%x" % (gpe.timestamp, gpe.size_of_image)
            br = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(exe))), "caligula_branch.txt")
            if os.path.isfile(br):
                with open(br, encoding="utf-8", errors="replace") as f:
                    version = f.read().strip() or "unknown"
        except (OSError, ValueError) as e:
            warn("cannot read %s: %s" % (exe, e))
    info["game"] = "%s (build %s)" % (version, build_id) if build_id else "not found"

    # manifest
    mf_rva = ex.get("v3x_plugin_manifest")
    builds, flags, min_api = None, 0, 0
    if mf_rva is None:
        warn("no v3x_plugin_manifest: add V3X_PLUGIN_MANIFEST(name, version, author, description, game_builds) "
             "so players and the loader know which game builds it supports")
    else:
        size, flags, min_api, _res = [pe.u32(mf_rva + 4 * i) for i in range(4)]
        if size is None or size < 16:
            fail("bad v3x_plugin_manifest (struct_size %s): the loader refuses it" % size)
        else:
            fields = ("name", "version", "author", "description", "game_builds")
            vals = {}
            for i, fld in enumerate(fields):
                o = 16 + 8 * i
                vals[fld] = pe.ptr_str(pe.u64(mf_rva + o) or 0) if size >= o + 8 else None
            info["manifest"] = dict(vals, struct_size=size, flags=flags, min_api=min_api)
            name = vals["name"] or name
            builds = vals["game_builds"]
            ok("manifest: %s %s by %s, builds %s, min_api %d%s" % (
                vals["name"], vals["version"], vals["author"], builds or "(any)", min_api,
                ", REQUIRE_BUILD" if flags & 1 else ""))
            if not vals["version"]:
                warn("manifest has no version string")
    info["name"] = name or os.path.splitext(os.path.basename(a.dll))[0]

    if min_api and loader_api is not None and min_api > loader_api:
        fail("needs loader API v%d, the loader is v%d: it refuses the plugin (update version.dll)" % (min_api, loader_api))
    elif loader_api is not None:
        ok("API: needs v%d, loader has v%d" % (min_api, loader_api))

    if build_id:
        if build_list_matches(builds, version, build_id):
            ok("game build: %s / %s is in its list" % (version, build_id))
        elif flags & 1:
            fail("game %s / %s not in its game_builds and REQUIRE_BUILD is set: the loader refuses it" % (version, build_id))
        else:
            warn("game %s / %s not in its game_builds \"%s\": loads with a WARNING; anchors must fail closed" % (
                version, build_id, builds))
        if known is not None:
            k = [x.strip().lower() for x in known.split(";") if x.strip()]
            if build_id.lower() not in k:
                strict = mf_rva is not None and build_list_matches(builds, version, build_id, strict=True)
                if a.unknown_build == "load":
                    warn("game build %s is not known to the loader; unknown_build = load: loads anyway" % build_id)
                elif strict:
                    ok("game build %s is unknown to the loader, but the manifest declares it: loads in safe mode" % build_id)
                else:
                    fail("game build %s is unknown to the loader (tested: %s) and the manifest does not declare it by '*', "
                         "a prefix or the build id: SKIPPED in safe mode (unknown_build = load in v3x.ini to force)" % (
                             build_id, known))
    else:
        warn("victoria3.exe not found: game build not checked (--exe or --game-dir)")
    return report(res, a.json)


def report(res, as_json):
    if as_json:
        res["result"] = "fail" if res["fail"] else "ok"
        print(json.dumps(res, indent=1))
    else:
        print("check %s" % res["dll"])
        for k, v in sorted(res["info"].items()):
            if k not in ("manifest", "imports"):
                print("  %-12s %s" % (k, v))
        for m in res["ok"]:
            print("OK   " + m)
        for m in res["warn"]:
            print("WARN " + m)
        for m in res["fail"]:
            print("FAIL " + m)
        print("RESULT: %s (%d ok, %d warnings, %d failures)" % (
            "INCOMPATIBLE" if res["fail"] else "compatible", len(res["ok"]), len(res["warn"]), len(res["fail"])))
    return 1 if res["fail"] else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "check":
        return check(sys.argv[2:])
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
