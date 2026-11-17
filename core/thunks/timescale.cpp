#include "thunks/timescale.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>

#include "common.h"

namespace h32::timescale {
namespace {

std::atomic<double> g_scale{1.0};
std::atomic<bool> g_ever_changed{false};

// Per clock: virtual = base_virtual + (real - base_real) * scale.
struct Timeline {
    int64_t base_real = 0, base_virtual = 0;
    bool started = false;
};
std::mutex g_mutex;

// Scaled clocks, each with its own timeline: they differ by more than an
// offset (BOOTTIME also counts suspend time, RAW isn't NTP-slewed), so
// sharing one base would make them jump by (scale - 1) x that difference.
constexpr clockid_t kScaledClocks[] = {CLOCK_REALTIME,        CLOCK_MONOTONIC,        CLOCK_MONOTONIC_RAW,
                                       CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME};
Timeline g_timelines[std::size(kScaledClocks)];

Timeline* timeline(clockid_t c) {
    for (size_t i = 0; i < std::size(kScaledClocks); i++)
        if (kScaledClocks[i] == c) return &g_timelines[i];
    return nullptr;  // not scaled (CPU-time clocks)
}

int64_t real_now_ns(clockid_t c) {
    timespec ts;
    clock_gettime(c, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

std::atomic<uint64_t> g_frames{0};

}  // namespace

double scale() { return g_scale.load(std::memory_order_relaxed); }

void set_scale(double s) {
    if (s < 0.05) s = 0.05;
    if (s > 10.0) s = 10.0;
    std::lock_guard lk(g_mutex);
    // Rebase every timeline at "now" so time stays continuous.
    for (size_t i = 0; i < std::size(kScaledClocks); i++) {
        Timeline& t = g_timelines[i];
        int64_t real = real_now_ns(kScaledClocks[i]);
        int64_t virt = t.started ? t.base_virtual + int64_t(double(real - t.base_real) * scale()) : real;
        t.base_real = real;
        t.base_virtual = virt;
        t.started = true;
    }
    g_scale = s;
    g_ever_changed = true;
    H32_INFO("speed: x%.2f", s);
}

int64_t virtual_ns(clockid_t clock, int64_t real_ns) {
    if (!g_ever_changed.load(std::memory_order_relaxed)) return real_ns;
    std::lock_guard lk(g_mutex);
    Timeline* t = timeline(clock);
    if (!t || !t->started) return real_ns;
    return t->base_virtual + int64_t(double(real_ns - t->base_real) * scale());
}

int64_t now_ns(clockid_t clock) { return virtual_ns(clock, real_now_ns(clock)); }

int64_t real_sleep_ns(int64_t guest_ns) {
    double s = scale();
    return s == 1.0 ? guest_ns : int64_t(double(guest_ns) / s);
}

int64_t real_ns_until(clockid_t clock, int64_t guest_abs_ns) {
    int64_t left = guest_abs_ns - now_ns(clock);
    if (left <= 0) return 0;
    return timeline(clock) ? real_sleep_ns(left) : left;
}

std::atomic<int64_t> g_frame_interval_ns{0};
std::atomic<bool> g_guest_swaps{false};

void note_guest_swap() { g_guest_swaps.store(true, std::memory_order_relaxed); }
bool guest_swaps() { return g_guest_swaps.load(std::memory_order_relaxed); }

void set_fps_limit(int fps) {
    g_frame_interval_ns = fps > 0 ? 1000000000LL / fps : 0;
    H32_INFO("fps limit: %s", fps > 0 ? std::to_string(fps).c_str() : "off");
}

void frame() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    int64_t interval = g_frame_interval_ns.load(std::memory_order_relaxed);
    if (interval <= 0) return;
    // Pace frames: sleep until the next slot (render thread only calls this).
    thread_local int64_t next = 0;
    int64_t now = real_now_ns(CLOCK_MONOTONIC);
    if (now < next) {
        timespec ts{time_t((next - now) / 1000000000), long((next - now) % 1000000000)};
        nanosleep(&ts, nullptr);
        now = next;
    }
    next = std::max(now, next) + interval;
    if (next < now) next = now + interval;
}

double fps() {
    using clock = std::chrono::steady_clock;
    static std::mutex m;
    static clock::time_point last = clock::now();
    static uint64_t last_frames = 0;
    static double value = 0;
    std::lock_guard lk(m);
    auto now = clock::now();
    double dt = std::chrono::duration<double>(now - last).count();
    if (dt >= 0.5) {
        uint64_t f = g_frames.load();
        value = double(f - last_frames) / dt;
        last_frames = f;
        last = now;
    }
    return value;
}

}  // namespace h32::timescale
