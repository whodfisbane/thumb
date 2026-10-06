# THUMB roadmap

Where we are (2026-10-06): the translator runs **Worms 3 fully playable on a Pixel 9a**
(GrapheneOS, Android 17): graphics, audio, touch, 60 fps. Installing still needs a PC
(`tools/repack.py` + adb).

## Phase 0: Release-ready open source

- [x] License: GPL-3.0 (`LICENSE`), third-party notices (`THIRD_PARTY_NOTICES.md`)
- [x] README credits: "built with Claude Opus 5.5", plus a note on AI-assisted code
- [x] OBB dev-fetch is compiled in only with `tools/build-android.sh --dev` (off by default)
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

## Phase 2.5: In-game THUMB menu

A floating button inside every patched app (no overlay permission needed: it lives in
the app's own window). The repackager adds a small `classes-thumb.dex` for the UI.

- [ ] Floating button + panel; **three-finger tap** hides/unhides it (gesture not passed to the game)
- [ ] FPS counter
- [ ] **FPS unlock** (90/120 Hz), shown with this warning on first enable:
      > ⚠️ Many older games tie their game speed to the frame rate. Unlocking FPS may make
      > the game run too fast, break physics or animations, or drain more battery.
      > If the game speeds up, use the speed slider to bring it back to 1×.
- [ ] **Speed slider** (slow-mo / fast-forward): THUMB scales the guest's clock
      (`gettimeofday`, `clock_gettime`, `time`, ...)
- [ ] Ad-block toggle (JNI method block list)
- [ ] **Dev mode: value editor** (search a value, narrow down, edit or freeze), off by default,
      shown with this warning every time it is turned on:
      > ⚠️ Dev mode edits the game's memory directly. Use it only in single-player/offline
      > games. Editing values can corrupt save files or crash the game, so back up your saves
      > first. In online games it may break the terms of service, get your account banned,
      > or spoil the game for other players.
- [ ] Force texture filtering (via the GLES thunks)
- [ ] Gamepad → touch mapping for touch-only games
- [ ] Log / crash viewer

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
