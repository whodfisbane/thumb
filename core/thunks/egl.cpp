// EGL. Displays, configs, contexts and surfaces are host pointers, so the
// guest gets 32-bit handles for them (handles.h). Native windows come from
// ANativeWindow_fromSurface (android.cpp) and share its handle table.
//
// On the Linux harness there is no EGL: calls succeed with fake handles.
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "thunks/handles.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"
#include "thunks/timescale.h"

#ifdef __ANDROID__
#include <EGL/egl.h>
#endif

namespace h32 {
namespace {

uint32_t arg(GuestThread& t, size_t i) { return t.arg_word(i); }
int32_t* iptr(uint32_t a) { return mem().ptr<int32_t>(a); }

#ifdef __ANDROID__

HandleTable g_dpy(0x71000000), g_cfg(0x72000000), g_ctx(0x73000000), g_surf(0x74000000);

EGLDisplay dpy(uint32_t h) { return g_dpy.to_host(h); }
EGLConfig cfg(uint32_t h) { return g_cfg.to_host(h); }
EGLContext ctx(uint32_t h) { return g_ctx.to_host(h); }
EGLSurface surf(uint32_t h) { return g_surf.to_host(h); }

void t_eglGetDisplay(GuestThread& t) { set_ret32(t, g_dpy.to_guest(eglGetDisplay(EGL_DEFAULT_DISPLAY))); }
void t_eglInitialize(GuestThread& t) { set_ret32(t, eglInitialize(dpy(arg(t, 0)), iptr(arg(t, 1)), iptr(arg(t, 2)))); }
void t_eglTerminate(GuestThread& t) { set_ret32(t, eglTerminate(dpy(arg(t, 0)))); }

void t_eglQueryString(GuestThread& t) {
    static std::mutex m;
    static std::unordered_map<std::string, gaddr> cache;
    const char* s = eglQueryString(dpy(arg(t, 0)), int32_t(arg(t, 1)));
    if (!s) return set_ret32(t, 0);
    std::lock_guard lk(m);
    auto& slot = cache[s];
    if (!slot) slot = mem().static_string(s);
    set_ret32(t, slot);
}

// Fills a guest EGLConfig array (4-byte handles) from host configs.
void put_configs(uint32_t out, const std::vector<EGLConfig>& configs, EGLint n) {
    if (!out) return;
    for (EGLint i = 0; i < n; i++) mem().write<uint32_t>(out + i * 4, g_cfg.to_guest(configs[i]));
}

void t_eglChooseConfig(GuestThread& t) {
    uint32_t out = arg(t, 2);
    EGLint size = int32_t(arg(t, 3));
    std::vector<EGLConfig> configs(out && size > 0 ? size : 0);
    EGLint n = 0;
    EGLBoolean ok = eglChooseConfig(dpy(arg(t, 0)), iptr(arg(t, 1)), out ? configs.data() : nullptr, size, &n);
    if (ok) put_configs(out, configs, std::min(n, EGLint(configs.size())));
    if (uint32_t num = arg(t, 4)) mem().write<int32_t>(num, n);
    set_ret32(t, ok);
}

void t_eglGetConfigs(GuestThread& t) {
    uint32_t out = arg(t, 1);
    EGLint size = int32_t(arg(t, 2));
    std::vector<EGLConfig> configs(out && size > 0 ? size : 0);
    EGLint n = 0;
    EGLBoolean ok = eglGetConfigs(dpy(arg(t, 0)), out ? configs.data() : nullptr, size, &n);
    if (ok) put_configs(out, configs, std::min(n, EGLint(configs.size())));
    if (uint32_t num = arg(t, 3)) mem().write<int32_t>(num, n);
    set_ret32(t, ok);
}

void t_eglGetConfigAttrib(GuestThread& t) {
    set_ret32(t, eglGetConfigAttrib(dpy(arg(t, 0)), cfg(arg(t, 1)), int32_t(arg(t, 2)), iptr(arg(t, 3))));
}

void t_eglCreateWindowSurface(GuestThread& t) {
    auto* win = static_cast<EGLNativeWindowType>(window_handles().to_host(arg(t, 2)));
    if (!win && arg(t, 2)) H32_ERROR("eglCreateWindowSurface: unknown window handle 0x%x", arg(t, 2));
    set_ret32(t, g_surf.to_guest(eglCreateWindowSurface(dpy(arg(t, 0)), cfg(arg(t, 1)), win, iptr(arg(t, 3)))));
}

void t_eglCreatePbufferSurface(GuestThread& t) {
    set_ret32(t, g_surf.to_guest(eglCreatePbufferSurface(dpy(arg(t, 0)), cfg(arg(t, 1)), iptr(arg(t, 2)))));
}

void t_eglDestroySurface(GuestThread& t) { set_ret32(t, eglDestroySurface(dpy(arg(t, 0)), surf(arg(t, 1)))); }

void t_eglCreateContext(GuestThread& t) {
    set_ret32(t, g_ctx.to_guest(eglCreateContext(dpy(arg(t, 0)), cfg(arg(t, 1)), ctx(arg(t, 2)), iptr(arg(t, 3)))));
}

void t_eglDestroyContext(GuestThread& t) { set_ret32(t, eglDestroyContext(dpy(arg(t, 0)), ctx(arg(t, 1)))); }

void t_eglMakeCurrent(GuestThread& t) {
    set_ret32(t, eglMakeCurrent(dpy(arg(t, 0)), surf(arg(t, 1)), surf(arg(t, 2)), ctx(arg(t, 3))));
}

void t_eglSwapBuffers(GuestThread& t) {
    timescale::note_guest_swap();
    timescale::frame();
    set_ret32(t, eglSwapBuffers(dpy(arg(t, 0)), surf(arg(t, 1))));
}

void t_eglSwapInterval(GuestThread& t) { set_ret32(t, eglSwapInterval(dpy(arg(t, 0)), int32_t(arg(t, 1)))); }
void t_eglGetCurrentContext(GuestThread& t) { set_ret32(t, g_ctx.to_guest(eglGetCurrentContext())); }
void t_eglGetCurrentDisplay(GuestThread& t) { set_ret32(t, g_dpy.to_guest(eglGetCurrentDisplay())); }
void t_eglGetCurrentSurface(GuestThread& t) { set_ret32(t, g_surf.to_guest(eglGetCurrentSurface(int32_t(arg(t, 0))))); }
void t_eglGetError(GuestThread& t) { set_ret32(t, eglGetError()); }
void t_eglBindAPI(GuestThread& t) { set_ret32(t, eglBindAPI(arg(t, 0))); }
void t_eglQueryAPI(GuestThread& t) { set_ret32(t, eglQueryAPI()); }
void t_eglWaitGL(GuestThread& t) { set_ret32(t, eglWaitGL()); }
void t_eglWaitClient(GuestThread& t) { set_ret32(t, eglWaitClient()); }
void t_eglWaitNative(GuestThread& t) { set_ret32(t, eglWaitNative(int32_t(arg(t, 0)))); }
void t_eglReleaseThread(GuestThread& t) { set_ret32(t, eglReleaseThread()); }

void t_eglQuerySurface(GuestThread& t) {
    set_ret32(t, eglQuerySurface(dpy(arg(t, 0)), surf(arg(t, 1)), int32_t(arg(t, 2)), iptr(arg(t, 3))));
}
void t_eglQueryContext(GuestThread& t) {
    set_ret32(t, eglQueryContext(dpy(arg(t, 0)), ctx(arg(t, 1)), int32_t(arg(t, 2)), iptr(arg(t, 3))));
}
void t_eglSurfaceAttrib(GuestThread& t) {
    set_ret32(t, eglSurfaceAttrib(dpy(arg(t, 0)), surf(arg(t, 1)), int32_t(arg(t, 2)), int32_t(arg(t, 3))));
}

#else  // Linux harness: no EGL

uint32_t g_fake = 0x7F000001;
void t_egl_true(GuestThread& t) { set_ret32(t, 1); }
void t_egl_handle(GuestThread& t) { set_ret32(t, g_fake); }
void t_eglGetError(GuestThread& t) { set_ret32(t, 0x3000); }  // EGL_SUCCESS
void t_egl_configs(GuestThread& t, size_t out, size_t num) {
    if (uint32_t o = arg(t, out)) mem().write<uint32_t>(o, g_fake);
    if (uint32_t n = arg(t, num)) mem().write<int32_t>(n, 1);
    set_ret32(t, 1);
}
void t_eglQueryString(GuestThread& t) {
    static gaddr s = mem().static_string("THUMB harness (no EGL)");
    set_ret32(t, s);
}
void t_eglSwapBuffers(GuestThread& t) {
    timescale::note_guest_swap();
    timescale::frame();
    set_ret32(t, 1);
}

#endif

// eglGetProcAddress: any function THUMB implements (GL extensions included).
void t_eglGetProcAddress(GuestThread& t) {
    const char* name = mem().str(arg(t, 0));
    set_ret32(t, name ? thunks::lookup(name).value_or(0) : 0);
}

}  // namespace

namespace thunks {

void register_egl() {
#ifdef __ANDROID__
#define H32_EGL(name) add(#name, t_##name);
    H32_EGL(eglGetDisplay) H32_EGL(eglInitialize) H32_EGL(eglTerminate) H32_EGL(eglQueryString)
    H32_EGL(eglChooseConfig) H32_EGL(eglGetConfigs) H32_EGL(eglGetConfigAttrib)
    H32_EGL(eglCreateWindowSurface) H32_EGL(eglCreatePbufferSurface) H32_EGL(eglDestroySurface)
    H32_EGL(eglCreateContext) H32_EGL(eglDestroyContext) H32_EGL(eglMakeCurrent) H32_EGL(eglSwapBuffers)
    H32_EGL(eglSwapInterval) H32_EGL(eglGetCurrentContext) H32_EGL(eglGetCurrentDisplay) H32_EGL(eglGetCurrentSurface)
    H32_EGL(eglGetError) H32_EGL(eglBindAPI) H32_EGL(eglQueryAPI) H32_EGL(eglWaitGL) H32_EGL(eglWaitClient)
    H32_EGL(eglWaitNative) H32_EGL(eglReleaseThread) H32_EGL(eglQuerySurface) H32_EGL(eglQueryContext)
    H32_EGL(eglSurfaceAttrib)
#undef H32_EGL
#else
    for (const char* n : {"eglInitialize", "eglTerminate", "eglGetConfigAttrib", "eglDestroySurface", "eglDestroyContext",
                          "eglMakeCurrent", "eglSwapInterval", "eglBindAPI", "eglWaitGL", "eglWaitClient", "eglWaitNative",
                          "eglReleaseThread", "eglQuerySurface", "eglQueryContext", "eglSurfaceAttrib", "eglQueryAPI"})
        add(n, t_egl_true);
    for (const char* n : {"eglGetDisplay", "eglCreateWindowSurface", "eglCreatePbufferSurface", "eglCreateContext",
                          "eglGetCurrentContext", "eglGetCurrentDisplay", "eglGetCurrentSurface"})
        add(n, t_egl_handle);
    add("eglChooseConfig", +[](GuestThread& t) { t_egl_configs(t, 2, 4); });
    add("eglGetConfigs", +[](GuestThread& t) { t_egl_configs(t, 1, 3); });
    add("eglGetError", t_eglGetError);
    add("eglQueryString", t_eglQueryString);
    add("eglSwapBuffers", t_eglSwapBuffers);
#endif
    add("eglGetProcAddress", t_eglGetProcAddress);
}

}  // namespace thunks
}  // namespace h32
