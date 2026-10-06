# arm32-revive

Run old **32-bit-only Android games** (armeabi-v7a) on modern **64-bit-only phones** (Pixel 8 and newer, …) — no root, no emulator.

The game's Java code runs normally on the phone. Its native 32-bit `.so` is replaced by a small 64-bit stub that loads the original library into a sandboxed 4 GB guest address space and runs it through a JIT (ARM32 → ARM64, via [dynarmic](https://github.com/azahar-emu/dynarmic)). Calls into the system — libc, OpenGL ES 1, zlib, JNI — are forwarded to the real 64-bit libraries through *thunks* that convert between the 32-bit and 64-bit ABIs.

First target: **Worms 3** (`com.worms3.app` 2.1).

> ⚠️ This repository contains no game code or assets. You need your own copy of the APK.

## Status

| Milestone | State |
|---|---|
| ELF loader + JIT runs a game's constructors and `JNI_OnLoad` | ✅ (Worms 3: 678 constructors, 45 natives) |
| libc / zlib / JNI thunks; game boots and renders frames in the Linux harness | 🟡 (600 frames, text rendering WIP) |
| Android runtime + APK repackaging | 🚧 |
| Playable on a phone | ⏳ |

## Layout

```
core/       translator: memory arena, ELF loader, dynarmic CPU, thunks, JNI bridge
harness/    Linux test runner with a fake JVM
shim/       Android runtime (libarm32revive.so) and per-library stub
tools/      build and code-generation scripts
```

## Building (Linux harness)

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja && ninja -C build harness
./build/harness path/to/libSomething.so
```

Requires Boost headers and libffi. For Android: `NDK=/path/to/ndk tools/build-android.sh`.
