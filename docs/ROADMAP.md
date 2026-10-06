# THUMB roadmap

Where we are (2026-10-06): the translator runs **Worms 3 fully playable on a Pixel 9a**
(GrapheneOS, Android 17): graphics, audio, touch, 60 fps. Installing still needs a PC
(`tools/repack.py` + adb).

## Phase 0: Release-ready open source

- [x] License: GPL-3.0 (`LICENSE`), third-party notices (`THIRD_PARTY_NOTICES.md`)
- [x] README credits: "built with Claude Opus 5.5", plus a note on AI-assisted code
- [x] OBB dev-fetch is compiled in only with `tools/build-android.sh --dev` (off by default)
- [x] Tests: `tools/run-tests.sh` builds ARM32 test libraries (tests/guest) with the NDK and runs
      them through the harness: libc, printf/scanf, qsort/bsearch, setjmp, C++ exceptions, threads,
      semaphores, files/stat/dirent, soft-float ABI, dlopen, raw syscalls, /proc/self/maps (63 checks)
- [ ] Tests for JNI (needs the fake JVM to drive natives from the test)
- [ ] GitHub Actions: build the harness and run the tests, build `libthumb.so`, build the THUMB app
- [ ] Release signing key (kept private) and reproducible release builds

## Phase 1: THUMB app (no PC needed)

- [x] Kotlin + Jetpack Compose app, arm64, minimum Android 10
- [x] Import an app: pick an `.apk`, or a bundle (`.xapk` / `.apks` / `.apkm`)
- [ ] Choose an already-installed app as the source
- [x] On-device patching: the same steps as `repack.py` (stubs + original libs + `libthumb.so`),
      handling split APKs (`config.armeabi_v7a`), signed with a per-device THUMB key via apksig
- [x] Install through `PackageInstaller` sessions (works for split APKs)
- [ ] OBB handling, automatic where possible:
  - `.xapk` bundles often contain the OBB: import it automatically
  - otherwise an "Add OBB" button with a file picker
  - delivery: THUMB serves the OBB through a content provider protected by a signature
    permission; the patched game's runtime copies it into its own OBB folder on first launch
- [ ] Library screen: patched apps, status, re-patch after THUMB updates, uninstall
- [x] Console: full raw log with copy button (patching side)
- [ ] Log viewer for the patched app's `thumb` runtime log

## Phase 2: Per-app options

- [ ] Custom app name and icon for patched apps
- [ ] FPS unlock (request 90/120 Hz), off by default; some games tie game speed to frame rate
- [ ] Ad-method blocker toggle and custom block patterns
- [ ] Log level, performance overlay (FPS, guest heap, JIT cache, translation time)
- [ ] Options travel as `assets/thumb.json` inside the patched APK

## Phase 2.5: In-game THUMB menu

A floating button inside every patched app (no overlay permission needed: it lives in
the app's own window). The repackager adds a small `classes-thumb.dex` for the UI.

- [ ] Floating button + panel; **three-finger double tap** (two taps within ~400 ms) hides/unhides it,
      so games played with three fingers don't trigger it. The gesture is configurable per app
      (e.g. four-finger double tap, or off), and gestures THUMB consumes are not passed to the game
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

- [x] **THUMB Doctor** (`tools/thumb-doctor.py`): scan an APK and list imports THUMB doesn't implement yet
- [ ] THUMB Doctor inside the THUMB app (Kotlin port; supported-function list generated at build time)
- [ ] Community compatibility list (apps/games, status, notes)
- [ ] More system APIs: GLES 2/3, EGL, OpenSL ES, AAudio, libandroid (assets, input, window),
      guest `dlopen`/`dlsym` (multi-library and plugin-based engines)
- [ ] Self-modifying code detection (Mono-based Unity games generate code at runtime)
- [ ] Test a second game and a non-game app

## Phase 3.5: Addons (niche and community)

All **official API support ships inside THUMB** (each API family is only ~10-50 KB; the
JIT is ~1.6 MB of the ~4 MB runtime), so patched apps work fully offline. Addons are for:

- [ ] Community content (data only, no native code): per-game patches, value-editor presets,
      gamepad layouts, compatibility notes
- [ ] Niche or very large optional components, official and signed (e.g. a Vulkan layer)
- [ ] THUMB Doctor names the addon an app needs and offers to download it
- [ ] Code stays organized per API family so packs can be split out later if needed

## Ideas queue

- [ ] **Sandbox mode** (per app, default on for games): strip unneeded permissions from the
      manifest at patch time; in the translator, block or limit native networking, virtualize
      file access (save backup/export, multiple save profiles), fake device identifiers
- [ ] **LAN multiplayer**: real socket support (32-bit sockaddr/addrinfo/timeval/select
      conversions), with a per-app network setting (none / LAN only / full)
- [ ] Raise `targetSdkVersion` during patching (Android 14+ refuses installs below 23)
- [ ] **Legacy Android compatibility shims** (per-app toggles, on by default):
  - [x] virtual listing for folders modern Android hides (`/`, `/storage`, `/storage/emulated`)
        when the real listing is denied, so old file browsers can reach `sdcard`
  - [x] `AssetFileDescriptor.getFileDescriptor()` returns a descriptor positioned at the asset
        (old Android behaviour; ScummVM's native port reads without seeking)
  - [ ] map old storage paths (`/mnt/sdcard`, `/sdcard/<app>`) to places the app can use
  - [ ] fake answers for removed system services/APIs as they show up

## Phase 4: Performance and polish

- [ ] Measure THUMB's own overhead (stats readout), tune JIT cache sizes per thread
- [ ] Free JNI handle-table entries; clean up on thread exit
- [ ] Crash reports with guest backtraces (symbolized like the harness does)

## Known gaps

- Linux harness: Worms 3's text rendering calls a Java method through a null method ID
  (fake-JVM fidelity issue; works on a real phone)
- `pthread_once` holds a global lock while running the init function
- Mutexes are always recursive
