#!/usr/bin/env bash
# Builds V3X with zig (pip install ziglang). Output: build/version.dll + build/plugins/*.dll
# Usage: ./build.sh            build
#        ./build.sh install    build and copy into ../binaries (the game)
#        ./build.sh dist       build and make build/dist/V3X-Setup.exe (installer/make_dist.py)
set -euo pipefail
cd "$(dirname "$0")"
CC="python -m ziglang cc -target x86_64-windows-gnu -O2 -Wall -Wno-unused-function"
MH=third_party/minhook
mkdir -p build/plugins
# -fms-extensions: the core uses SEH __try/__except to isolate faults in plugin callbacks
$CC -fms-extensions -shared -o build/version.dll -I$MH/include src/core.c src/proxy.c src/safe_hook.c \
    $MH/src/buffer.c $MH/src/hook.c $MH/src/trampoline.c $MH/src/hde/hde64.c -lkernel32 -luser32
for p in plugins/*/; do
    n=$(basename "$p")
    compgen -G "$p*.c" > /dev/null || { echo "skip $n (no .c files yet)"; continue; }
    $CC -shared -Iinclude -Isdk -o "build/plugins/$n.dll" "$p"*.c
done
rm -f build/*.lib build/*.pdb build/plugins/*.lib build/plugins/*.pdb
echo "built: $(ls build build/plugins | tr '\n' ' ')"
if [[ "${1:-}" == dist ]]; then   # build + build/dist/V3X-Setup.exe (installer/make_dist.py)
    python installer/make_dist.py --no-build
fi
if [[ "${1:-}" == install ]]; then
    mkdir -p ../binaries/v3x/plugins
    cp build/version.dll ../binaries/version.dll
    cp build/plugins/*.dll ../binaries/v3x/plugins/
    echo "installed into ../binaries"
fi
