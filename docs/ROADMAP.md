# THUMB roadmap

Where we are (2026-10-08): **4 apps run on a Pixel 9a** (GrapheneOS, Android 17, arm64-only), patched
and installed **on the phone itself**: Worms 3 (fully playable), ScummVM SDL and native builds
(playable), and VLC 2.0.6 (plays video with hardware decoding). Covered APIs: libc/libm/pthreads,
zlib, JNI, OpenGL ES 1 + 2, EGL, native windows, AMediaCodec, sockets, guest `dlopen`. Biggest gaps:
OpenSL ES audio and Mono-based Unity games.

## Done

- **Release basics:** GPL-3.0, third-party notices, README credits, dev-only features behind
  `--dev`, signed release builds, GitHub Actions CI (translator + guest tests + app)
- **Tests:** `tools/run-tests.sh` runs ARM32 test libraries through the harness (libc, printf/scanf,
  setjmp, C++ exceptions, threads incl. cleanup handlers, files, soft-float ABI, dlopen, syscalls)
- **THUMB app (no PC needed):** import `.apk`/`.xapk`/`.apks`/`.apkm`, THUMB Doctor with a clear
  verdict, on-phone patching (targetSdk raised, 16 KB-aligned libs, split APKs), signing, install,
  automatic OBB search and delivery, console
- **Library ("Patched"):** open, update/re-patch from the installed copy, change options, data file
  status (reported by the app), uninstall
- **Per-app options** (stored as `assets/thumb/options.json`): ad blocking, FPS (Compat 60 / Default),
  sandbox (remove sensitive permissions, optionally internet), custom app name
- **Per-game fixes** (Proton-style, offered only for their app): Worms 3 "Lower audio delay"
  (its sound queue went from ~1.4 s to 0.16 s)
- **In-game menu:** floating button (three-finger double tap hides it), FPS counter, speed slider,
  FPS unlock slider, ad-block toggle, keep screen on, force restart / kill
- **Translator:** dynarmic JIT, ELF loader, ~650 thunks: libc, pthreads, zlib, JNI bridge,
  GLES 1 + 2, EGL, ANativeWindow, AMediaCodec (hardware decoding), sockets, raw syscalls (futex),
  self-unpacking libraries, guest `dlopen`/`dlsym`, legacy compat shims (virtual `/`, positioned asset
  fds), code cache invalidation on `mprotect`/`cacheflush`

## Next: correctness bugs (before anything else)

- [x] **JNI handle table never frees entries.** (fixed: counted handles, freed per native call and on Delete*Ref; Worms stays at 3 live handles) Every local reference Java hands out gets a new
      handle, so a game creating strings every frame grows memory without limit (and wraps after
      ~67M handles). Free handles on `DeleteLocalRef`/`DeleteGlobalRef` and when a native call returns
- [x] **`pthread_once` holds one global lock** (fixed: per-control state, test included) while running the init function: an init that waits on
      another thread which hits a different `pthread_once` deadlocks (common in C++ static init).
      Use a per-`once_control` state with a condition variable
- [ ] Mutexes are always recursive: `pthread_mutex_trylock` by the owner returns 0 instead of `EBUSY`
      for normal/errorcheck mutexes. Track the mutex type
- [ ] **Signing key can't be backed up.** It lives in AndroidKeyStore, so reinstalling THUMB, a factory
      reset or a new phone loses it, and patched apps can then only be updated by uninstalling them
      (which wipes their saves). Options: an exportable key generated in software and backed up
      encrypted, and/or save export/import before re-patching (see Sandbox: save virtualisation)

## Phase 3: Compatibility

- [ ] **OpenSL ES** (audio): a large share of 2011–2016 NDK games output sound through it. Top item
- [ ] **Mono-based Unity games** (arm32-only apps are mostly pre-2019, i.e. the Mono era): the Mono
      JIT writes ARM code into RWX memory without calling `mprotect` each time, so invalidating on
      `mprotect` misses it. Needs write tracking on executable pages (write-protect translated pages,
      invalidate on fault). The single biggest compatibility unlock
- [ ] More system APIs: GLES 3, AAudio, OpenAL, libandroid input/assets (`AAsset*`, `AInputQueue`),
      `ANativeActivity` (NativeActivity games)
- [x] Doctor: don't count libraries that need private Android system libraries (e.g. VLC's
      `libiomx`/`libanw`); modern Android won't load them even natively, so apps already fall back
- [ ] Choose an already-installed app as the source
- [ ] Community compatibility list (apps/games, status, notes)
- [ ] Tests for JNI (needs the fake JVM to drive natives from the test)

## Phase 3.5: Addons (niche and community)

All **official API support ships inside THUMB**, so patched apps work fully offline. Addons are for:

- [ ] Community content (data only, no native code): per-game patches, value-editor presets,
      gamepad layouts, compatibility notes
- [ ] Niche or very large optional components, official and signed (e.g. a Vulkan layer)
- [ ] THUMB Doctor names the addon an app needs and offers to download it

## Phase 4: In-game menu extras

- [ ] Configurable hide gesture (e.g. four-finger double tap, or off)
- [ ] **Dev mode: value editor** (search a value, narrow down, edit or freeze), off by default, with
      this warning every time it is turned on:
      > ⚠️ Dev mode edits the game's memory directly. Use it only in single-player/offline
      > games. Editing values can corrupt save files or crash the game, so back up your saves
      > first. In online games it may break the terms of service, get your account banned,
      > or spoil the game for other players.
- [ ] Force texture filtering (via the GLES thunks)
- [ ] Gamepad → touch mapping for touch-only games
- [ ] Log / crash viewer for the patched app's `thumb` log
- [ ] Custom icon (badge) for patched apps

## Phase 5: Performance and polish

- [ ] Measure THUMB's own overhead (stats readout), tune JIT cache sizes per thread
- [ ] Clean up per-thread state on thread exit
- [ ] Crash reports with guest backtraces (symbolized like the harness does)
- [ ] Reproducible release builds

## Ideas queue

- [ ] **Sandbox mode, part 2:** virtualize file access (save backup/export, multiple save profiles),
      fake device identifiers, per-app network setting (none / LAN only / full)
- [ ] **LAN multiplayer:** sockets now work; test with LAN-capable games
- [ ] **Legacy Android compatibility shims** (more): map old storage paths (`/mnt/sdcard`,
      `/sdcard/<app>`), fake answers for removed system services as they show up

## Known gaps

- Linux harness: Worms 3's text rendering calls a Java method through a null method ID
  (fake-JVM fidelity issue; works on a real phone)
- `pthread_exit` from a nested guest call unwinds only the innermost frame
