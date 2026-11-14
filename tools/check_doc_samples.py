"""Compile every C code block of the documentation, so no guide teaches code that does not build.

    python tools/check_doc_samples.py [FILE.md ...] [--list] [--keep] [-j N]

Without FILE arguments it checks DEFAULT_FILES below (relative to the repo root). Every ```c block is
written to its own .c file under $V3X_TMP/doc_samples (else $TEMP/doc_samples, else
build/doc_samples) and compiled with

    python -m ziglang cc -target x86_64-windows-gnu -c -o <block>.o -Wall -Wextra -Werror
                         -Wno-unused-function -Iinclude -Isdk -Itest/fuzz

(-c -o instead of -fsyntax-only: zig cc 0.17.1 passes -fsyntax-only to clang and then fails renaming the
object file clang never wrote, so every block would "fail"; the object is deleted afterwards.
-Wno-unused-function because the SDK is header-only `static` functions, as in build_plugin.py; set
V3X_CC to use another compiler command). How a block is compiled is chosen by an HTML comment on the
line(s) right before its fence (blank lines in between are allowed):

    <!-- sample: complete -->          the block is a whole translation unit, compiled exactly as written
    <!-- sample: file -->              file-scope code: `#include "v3x_sdk.h"` + `#define TAG` are put first
    <!-- sample: body -->              statements: wrapped in `static int v3x_doc_sample_(const V3X_API *api)`
    <!-- sample: skip <reason> -->     not compiled (a header excerpt, deliberately broken code, pseudo code)
    <!-- sample: file | <C text> -->   ... with <C text> (declarations the fragment uses) put before the block

Without a marker the mode is guessed: a block with #include or a file-scope definition (a line
starting with static/V3X_EXPORT/V3X_PLUGIN_MANIFEST/typedef/int/void/struct at column 0) is `file`,
anything else `body`. Fragments (file/body) also get -Wno-unused-variable -Wno-unused-but-set-variable
-Wno-unused-parameter: an excerpt may declare more than it uses. `complete` blocks get no such relief.

Output: one "OK  : file:line (mode)" / "FAIL: file:line (mode)" + the compiler errors (with the line in
the .md file) per block, then a self-check (a broken block must fail), then "N checks, M failed".
Exit 0 = all compiled, 1 = failures, 2 = usage error. test/run_all.sh runs it as `t doc_samples`.
"""
import argparse
import concurrent.futures
import os
import re
import shlex
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FILES = [
    "README.md",
    "docs/getting-started.md",
    "docs/safe-coding.md",
    "docs/safety.md",
    "docs/reverse-engineering.md",
    "docs/engine-map.md",
    "docs/console-commands.md",
    "docs/faq.md",
    "docs/troubleshooting.md",
    "docs/api-reference.md",
    "docs/api-stability.md",
    "docs/configuration.md",
    "docs/plugins.md",
    "docs/scripting.md",
    "README.md",
]
FLAGS = ["-target", "x86_64-windows-gnu", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
FRAGMENT_FLAGS = ["-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-parameter"]
MARKER = re.compile(r"<!--\s*sample:\s*(complete|file|body|skip)\b\s*(?:\|?\s*(.*?))?\s*-->", re.S)
FILE_SCOPE = re.compile(r"^(#\s*include|static\b|V3X_EXPORT\b|V3X_PLUGIN_MANIFEST|V3X_FUZZ_|typedef\b|"
                        r"(const\s+)?(unsigned\s+)?(int|void|char|struct|V3xAnchor|V3X_API|uint\d+_t)\b)", re.M)
PRELUDE = '#include "v3x_sdk.h"\n#ifndef TAG\n#define TAG "doc_sample"\n#endif\n'


def compiler():
    cmd = os.environ.get("V3X_CC")
    return shlex.split(cmd, posix=(os.name != "nt")) if cmd else [sys.executable, "-m", "ziglang", "cc"]


def blocks(path):
    """yields (fence_line_1based, mode, extra, code) for every ```c block"""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"^(\s*)```c\s*$", lines[i])
        if not m:
            i += 1
            continue
        indent = len(m.group(1))
        start = i
        j = i + 1
        body = []
        while j < len(lines) and not re.match(r"^\s*```\s*$", lines[j]):
            body.append(lines[j][indent:] if lines[j][:indent].strip() == "" else lines[j])
            j += 1
        # marker: the nearest non-blank lines above the fence (a marker may span lines)
        k = start - 1
        while k >= 0 and not lines[k].strip():
            k -= 1
        above = []
        while k >= 0 and lines[k].strip() and len(above) < 6:
            above.insert(0, lines[k])
            if "<!--" in lines[k]:
                break
            k -= 1
        mm = MARKER.search("\n".join(above))
        mode, extra = (mm.group(1), (mm.group(2) or "").strip()) if mm else (None, "")
        code = "\n".join(body)
        if not mode:
            mode = "file" if FILE_SCOPE.search(code) else "body"
        yield start + 1, mode, extra, code
        i = j + 1


def source(mode, extra, code, md, line):
    """the .c text for one block"""
    if mode == "complete":
        head = ""
    elif mode == "file":
        head = PRELUDE + (extra + "\n" if extra else "")
    else:
        head = PRELUDE + "static int v3x_doc_sample_(const V3X_API *api)\n{\n    (void)api;\n" + \
               (extra + "\n" if extra else "")
    tail = "" if mode != "body" else "\n    return 0;\n}\n"
    # #line maps compiler messages to the .md file: line 1 of the block = fence line + 1
    return head + '#line %d "%s"\n' % (line + 1, md.replace("\\", "/")) + code + "\n" + tail


def compile_one(job):
    md, line, mode, extra, code, out_dir, cc = job
    name = "%s_%d.c" % (re.sub(r"[^A-Za-z0-9]+", "_", md), line)
    path = os.path.join(out_dir, name)
    text = source(mode, extra, code, md, line)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    flags = FLAGS + ([] if mode == "complete" else FRAGMENT_FLAGS)
    inc = [os.path.join(REPO, d) for d in ("include", "sdk", os.path.join("test", "fuzz"))]   # fuzz: v3x_fuzz.h
    obj = path[:-2] + ".o"
    cmd = cc + flags + ["-I" + d for d in inc] + ["-c", "-o", obj, path]
    # own zig local cache per block: parallel zig processes sharing one race on its tmp/ renames
    env = dict(os.environ, ZIG_LOCAL_CACHE_DIR=os.path.join(out_dir, "zc", name[:-2]))
    p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, errors="replace", env=env)
    if os.path.exists(obj):
        os.remove(obj)
    shutil.rmtree(env["ZIG_LOCAL_CACHE_DIR"], ignore_errors=True)
    return md, line, mode, p.returncode, (p.stdout + p.stderr).strip(), path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--list", action="store_true", help="list the blocks and their modes, compile nothing")
    ap.add_argument("--keep", action="store_true", help="keep the generated .c files")
    ap.add_argument("-j", type=int, default=min(8, os.cpu_count() or 2))
    a = ap.parse_args()
    files = a.files or DEFAULT_FILES
    tmp = os.environ.get("V3X_TMP") or os.environ.get("TEMP")   # run_all.sh exports TEMP = its scratch root
    out_dir = os.path.join(tmp, "doc_samples") if tmp else os.path.join(REPO, "build", "doc_samples")
    os.makedirs(out_dir, exist_ok=True)
    cc = compiler()
    jobs, skipped = [], 0
    for rel in files:
        path = rel if os.path.isabs(rel) else os.path.join(REPO, rel)
        if not os.path.isfile(path):
            if a.files:
                print("check_doc_samples: no such file: %s" % rel, file=sys.stderr)
                return 2
            continue   # a default file that does not exist (yet)
        md = os.path.relpath(path, REPO).replace("\\", "/")
        for line, mode, extra, code in blocks(path):
            if a.list:
                print("%s:%d %s" % (md, line, mode))
                continue
            if mode == "skip":
                skipped += 1
                print("skip: %s:%d" % (md, line))
                continue
            jobs.append((md, line, mode, extra, code, out_dir, cc))
    if a.list:
        return 0
    # self-check: the harness must reject a broken block (else every OK above means nothing)
    jobs.append(("<selfcheck>", 1, "body", "", "int x = undeclared_identifier_;", out_dir, cc))
    checks = failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.j)) as ex:
        for md, line, mode, rc, msg, path in ex.map(compile_one, jobs):
            checks += 1
            if md == "<selfcheck>":
                ok = rc != 0 and "undeclared_identifier_" in msg   # rejected for the right reason
                print("%s: self-check: a broken sample is rejected" % ("OK  " if ok else "FAIL"))
            else:
                ok = rc == 0
                print("%s: %s:%d (%s)" % ("OK  " if ok else "FAIL", md, line, mode))
                if not ok:
                    print("      " + msg.replace("\n", "\n      ")[:4000])
                    print("      generated: %s" % path)
            failed += not ok
            if ok and not a.keep:
                try:
                    os.remove(path)
                except OSError:
                    pass
    print("%d checks, %d failed (%d skipped)" % (checks, failed, skipped))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
