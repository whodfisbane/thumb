// <android/native_window.h> and <android/native_window_jni.h>. Windows are
// host pointers, so the guest gets handles (shared with EGL, see handles.h).
//
// ANativeWindow_lock hands out the window's pixel memory, which lives outside
// the guest arena: the guest gets a shadow buffer in guest memory instead,
// copied into the real buffer on ANativeWindow_unlockAndPost (the same trick
// as AndroidBitmap_lockPixels).
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "thunks/handles.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

namespace h32 {
namespace {

uint32_t arg(GuestThread& t, size_t i) { return t.arg_word(i); }

#ifdef __ANDROID__

ANativeWindow* win(uint32_t h) { return static_cast<ANativeWindow*>(window_handles().to_host(h)); }

// ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject surface)
void t_fromSurface(GuestThread& t) {
    auto surface = static_cast<jobject>(jni::ref_to_host(arg(t, 1)));
    set_ret32(t, window_handles().to_guest(ANativeWindow_fromSurface(jni::host_env(), surface)));
}

void t_acquire(GuestThread& t) { if (auto* w = win(arg(t, 0))) ANativeWindow_acquire(w); }
void t_release(GuestThread& t) { if (auto* w = win(arg(t, 0))) ANativeWindow_release(w); }
void t_getWidth(GuestThread& t) { auto* w = win(arg(t, 0)); set_ret32(t, w ? ANativeWindow_getWidth(w) : -1); }
void t_getHeight(GuestThread& t) { auto* w = win(arg(t, 0)); set_ret32(t, w ? ANativeWindow_getHeight(w) : -1); }
void t_getFormat(GuestThread& t) { auto* w = win(arg(t, 0)); set_ret32(t, w ? ANativeWindow_getFormat(w) : -1); }

void t_setBuffersGeometry(GuestThread& t) {
    auto* w = win(arg(t, 0));
    set_ret32(t, w ? ANativeWindow_setBuffersGeometry(w, int32_t(arg(t, 1)), int32_t(arg(t, 2)), int32_t(arg(t, 3))) : -1);
}

int bytes_per_pixel(int32_t format) {
    return format == WINDOW_FORMAT_RGB_565 ? 2 : 4;
}

struct Locked {
    void* host = nullptr;
    gaddr shadow = 0;
    size_t size = 0;
};
std::mutex g_lock;
std::unordered_map<ANativeWindow*, Locked> g_locked;
std::unordered_map<ANativeWindow*, std::pair<gaddr, size_t>> g_shadow;  // reused between frames

// int32_t ANativeWindow_lock(ANativeWindow*, ANativeWindow_Buffer* out, ARect* inOutDirtyBounds)
// Guest ANativeWindow_Buffer: width, height, stride, format, bits (4 bytes), reserved[6].
void t_lock(GuestThread& t) {
    auto* w = win(arg(t, 0));
    uint32_t out = arg(t, 1), dirty = arg(t, 2);
    if (!w || !out) return set_ret32(t, uint32_t(-22));
    ARect rect{};
    if (dirty) std::memcpy(&rect, mem().ptr<void>(dirty), sizeof rect);  // ARect is four int32: same layout
    ANativeWindow_Buffer buf{};
    int32_t r = ANativeWindow_lock(w, &buf, dirty ? &rect : nullptr);
    if (r != 0) return set_ret32(t, uint32_t(r));
    if (dirty) std::memcpy(mem().ptr<void>(dirty), &rect, sizeof rect);
    size_t size = size_t(buf.stride) * buf.height * bytes_per_pixel(buf.format);
    gaddr shadow;
    {
        std::lock_guard lk(g_lock);
        auto& s = g_shadow[w];
        if (s.second < size) {
            if (s.first) mem().free(s.first);
            s = {mem().malloc(size), size};
        }
        shadow = s.first;
        g_locked[w] = {buf.bits, shadow, size};
    }
    if (!shadow) {
        ANativeWindow_unlockAndPost(w);
        return set_ret32(t, uint32_t(-12));
    }
    // Keep what's already on screen, for apps that only redraw the dirty area.
    std::memcpy(mem().ptr<void>(shadow), buf.bits, size);
    mem().write<int32_t>(out + 0, buf.width);
    mem().write<int32_t>(out + 4, buf.height);
    mem().write<int32_t>(out + 8, buf.stride);
    mem().write<int32_t>(out + 12, buf.format);
    mem().write<uint32_t>(out + 16, shadow);
    set_ret32(t, 0);
}

void t_unlockAndPost(GuestThread& t) {
    auto* w = win(arg(t, 0));
    if (!w) return set_ret32(t, uint32_t(-22));
    Locked l;
    {
        std::lock_guard lk(g_lock);
        auto it = g_locked.find(w);
        if (it == g_locked.end()) return set_ret32(t, uint32_t(-22));
        l = it->second;
        g_locked.erase(it);
    }
    std::memcpy(l.host, mem().ptr<void>(l.shadow), l.size);
    set_ret32(t, uint32_t(ANativeWindow_unlockAndPost(w)));
}

#else  // Linux harness: no windows

void t_fail(GuestThread& t) { set_ret32(t, uint32_t(-1)); }
void t_zero(GuestThread& t) { set_ret32(t, 0); }

#endif

}  // namespace

namespace thunks {

void register_native_window() {
#ifdef __ANDROID__
    add("ANativeWindow_fromSurface", t_fromSurface);
    add("ANativeWindow_acquire", t_acquire);
    add("ANativeWindow_release", t_release);
    add("ANativeWindow_getWidth", t_getWidth);
    add("ANativeWindow_getHeight", t_getHeight);
    add("ANativeWindow_getFormat", t_getFormat);
    add("ANativeWindow_setBuffersGeometry", t_setBuffersGeometry);
    add("ANativeWindow_lock", t_lock);
    add("ANativeWindow_unlockAndPost", t_unlockAndPost);
#else
    add("ANativeWindow_fromSurface", t_zero);
    for (const char* n : {"ANativeWindow_acquire", "ANativeWindow_release"}) add(n, t_zero);
    for (const char* n : {"ANativeWindow_getWidth", "ANativeWindow_getHeight", "ANativeWindow_getFormat",
                          "ANativeWindow_setBuffersGeometry", "ANativeWindow_lock", "ANativeWindow_unlockAndPost"})
        add(n, t_fail);
#endif
}

}  // namespace thunks
}  // namespace h32
