<p align="center"><img src="docs/branding/thumb-icon-1024.png" width="160" alt="THUMB icon: a thumbs-up on a CPU chip"></p>

# THUMB 👍

**THUMB Helps Unsupported Mobile Binaries.**

Run old **32-bit-only Android apps and games** (armeabi-v7a) on modern **64-bit-only phones** (Pixel 8 and newer, …) — no root, no emulator.

The game's Java code runs normally on the phone. Its native 32-bit `.so` is replaced by a small 64-bit stub that loads the original library into a sandboxed 4 GB guest address space and runs it through a JIT (ARM32 → ARM64, via [dynarmic](https://github.com/azahar-emu/dynarmic)). Calls into the system — libc, OpenGL ES 1, zlib, JNI — are forwarded to the real 64-bit libraries through *thunks* that convert between the 32-bit and 64-bit ABIs.

Built gaming-first: the first app it ran was **Worms 3** (`com.worms3.app` 2.1). The name is a pun: *Thumb* is the 32-bit ARM instruction set most of these old apps are compiled to.

> ⚠️ This repository contains no game code or assets. You need your own copy of the APK.

## Status

**Worms 3 and ScummVM are playable on a Pixel 9a (GrapheneOS, Android 17)**: graphics, audio, touch. 🎉
**No PC needed:** the THUMB app patches, signs and installs apps on the phone and hands games their OBB data.

### Compatibility

| App | Version | Status | Notes |
|---|---|---|---|
| Worms 3 | 2.1 | ✅ Playable | GLES1, Java audio, self-unpacking `libgvradio` |
| ScummVM (SDL build) | 1.8.1 | ✅ Playable | 7 libraries, SDL 1.2, old-NDK stdio macros. Tested: Lure of the Temptress, with sound. Launcher menus need repeated taps (under investigation) |
| ScummVM (native build) | 1.8.1 | ✅ Playable | Needed two compat shims: virtual `/` listing, positioned asset descriptors. Tested: Lure of the Temptress |
| VLC | 2.0.6 | ❌ Not yet | Doctor: 66% (needs GLES2/EGL, more libc, sockets) |

| Milestone | State |
|---|---|
| ELF loader + JIT runs a game's constructors and `JNI_OnLoad` | ✅ |
| libc / zlib / JNI thunks, Linux test harness | ✅ |
| Raw syscalls, self-unpacking (packed) libraries, exported `Java_*` natives | ✅ |
| Android runtime + APK repackaging | ✅ |
| Playable on a phone | ✅ Worms 3 |
| THUMB app: import APK/bundle, patch, sign and install on the phone, OBB import | ✅ |
| Per-app options, in-game menu | ⏳ |
| More games and apps | ⏳ |

## Layout

```
core/       translator: memory arena, ELF loader, dynarmic CPU, thunks, JNI bridge
android/    THUMB app (Kotlin/Compose): Doctor, on-phone patching, signing, install, OBB import
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

## Tests

```sh
NDK=/path/to/android-ndk tools/run-tests.sh   # ARM32 test libraries run through the harness
```

## Building for Android

```sh
NDK=/path/to/android-ndk tools/build-android.sh     # -> build-android/libthumb.so, libthumb_stub.so
                                                    #    (--dev: developer conveniences, never release)
tools/repack.py original.apk patched.apk            # PC-side patching (developers)

cd android && ./gradlew assembleRelease             # the THUMB app (patches apps on the phone)
```

The THUMB app bundles the runtime from `build-android/` and the supported-function list
from `build/harness`, so build both first. Release builds refuse a `--dev` runtime.

## License

THUMB is free software under the **GNU General Public License v3.0** (see [`LICENSE`](LICENSE)).
Bundled third-party components and their licenses are listed in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Patch only apps you own. Never redistribute patched APKs or game data: THUMB ships the translator, not anyone's app.

## Credits

THUMB was designed and written with **Claude Opus 5.5** (Anthropic) as a pair programmer;
commits carry a `Co-Authored-By` line. The legal status of copyright in AI-assisted code
is still unsettled in some jurisdictions; the GPL-3.0 license applies to the extent the
code is copyrightable.

The JIT is [dynarmic](https://github.com/azahar-emu/dynarmic) by merryhime and contributors.
