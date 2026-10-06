# THUMB 👍

**THUMB Helps Unsupported Mobile Binaries.**

Run old **32-bit-only Android apps and games** (armeabi-v7a) on modern **64-bit-only phones** (Pixel 8 and newer, …) — no root, no emulator.

The game's Java code runs normally on the phone. Its native 32-bit `.so` is replaced by a small 64-bit stub that loads the original library into a sandboxed 4 GB guest address space and runs it through a JIT (ARM32 → ARM64, via [dynarmic](https://github.com/azahar-emu/dynarmic)). Calls into the system — libc, OpenGL ES 1, zlib, JNI — are forwarded to the real 64-bit libraries through *thunks* that convert between the 32-bit and 64-bit ABIs.

Built gaming-first: the first app it ran was **Worms 3** (`com.worms3.app` 2.1). The name is a pun: *Thumb* is the 32-bit ARM instruction set most of these old apps are compiled to.

> ⚠️ This repository contains no game code or assets. You need your own copy of the APK.

## Status

**Worms 3 is playable on a Pixel 9a (GrapheneOS, Android 17)**: graphics, audio, touch, 60 fps. 🎉

| Milestone | State |
|---|---|
| ELF loader + JIT runs a game's constructors and `JNI_OnLoad` | ✅ |
| libc / zlib / JNI thunks, Linux test harness | ✅ |
| Raw syscalls, self-unpacking (packed) libraries, exported `Java_*` natives | ✅ |
| Android runtime + APK repackaging | ✅ |
| Playable on a phone | ✅ Worms 3 |
| THUMB app (import APK/OBB on the phone, per-game options) | ⏳ |
| More games and apps | ⏳ |

## Layout

```
core/       translator: memory arena, ELF loader, dynarmic CPU, thunks, JNI bridge
harness/    Linux test runner with a fake JVM
shim/       Android runtime (libthumb.so) and per-library stub
tools/      build and code-generation scripts
```

## Building (Linux harness)

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja && ninja -C build harness
./build/harness path/to/libSomething.so
```

Requires Boost headers and libffi.

## Building for Android

```sh
NDK=/path/to/android-ndk tools/build-android.sh     # -> build-android/libthumb.so, libthumb_stub.so
tools/repack.py original.apk patched.apk            # swap 32-bit libs for THUMB, re-sign
```
