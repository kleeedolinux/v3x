#!/usr/bin/env bash
# Builds V3X with zig (pip install ziglang). Output: build/version.dll + build/plugins/*.dll
# Usage: ./build.sh            build
#        ./build.sh install    build and copy into ../binaries (the game)
#        ./build.sh dist       build and make build/dist/V3X-Setup.exe (installer/make_dist.py)
#
# Hardening (docs/security-audit.md action 14, docs/adr/0004-sdk-safety.md): every target gets
# V3X_HARDEN (stack protector, fortify, zero-initialised locals, no NULL-check deletion) and the
# warning set V3X_WARN. The core (src/) and the SDK headers (include/, sdk/) build with -Werror.
# Plugins get -Werror only with V3X_WERROR=1 (CI turns it on once every plugin is clean).
# Env: V3X_WERROR=1   -Werror for plugins too
#      V3X_NO_WERROR=1 no -Werror anywhere (local experiments only)
#      V3X_SANITIZE=1  add -fsanitize=undefined -fsanitize-trap=undefined (no runtime; a UB trap = crash)
set -euo pipefail
cd "$(dirname "$0")"
ZCC="python -m ziglang cc -target x86_64-windows-gnu"
V3X_HARDEN="-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ftrivial-auto-var-init=zero -fno-delete-null-pointer-checks"
V3X_WARN="-Wall -Wextra -Wformat=2 -Wvla -Wshadow -Wno-unused-function"
SAN=""
[[ "${V3X_SANITIZE:-}" == 1 ]] && SAN="-fsanitize=undefined -fsanitize-trap=undefined"
WERR_CORE="-Werror"; WERR_PLUG=""
[[ "${V3X_WERROR:-}" == 1 ]] && WERR_PLUG="-Werror"
[[ "${V3X_NO_WERROR:-}" == 1 ]] && { WERR_CORE=""; WERR_PLUG=""; }
CC="$ZCC $V3X_HARDEN $SAN"
MH=third_party/minhook
mkdir -p build/plugins build/obj

# SDK headers: every public header must compile warning-free as C and as C++ (-Werror)
printf '#include "v3x_sdk.h"\n#include "v3x_safe.h"\n#include "v3x_testkit.h"\n#include "v3x_script.h"\nint main(void){return 0;}\n' > build/obj/sdk_check.c
cp build/obj/sdk_check.c build/obj/sdk_check.cpp
$CC $V3X_WARN $WERR_CORE -Iinclude -Isdk -c -o build/obj/sdk_check.o build/obj/sdk_check.c
python -m ziglang c++ -target x86_64-windows-gnu $V3X_HARDEN $V3X_WARN $WERR_CORE -Iinclude -Isdk -c -o build/obj/sdk_check_cxx.o build/obj/sdk_check.cpp
# v3x_safe.h is also -Wconversion clean
$CC $V3X_WARN -Wconversion -Wsign-conversion $WERR_CORE -Iinclude -Isdk -c -o build/obj/safe_check.o -x c - <<< '#include "v3x_safe.h"'

# MinHook (third party): hardened, its own warnings are not ours to fail on
MHOBJ=()
for f in buffer hook trampoline hde/hde64; do
    o=build/obj/mh_$(basename "$f").o
    $CC -w -I$MH/include -c -o "$o" "$MH/src/$f.c"
    MHOBJ+=("$o")
done
# -fms-extensions: the core uses SEH __try/__except to isolate faults in plugin callbacks
$CC $V3X_WARN $WERR_CORE -fms-extensions -shared -o build/version.dll -I$MH/include src/core.c src/proxy.c src/safe_hook.c \
    "${MHOBJ[@]}" -lkernel32 -luser32
for p in plugins/*/; do
    n=$(basename "$p")
    compgen -G "$p*.c" > /dev/null || { echo "skip $n (no .c files yet)"; continue; }
    $CC $V3X_WARN $WERR_PLUG -Iinclude -Isdk -o "build/plugins/$n.dll" -shared "$p"*.c
done
rm -f build/*.lib build/*.pdb build/plugins/*.lib build/plugins/*.pdb
echo "built: $(ls build build/plugins | grep -v '^obj$' | tr '\n' ' ')"
if [[ "${1:-}" == dist ]]; then   # build + build/dist/V3X-Setup.exe (installer/make_dist.py)
    python installer/make_dist.py --no-build
fi
if [[ "${1:-}" == install ]]; then
    mkdir -p ../binaries/v3x/plugins
    cp build/version.dll ../binaries/version.dll
    cp build/plugins/*.dll ../binaries/v3x/plugins/
    echo "installed into ../binaries"
fi
