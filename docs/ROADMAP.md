# THUMB roadmap

Where we are (2026-10-06): the translator runs **Worms 3 fully playable on a Pixel 9a**
(GrapheneOS, Android 17): graphics, audio, touch, 60 fps. Installing still needs a PC
(`tools/repack.py` + adb).

## Phase 0: Release-ready open source

- [ ] Pick a license (GPL-3.0 or MIT), add `LICENSE` and third-party notices
      (dynarmic 0BSD, TLSF BSD, libffi MIT, jni.h Apache-2.0, dynarmic externals)
- [ ] README credits: "built with Claude Opus 5.5", plus a note on AI-assisted code
- [ ] Put the OBB dev-fetch (127.0.0.1:47070) behind a debug-only switch; it must never ship enabled
- [ ] Tests: small ARM32 test libraries (NDK `armeabi-v7a`) exercising libc, setjmp,
      C++ exceptions, threads, JNI, run through the Linux harness
- [ ] GitHub Actions: build the harness and run the tests, build `libthumb.so`, build the THUMB app
- [ ] Release signing key (kept private) and reproducible release builds

## Phase 1: THUMB app (no PC needed)

- [ ] Kotlin + Jetpack Compose app, arm64, minimum Android 10
- [ ] Import an app: pick an `.apk`, or a bundle (`.xapk` / `.apks` / `.apkm`), or choose an installed app
- [ ] On-device patching: the same steps as `repack.py` (stubs + original libs + `libthumb.so`),
      handling split APKs (`config.armeabi_v7a`), signed with a per-device THUMB key via apksig
- [ ] Install through `PackageInstaller` sessions (works for split APKs)
- [ ] OBB handling, automatic where possible:
  - `.xapk` bundles often contain the OBB: import it automatically
  - otherwise an "Add OBB" button with a file picker
  - delivery: THUMB serves the OBB through a content provider protected by a signature
    permission; the patched game's runtime copies it into its own OBB folder on first launch
- [ ] Library screen: patched apps, status, re-patch after THUMB updates, uninstall
- [ ] Log viewer for the `thumb` log tag (for bug reports)

## Phase 2: Per-app options

- [ ] Custom app name and icon for patched apps
- [ ] FPS unlock (request 90/120 Hz), off by default; some games tie game speed to frame rate
- [ ] Ad-method blocker toggle and custom block patterns
- [ ] Log level, performance overlay (FPS, guest heap, JIT cache, translation time)
- [ ] Options travel as `assets/thumb.json` inside the patched APK

## Phase 3: Compatibility

- [ ] **THUMB Doctor**: scan an APK and list imports THUMB doesn't implement yet
      ("92% ready, missing: OpenSL ES")
- [ ] Community compatibility list (apps/games, status, notes)
- [ ] More system APIs: GLES 2/3, EGL, OpenSL ES, AAudio, libandroid (assets, input, window),
      guest `dlopen`/`dlsym` (multi-library and plugin-based engines)
- [ ] Self-modifying code detection (Mono-based Unity games generate code at runtime)
- [ ] Test a second game and a non-game app

## Phase 4: Performance and polish

- [ ] Measure THUMB's own overhead (stats readout), tune JIT cache sizes per thread
- [ ] Free JNI handle-table entries; clean up on thread exit
- [ ] Crash reports with guest backtraces (symbolized like the harness does)

## Known gaps

- Linux harness: Worms 3's text rendering calls a Java method through a null method ID
  (fake-JVM fidelity issue; works on a real phone)
- `pthread_once` holds a global lock while running the init function
- Mutexes are always recursive
