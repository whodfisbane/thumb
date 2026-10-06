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
int64_t virtual_ns(clockid_t clock, int64_t real_ns);
// Host duration to sleep for a guest-requested duration.
int64_t real_sleep_ns(int64_t guest_ns);

// Frame counting for the FPS display: thunks call frame() once per frame
// (e.g. on glClear with the color buffer); fps() returns the recent rate.
void frame();
double fps();

}  // namespace h32::timescale
