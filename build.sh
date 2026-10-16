#!/usr/bin/env bash
# Build hdcfwd for Linux and Windows (cross) in one shot.
# Usage: ./build.sh [mingw-clang++-path]
set -e
cd "$(dirname "$0")"

OUT_DIR="${1:-.}"

echo "[1/2] building Linux binary..."
g++ -O2 -std=c++17 -Wall -Wextra -o "$OUT_DIR/hdcfwd" hdcfwd.cpp -lpthread
echo "    -> $OUT_DIR/hdcfwd"

echo "[2/2] building Windows binary (cross)..."
MINGW="${MINGW_CXX:-}"
if [ -z "$MINGW" ]; then
    # try repo-bundled mingw toolchain first, then PATH
    ROOT="$(cd ../../.. && pwd)"
    CAND="$ROOT/prebuilts/mingw-w64/ohos/linux-x86_64/clang-mingw/bin/x86_64-w64-mingw32-clang++"
    if [ -x "$CAND" ]; then
        MINGW="$CAND"
    elif command -v x86_64-w64-mingw32-clang++ >/dev/null 2>&1; then
        MINGW="x86_64-w64-mingw32-clang++"
    elif command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1; then
        MINGW="x86_64-w64-mingw32-g++"
    else
        echo "    !! no mingw cross compiler found, skip windows build" >&2
        exit 0
    fi
fi
"$MINGW" -O2 -std=c++17 -Wall -Wextra -o "$OUT_DIR/hdcfwd.exe" hdcfwd.cpp -lws2_32 -static -lpthread
echo "    -> $OUT_DIR/hdcfwd.exe"
echo "done."
