"""Test of `sdk/build_plugin.py check` (plugin DLL validation without loading it).

    python test/compat_check.py <T>     T = scratch dir where test/run_all.sh built $T/api_v4_test

Uses the manifest test plugins of test/api_v4_test (v4_ok, v4_minapi, v4_require), build/version.dll
(API v5 loader with the v3x_loader_* exports), build/plugins/*.dll and the real victoria3.exe when it is
next to the repo. A test exe stands in for "an unknown game build". Exit code 0 = ALL OK.
"""
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, "sdk", "build_plugin.py")
checks = fails = 0


def check(cond, msg):
    global checks, fails
    checks += 1
    fails += not cond
    print("%s: %s" % ("OK  " if cond else "FAIL", msg))


def run(*args):
    p = subprocess.run([sys.executable, TOOL, "check", "--json"] + list(args), capture_output=True, text=True)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        print(p.stdout, p.stderr)
        return p.returncode, {"fail": ["unparsable"], "ok": [], "warn": [], "info": {}}


def has(lst, text):
    return any(text in m for m in lst)


def main():
    t = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TEMP", "."), "v3x", "t")
    plug = os.path.join(t, "api_v4_test", "v3x", "plugins")
    loader = os.path.join(REPO, "build", "version.dll")
    game_exe = os.path.join(os.path.dirname(REPO), "binaries", "victoria3.exe")
    unknown_exe = os.path.join(t, "api_v4_test", "api_v4_test.exe")   # any x64 exe = a build the loader does not know

    rc, r = run(os.path.join(plug, "v4_ok.dll"), "--loader", loader, "--exe", unknown_exe)
    check(rc == 0 and r["result"] == "ok" and r["info"]["loader_api"] == 5, "v4_ok ('*' builds) compatible with the v5 loader")
    check(r["info"].get("manifest", {}).get("version") == "2.3.4" and r["info"]["name"] == "v4_ok", "manifest fields read from the file")
    check(has(r["ok"], "unknown to the loader, but the manifest declares it"), "'*' passes the unknown-build gate")

    rc, r = run(os.path.join(plug, "v4_minapi.dll"), "--loader", loader, "--exe", unknown_exe)
    check(rc == 1 and has(r["fail"], "needs loader API v99"), "min_api 99 -> incompatible (exit 1)")

    rc, r = run(os.path.join(plug, "v4_require.dll"), "--loader", loader, "--exe", unknown_exe)
    check(rc == 1 and has(r["fail"], "REQUIRE_BUILD"), "REQUIRE_BUILD with a non-matching game -> incompatible")

    lg = os.path.join(REPO, "build", "plugins", "lazy_gfx.dll")
    rc, r = run(lg, "--loader", loader, "--exe", unknown_exe)
    check(rc == 1 and has(r["fail"], "SKIPPED in safe mode") and has(r["warn"], "not in its game_builds"),
          "release/1.15.1 plugin on an unknown build: skipped in safe mode")
    rc, r = run(lg, "--loader", loader, "--exe", unknown_exe, "--unknown-build", "load")
    check(rc == 0 and has(r["warn"], "unknown_build = load"), "same with --unknown-build load: compatible with warnings")

    if os.path.isfile(game_exe):
        rc, r = run(lg, "--loader", loader, "--exe", game_exe)
        check(rc == 0 and has(r["ok"], "is in its list") and not r["warn"], "lazy_gfx on the installed 1.15.1 exe: clean")
    else:
        print("(victoria3.exe not found: installed-game check skipped)")

    rc, r = run(loader, "--loader", loader)
    check(rc == 1 and has(r["fail"], "no v3x_plugin_load export"), "version.dll itself is not a plugin")
    rc, r = run(os.path.join(REPO, "README.md"))
    check(rc == 1 and has(r["fail"], "not a PE file"), "non-PE file rejected")
    rc, r = run(os.path.join(REPO, "build", "plugins", "hello.dll"), "--loader", os.path.join(t, "oldloader", "version.dll"))
    if os.path.isfile(os.path.join(t, "oldloader", "version.dll")):
        check(r["info"]["loader_api"] == 4 and has(r["warn"], "predates 0.5.0"), "a pre-0.5.0 loader is recognised as API v4")

    p = subprocess.run([sys.executable, TOOL, "check", os.path.join(plug, "v4_ok.dll"), "--loader", loader],
                       capture_output=True, text=True)
    check(p.returncode == 0 and "RESULT: compatible" in p.stdout, "text output ends with RESULT line")
    print("%d checks, %d failed\n%s" % (checks, fails, "FAILED" if fails else "ALL OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
