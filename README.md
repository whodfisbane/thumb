<p align="center"><img src="docs/branding/thumb-icon-1024.png" width="160" alt="THUMB icon: a thumbs-up on a CPU chip"></p>

# THUMB 👍

**THUMB Helps Unsupported Mobile Binaries.**

Run old **32-bit-only Android apps and games** (armeabi-v7a) on modern **64-bit-only phones** (Pixel 8 and newer, …) — no root, no emulator.

The game's Java code runs normally on the phone. Its native 32-bit `.so` is replaced by a small 64-bit stub that loads the original library into a sandboxed 4 GB guest address space and runs it through a JIT (ARM32 → ARM64, via [dynarmic](https://github.com/azahar-emu/dynarmic)). Calls into the system — libc, OpenGL ES 1, zlib, JNI — are forwarded to the real 64-bit libraries through *thunks* that convert between the 32-bit and 64-bit ABIs.

Built gaming-first: the first app it ran was **Worms 3** (`com.worms3.app` 2.1). It also works for
old non-game apps. The name is a pun: *Thumb* is the 32-bit ARM instruction set most of these old
apps are compiled to.

> ⚠️ This repository contains no game code or assets. You need your own copy of the APK.

## Why

Since the Pixel 7 (and on more and more phones), Android devices ship **without 32-bit CPU support**.
Thousands of older apps and games only ever shipped 32-bit native code and were never updated, so
they simply refuse to install. There's no source code to recompile, and full Android emulators are
heavy. THUMB translates just the native part and lets everything else run as a normal app.

## Status

**Worms 3, ScummVM and VLC run on a Pixel 9a (GrapheneOS, Android 17)**: graphics, audio, touch, hardware video decoding. 🎉
Tested on: Pixel 9a, GrapheneOS, Android 17 (API 37), arm64-only.

### Compatibility

| App | Version | Status | Notes |
|---|---|---|---|
| Worms 3 | 2.1 | ✅ Playable | GLES1, Java audio, self-unpacking `libgvradio` |
| ScummVM (SDL build) | 1.8.1 | ✅ Playable | 7 libraries, SDL 1.2, old-NDK stdio macros. Tested: Lure of the Temptress, with sound. Launcher menus need repeated taps (under investigation) |
| ScummVM (native build) | 1.8.1 | ✅ Playable | Needed two compat shims: virtual `/` listing, positioned asset descriptors. Tested: Lure of the Temptress |
| VLC | 2.0.6 | ✅ Plays video | Hardware decoding through AMediaCodec, futex, sockets, pthread cleanup handlers. Its optional `libiomx`/`libanw` plugins need private Android libraries and are skipped (VLC falls back by itself) |

| Milestone | State |
|---|---|
| ELF loader + JIT runs a game's constructors and `JNI_OnLoad` | ✅ |
| libc / zlib / JNI thunks, Linux test harness | ✅ |
| Raw syscalls, self-unpacking (packed) libraries, exported `Java_*` natives | ✅ |
| Android runtime + APK repackaging | ✅ |
| Playable on a phone | ✅ Worms 3 |
| THUMB app: import APK/bundle, patch, sign and install on the phone, OBB import | ✅ |
| Per-app options, in-game menu | ✅ |
| Library: update, options, data file, uninstall | ✅ |
| More games and apps | ⏳ |

## Download

Get the latest **THUMB APK** from [Releases](../../releases) and install it on your phone (Android 10+,
arm64). Releases are signed with the THUMB release key; certificate SHA-256:
`23:EE:AA:13:05:FA:B9:A0:84:B9:3B:F6:E3:A7:24:7A:FF:CA:4E:C9:4B:54:FC:07:CB:E7:49:4B:B4:2A:DC:55`

Prefer building it yourself? See [Building](#building) below. Same code, no binaries required.

## Using it (phone only, no PC)

1. Install the THUMB app and open it.
2. Tap **Add app** and pick an `.apk` (or an `.xapk` / `.apks` / `.apkm` bundle) you own.
3. **THUMB Doctor** checks the app's 32-bit libraries and shows how much of what they need THUMB supports.
4. Choose options (or keep the defaults) and tap **Patch & install**. THUMB adds its translator,
   signs the app with a key that lives only on your phone, and installs it.
5. If the game uses an **OBB data file**, THUMB finds it automatically in a folder you allow, or you
   pick it yourself. It's handed to the game on first launch.
6. Patched apps show up under **Patched**: open, update to a newer THUMB, change options, add a data
   file or uninstall. Updates are re-patched from the installed copy, so saves are kept and you don't
   need the APK again.

### Options

| Build option | |
|---|---|
| Block ads | Stops calls to known ad SDKs (on by default) |
| FPS | **Compat (60 FPS)**, what old games were built for (default), or **Default** (the screen decides) |
| Sandbox | Removes sensitive permissions (contacts, location, camera, …); optionally internet too |
| Legacy compat shims | Always on: old Android behaviour modern Android removed (browsing from `/`, positioned asset files, …) |

**In-game menu** (optional, on by default): a draggable THUMB button inside the app, hidden or shown
with a **three-finger double tap**. It has an FPS counter, a speed slider (slow motion / fast forward),
**FPS unlock** (a slider from 30 FPS to the screen's maximum), ad blocking on/off, keep screen on,
**Force restart** and **Kill** for frozen apps. Everything starts as the app normally behaves.

## How it works

```
 patched APK
 ├─ classes*.dex           the app's Java code, unchanged (runs on Android's 64-bit ART)
 ├─ lib/arm64-v8a/
 │   ├─ libgame.so         THUMB stub (64-bit): loads libthumb.so on System.loadLibrary
 │   ├─ libgame_arm32.so   the original 32-bit library
 │   └─ libthumb.so        the translator
 └─ assets/thumb/          options + in-game menu
```

- **CPU:** the original ARM32 code is JIT-translated to ARM64 with dynarmic, the first time each
  block runs, then cached. Guest memory is a 4 GB arena, so a 32-bit pointer maps to a host pointer by
  adding a constant.
- **Loader:** an ELF32 loader maps the library, applies relocations, loads its sibling libraries and runs
  constructors and `JNI_OnLoad`.
- **Thunks:** imports (libc, libm, pthreads, zlib, OpenGL ES 1, liblog, libandroid, …) resolve to tiny
  stubs that jump into 64-bit host code, converting between the 32-bit and 64-bit ABIs. Raw `svc`
  syscalls are emulated too (some games ship self-unpacking libraries).
- **JNI:** the guest gets a 32-bit `JNIEnv`. Java objects map to 32-bit handles, and native methods
  registered by the game become libffi closures that Java can call.
- **Install:** the THUMB app rewrites the APK on the phone (manifest updated for modern Android,
  libraries swapped, 16 KB-aligned), signs it with apksig, and installs it with `PackageInstaller`.
  Android doesn't let installers write other apps' OBB folders, so the OBB is shared through a
  content provider and the translator moves it into place.

### Performance

It's a translator, not an emulator: only the native library is translated, while Java code, graphics
(OpenGL) and audio run natively. Worms 3 runs at a steady 60 FPS on a Pixel 9a.

## Layout

```
core/       translator: memory arena, ELF loader, dynarmic CPU, thunks, JNI bridge
android/    THUMB app (Kotlin/Compose): Doctor, on-phone patching, signing, install, OBB import, Library
            overlay/: the in-game menu (plain Android views, added to patched apps as a small dex)
harness/    Linux test runner with a fake JVM
shim/       Android runtime (libthumb.so) and per-library stub
tools/      build and code-generation scripts
```

## Building

Everything needed to build THUMB is in this repository (plus the two submodules).

**Requirements:** Linux, CMake + Ninja, a C++20 compiler, Boost headers, libffi (for the Linux
harness), Python 3, JDK 21, the Android SDK (platform 37, build-tools 37) and NDK r30 (the versions CI uses).

```sh
git clone --recursive https://github.com/whodfisbane/thumb.git && cd thumb

# 1. Linux harness (also generates the supported-function list the app ships)
cmake -S . -B build -G Ninja && ninja -C build harness

# 2. Android runtime: build-android/libthumb.so + libthumb_stub.so (downloads libffi once)
NDK=/path/to/android-ndk tools/build-android.sh       # --dev: developer extras, never for releases

# 3. The THUMB app
cd android
./gradlew assembleDebug                                # or assembleRelease, see below
```

The app bundles the runtime from `build-android/` and the function list from `build/harness`, so
build steps 1 and 2 first. Release builds refuse a `--dev` runtime. To sign a release build, set
`THUMB_RELEASE_KEYSTORE`, `THUMB_RELEASE_STORE_PASSWORD` and `THUMB_RELEASE_KEY_ALIAS`.

Developers can also patch an APK on the PC: `tools/repack.py original.apk patched.apk`.

### Tests

```sh
NDK=/path/to/android-ndk tools/run-tests.sh   # ARM32 test libraries run through the harness
```

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
