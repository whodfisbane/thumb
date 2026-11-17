// Speed control ("speed hack"): every clock the guest reads runs through a
// virtual timeline that advances `scale` times as fast as real time, and
// sleeps are shortened or stretched to match. scale 1.0 = untouched.
#pragma once

#include <cstdint>
#include <ctime>

namespace h32::timescale {

void set_scale(double scale);
double scale();

// Guest view of a host clock reading (nanoseconds since that clock's epoch).
// Wall and monotonic clocks are scaled, each on its own timeline; CPU-time
// clocks are not.
int64_t virtual_ns(clockid_t clock, int64_t real_ns);
// The guest's current time on `clock`.
int64_t now_ns(clockid_t clock);
// Host duration to sleep for a guest-requested duration.
int64_t real_sleep_ns(int64_t guest_ns);
// Real nanoseconds left until the guest's absolute deadline `guest_abs_ns` on
// `clock` (a time the guest computed from its virtual clock); 0 if it passed.
// Waits with absolute timeouts must use this: comparing a virtual deadline with
// the real clock is off by however far the virtual clock has drifted.
int64_t real_ns_until(clockid_t clock, int64_t guest_abs_ns);

// Frame counting for the FPS display: thunks call frame() once per frame
// (e.g. on glClear with the color buffer); fps() returns the recent rate.
// With a limit set, frame() also waits so frames are at least 1/limit apart.
void frame();
// Apps that swap buffers themselves (guest eglSwapBuffers) are counted there;
// otherwise a color-buffer glClear marks a frame (Java-side GLSurfaceView apps).
void note_guest_swap();
bool guest_swaps();
double fps();
void set_fps_limit(int fps);  // 0 = unlimited

}  // namespace h32::timescale
