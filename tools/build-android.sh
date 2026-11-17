#!/usr/bin/env bash
# Cross-compiles the translator for arm64 Android.
#   NDK=/path/to/android-ndk tools/build-android.sh [--dev]   (or ANDROID_SDK_ROOT, see ndk-env.sh)
# Output: build-android/libthumb.so and build-android/libthumb_stub.so
#   --dev  enable developer conveniences (OBB fetch over adb reverse). Never release these.
set -euo pipefail
DEV=OFF
[ "${1:-}" = "--dev" ] && DEV=ON
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tools/ndk-env.sh"
API=29
OUT=$ROOT/build-android
DEPS=$OUT/deps
TOOLS=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p "$DEPS/include"

# Boost is header-only for dynarmic; expose just the boost/ headers.
[ -e "$DEPS/include/boost" ] || ln -s /usr/include/boost "$DEPS/include/boost"

# libffi (static, for the Java -> guest native trampolines)
FFI_VER=3.8.0
FFI_SHA256=7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4
if [ ! -d "$ROOT/third_party/libffi-$FFI_VER" ]; then
    tarball=$(mktemp)
    curl -sfL -o "$tarball" "https://github.com/libffi/libffi/releases/download/v$FFI_VER/libffi-$FFI_VER.tar.gz"
    if ! echo "$FFI_SHA256  $tarball" | sha256sum -c --quiet; then
        echo "error: libffi-$FFI_VER.tar.gz does not match the pinned sha256" >&2
        rm -f "$tarball"
        exit 1
    fi
    tar xzf "$tarball" -C "$ROOT/third_party"
    rm -f "$tarball"
fi
if [ ! -f "$DEPS/lib/libffi.a" ]; then
    mkdir -p "$OUT/libffi"
    (cd "$OUT/libffi" && \
     CC="$TOOLS/aarch64-linux-android$API-clang" AR="$TOOLS/llvm-ar" RANLIB="$TOOLS/llvm-ranlib" \
     CFLAGS="-O2 -fPIC" \
     "$ROOT/third_party/libffi-$FFI_VER/configure" --host=aarch64-linux-android --prefix="$DEPS" \
         --disable-shared --enable-static --disable-docs --disable-multi-os-directory >/dev/null && \
     make -j"$(nproc)" >/dev/null && make install >/dev/null)
fi

PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" PKG_CONFIG_PATH="" \
cmake -S "$ROOT" -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DTHUMB_DEV=$DEV \
    -DCMAKE_CXX_FLAGS="-include cstdlib" \
    -DCMAKE_FIND_ROOT_PATH="$DEPS" -DBOOST_ROOT="$DEPS" -DBoost_INCLUDE_DIR="$DEPS/include" >/dev/null
ninja -C "$OUT" thumb thumb_stub
