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
- [x] **Raw `exit` syscall ended the whole process** (fixed: `exit` (1) ends the calling thread,
      `exit_group` (248) ends the process without atexit handlers)
- [x] **Packed relocations were silently ignored** (fixed: RELR and Android APS2 are applied, RELA
      and other unsupported formats refuse to load instead of crashing later; tests included)
- [x] **Guest threads stayed attached to the JVM** after THUMB attached them on their first JNI call;
      ART aborts when an attached thread exits (fixed: detached after the TLS destructors)
- [ ] **Speed slider breaks absolute timeouts.** The game reads the virtual `CLOCK_REALTIME`, but
      `pthread_cond_timedwait`/`sem_timedwait` compare its deadline with the real clock (and futex
      `WAIT_BITSET` passes it through), so after a minute at 2x every short timed wait takes ~60 s,
      and at 0.5x they time out instantly. The offset stays after going back to 1x. Convert virtual
      deadlines to real ones; also honour `pthread_condattr_setclock` (ignored, so monotonic conds
      time out at once), scale relative futex timeouts, and give `CLOCK_BOOTTIME` its own timeline
      (it shares the monotonic one, so it jumps by (scale − 1) × suspend time)
- [ ] **Mutex/cond table is fragile:** one global lock + hash lookup per lock/unlock; entries keyed by
      address, so a mutex freed without `pthread_mutex_destroy` leaves a stale (possibly locked) host
      mutex for whatever reuses that memory; `cond_wait` on a recursively locked mutex unlocks only
      one level (deadlock); mutexes are always recursive (`trylock` by the owner returns 0, not
      `EBUSY`). Fix: implement mutex and cond on the guest word with host futexes, like bionic
- [ ] **Guest stacks have no guard page** (8 MB blocks from the heap): an overflow silently
      corrupts the heap block below. Reserve stacks with a `PROT_NONE` page under them
- [ ] `PushLocalFrame`/`PopLocalFrame` are passthroughs: the host frees the refs but their guest
      handles stay in the table (attached loop threads only free them on `DeleteLocalRef`)
- [ ] **Signing key can't be backed up.** It lives in AndroidKeyStore, so reinstalling THUMB, a factory
      reset or a new phone loses it, and patched apps can then only be updated by uninstalling them
      (which wipes their saves). Options: an exportable key generated in software and backed up
      encrypted, and/or save export/import before re-patching (see Sandbox: save virtualisation)

## Phase 3: Compatibility

- [ ] **OpenSL ES** (audio): a large share of 2011–2016 NDK games output sound through it. Top item
- [ ] **Unity, IL2CPP first:** arm32 IL2CPP games have no JIT, so they may already be close. Try one
      before Mono
- [ ] **Mono-based Unity games** (arm32-only apps are mostly pre-2019, i.e. the Mono era). Two
      prerequisites: (1) **guest signal delivery** — `sigaction` and `pthread_kill` are no-ops today,
      but Mono uses SIGSEGV for implicit null checks and signals to suspend threads for GC, so it
      won't boot without them; (2) write tracking on executable pages — the Mono JIT writes ARM code
      into RWX memory without calling `mprotect` each time, so invalidating on `mprotect` misses it
      (write-protect translated pages, invalidate on fault). The single biggest compatibility unlock
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
- [ ] Log / crash viewer for the patched app's `thumb` log. Crashes are invisible today (`fatal()`
      is a bare `abort()`, missing imports quietly return 0): write a crash / missing-symbol report
      through the existing game → THUMB channel (`ObbProvider.update`) and show it in the Library
- [ ] Custom icon (badge) for patched apps

## Phase 5: Performance and polish

- [ ] Measure THUMB's own overhead (stats readout), tune JIT cache sizes per thread
- [ ] Clean up per-thread state on thread exit
- [ ] Crash reports with guest backtraces (symbolized like the harness does)
- [ ] Reproducible release builds
- [ ] **Patcher streaming:** every entry is read into a `ByteArray` and re-deflated, so big
      asset-heavy APKs can run out of memory and patch slowly. Stream entries and copy unchanged
      compressed bytes raw
- [ ] Golden tests for patcher/unpatcher/zip code (patch → unpatch → compare on a tiny fake APK)
- [ ] **F-Droid**: build libffi from source (submodule or srclib instead of a download), generate the
      supported-function list during the build, reproducible builds; then submit. (Play Store is
      unlikely: its policies restrict apps that modify and re-sign other apps)

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
