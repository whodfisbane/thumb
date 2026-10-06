// liblog and libjnigraphics.
#include <mutex>
#include <unordered_map>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

#ifdef __ANDROID__
#include <android/bitmap.h>
#include <android/log.h>
#endif

namespace h32 {
namespace {

void emit_log(int prio, const char* tag, const std::string& msg) {
#ifdef __ANDROID__
    __android_log_write(prio, tag ? tag : "guest", msg.c_str());
#else
    static const char* names = "??VDIWEFS";
    H32_INFO("[%c/%s] %s", (prio >= 0 && prio < 9) ? names[prio] : '?', tag ? tag : "guest", msg.c_str());
#endif
}

// int __android_log_print(int prio, const char* tag, const char* fmt, ...)
void t_log_print(GuestThread& t) {
    ArgCursor c{t, 3};
    VarArgs va(c);
    emit_log(int32_t(t.regs()[0]), mem().str(t.regs()[1]), guest_format(mem().str(t.regs()[2]), va));
    set_ret32(t, 1);
}
void t_log_vprint(GuestThread& t) {
    VarArgs va(t, t.regs()[3]);
    emit_log(int32_t(t.regs()[0]), mem().str(t.regs()[1]), guest_format(mem().str(t.regs()[2]), va));
    set_ret32(t, 1);
}
void t_log_write(GuestThread& t) {
    emit_log(int32_t(t.regs()[0]), mem().str(t.regs()[1]), mem().str(t.regs()[2]) ?: "");
    set_ret32(t, 1);
}

// ---- AndroidBitmap: pixels live in host memory, so lockPixels hands the
// guest a copy and unlockPixels writes it back. ----
struct Locked {
    void* host;
    gaddr guest;
    size_t size;
};
std::mutex g_bitmap_lock;
std::unordered_map<void*, Locked> g_locked;  // key: host jobject

// int AndroidBitmap_getInfo(JNIEnv*, jobject, AndroidBitmapInfo*) — layout identical on both sides.
void t_bitmap_info(GuestThread& t) {
#ifdef __ANDROID__
    int r = AndroidBitmap_getInfo(jni::host_env(), static_cast<jobject>(jni::ref_to_host(t.regs()[1])),
                                  mem().ptr<AndroidBitmapInfo>(t.regs()[2]));
    set_ret32(t, uint32_t(r));
#else
    H32_WARN("AndroidBitmap_getInfo: no bitmaps in the Linux harness");
    set_ret32(t, uint32_t(-1));
#endif
}

void t_bitmap_lock(GuestThread& t) {
#ifdef __ANDROID__
    JNIEnv* env = jni::host_env();
    auto bmp = static_cast<jobject>(jni::ref_to_host(t.regs()[1]));
    AndroidBitmapInfo info;
    int r = AndroidBitmap_getInfo(env, bmp, &info);
    if (r != 0) return set_ret32(t, uint32_t(r));
    void* pixels = nullptr;
    r = AndroidBitmap_lockPixels(env, bmp, &pixels);
    if (r != 0) return set_ret32(t, uint32_t(r));
    size_t size = size_t(info.stride) * info.height;
    gaddr g = mem().malloc(size);
    std::memcpy(mem().ptr<void>(g), pixels, size);
    {
        std::lock_guard lk(g_bitmap_lock);
        g_locked[bmp] = {pixels, g, size};
    }
    if (gaddr out = t.regs()[2]) mem().write<uint32_t>(out, g);
    set_ret32(t, 0);
#else
    set_ret32(t, uint32_t(-1));
#endif
}

void t_bitmap_unlock(GuestThread& t) {
#ifdef __ANDROID__
    JNIEnv* env = jni::host_env();
    auto bmp = static_cast<jobject>(jni::ref_to_host(t.regs()[1]));
    Locked l{};
    {
        std::lock_guard lk(g_bitmap_lock);
        auto it = g_locked.find(bmp);
        if (it == g_locked.end()) return set_ret32(t, uint32_t(-1));
        l = it->second;
        g_locked.erase(it);
    }
    std::memcpy(l.host, mem().ptr<void>(l.guest), l.size);
    mem().free(l.guest);
    set_ret32(t, uint32_t(AndroidBitmap_unlockPixels(env, bmp)));
#else
    set_ret32(t, uint32_t(-1));
#endif
}

}  // namespace

namespace thunks {

void register_android() {
    add("__android_log_print", t_log_print);
    add("__android_log_vprint", t_log_vprint);
    add("__android_log_write", t_log_write);
    add("AndroidBitmap_getInfo", t_bitmap_info);
    add("AndroidBitmap_lockPixels", t_bitmap_lock);
    add("AndroidBitmap_unlockPixels", t_bitmap_unlock);
}

}  // namespace thunks
}  // namespace h32
