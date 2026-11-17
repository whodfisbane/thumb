# Sourced by the build scripts: finds the pinned Android NDK or fails loudly.
# Uses $NDK if set, else $ANDROID_SDK_ROOT (or $ANDROID_HOME)/ndk/$NDK_VERSION.
# Keep NDK_VERSION in sync with android/app/build.gradle.kts and .github/workflows/ci.yml.
NDK_VERSION=30.0.16248370
if [ -z "${NDK:-}" ]; then
    _sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}
    if [ -n "$_sdk" ] && [ -d "$_sdk/ndk/$NDK_VERSION" ]; then NDK=$_sdk/ndk/$NDK_VERSION; fi
fi
if [ -z "${NDK:-}" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "error: Android NDK $NDK_VERSION not found (NDK=${NDK:-unset})." >&2
    echo "       Set NDK=/path/to/android-ndk, or ANDROID_SDK_ROOT with ndk/$NDK_VERSION installed." >&2
    exit 2
fi
_rev=$(sed -n 's/^Pkg\.Revision *= *//p' "$NDK/source.properties" 2>/dev/null)
[ "$_rev" = "$NDK_VERSION" ] || echo "warning: NDK at $NDK is ${_rev:-unknown}, THUMB is built with $NDK_VERSION" >&2
export NDK
