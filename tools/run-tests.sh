#!/usr/bin/env bash
# Builds the guest test libraries (tests/guest, armeabi-v7a) with the NDK and
# runs each through the Linux harness. Exits non-zero on any failure.
#   NDK=/path/to/android-ndk tools/run-tests.sh
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-/home/powmy/android-sdk/android-ndk-r30}
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC="$BIN/armv7a-linux-androideabi21-clang"
CXX="$BIN/armv7a-linux-androideabi21-clang++"
OUT=$ROOT/build/guest-tests
HARNESS=$ROOT/build/harness
mkdir -p "$OUT"
[ -x "$HARNESS" ] || { echo "build the harness first (ninja -C build harness)"; exit 2; }

build() { # name sources...
    local name=$1; shift
    local cc=$CC; [[ $1 == *.cpp ]] && cc="$CXX -static-libstdc++"
    $cc -shared -fPIC -O1 -o "$OUT/lib$name.so" "$@" || { echo "BUILD FAIL $name"; return 1; }
}
build thumbtest_helper "$ROOT/tests/guest/helper.c"

total_fail=0
for src in "$ROOT"/tests/guest/test_*.c "$ROOT"/tests/guest/test_*.cpp; do
    name=$(basename "${src%.*}")
    build "thumb$name" "$src" || { total_fail=$((total_fail + 1)); continue; }
    out=$(THUMB_TEST_DIR="$OUT" timeout 60 "$HARNESS" "$OUT/libthumb$name.so" 2>&1)
    result=$(grep -oE 'RESULT pass=[0-9]+ fail=[0-9]+' <<<"$out" | tail -1)
    if [[ $result =~ fail=0 ]]; then
        printf '  ok    %-14s %s\n' "$name" "$result"
    else
        printf '  FAIL  %-14s %s\n' "$name" "${result:-crashed}"
        grep -E '^FAIL|\[E\]' <<<"$out" | head -10 | sed 's/^/          /'
        total_fail=$((total_fail + 1))
    fi
done
[ $total_fail -eq 0 ] && echo "all guest tests passed" || echo "$total_fail test file(s) failed"
exit $(( total_fail > 0 ))
