#!/usr/bin/env bash
# Build hdcfwd for Linux and Windows (cross) in one shot.
#
# Windows cross-build compiler resolution order:
#   1. $MINGW_CXX environment variable
#   2. $OHOS_ROOT/prebuilts/mingw-w64/... (OpenHarmony source tree)
#   3. ../../.. relative path (when running inside the OpenHarmony tree)
#   4. x86_64-w64-mingw32-clang++ / x86_64-w64-mingw32-g++ from PATH
# If none is available the Windows build is skipped with a warning.
#
# Examples:
#   ./build.sh
#   MINGW_CXX=/path/to/x86_64-w64-mingw32-clang++ ./build.sh
#   OHOS_ROOT=/path/to/openharmony ./build.sh
set -e
cd "$(dirname "$0")"

echo "[1/2] building Linux binary (fully static, no glibc version dependency)..."
g++ -O2 -std=c++17 -Wall -Wextra -static -o hdcfwd hdcfwd.cpp -lpthread
echo "    -> $(pwd)/hdcfwd"

echo "[2/2] building Windows binary (cross)..."
MINGW="${MINGW_CXX:-}"
if [ -z "$MINGW" ]; then
    CANDS=()
    [ -n "$OHOS_ROOT" ] && CANDS+=("$OHOS_ROOT/prebuilts/mingw-w64/ohos/linux-x86_64/clang-mingw/bin/x86_64-w64-mingw32-clang++")
    IN_TREE="$(cd ../../.. 2>/dev/null && pwd)/prebuilts/mingw-w64/ohos/linux-x86_64/clang-mingw/bin/x86_64-w64-mingw32-clang++"
    CANDS+=("$IN_TREE")
    for c in "${CANDS[@]}"; do
        if [ -x "$c" ]; then
            MINGW="$c"
            break
        fi
    done
fi
if [ -z "$MINGW" ]; then
    if command -v x86_64-w64-mingw32-clang++ >/dev/null 2>&1; then
        MINGW="x86_64-w64-mingw32-clang++"
    elif command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        MINGW="x86_64-w64-mingw32-g++"
    fi
fi
if [ -z "$MINGW" ]; then
    echo "    !! no mingw cross compiler found (set MINGW_CXX or OHOS_ROOT), skip windows build" >&2
    echo "done (linux only)."
    exit 0
fi
"$MINGW" -O2 -std=c++17 -Wall -Wextra -o hdcfwd.exe hdcfwd.cpp -lws2_32 -liphlpapi -static -lpthread
echo "    -> $(pwd)/hdcfwd.exe"
echo "done."
