// <dlfcn.h> for guest code: dlopen/dlsym/dlclose/dlerror.
//
// Handles are small integers in an unmapped range:
//   kHandleDefault  RTLD_DEFAULT-style global lookup (also dlopen(NULL))
//   kHandleSystem   any Android system library (resolved via THUMB's thunks)
//   kHandleModule+i guest module i (an app library, loaded on demand)
#include <mutex>
#include <string>
#include <vector>

#include "loader/elf_loader.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

constexpr gaddr kHandleDefault = 0xF8000000;
constexpr gaddr kHandleSystem = 0xF8000004;
constexpr gaddr kHandleModule = 0xF8001000;

std::mutex g_lock;
std::vector<Module*> g_handles;  // index -> module

thread_local std::string t_error;
thread_local bool t_error_pending = false;

void set_error(std::string msg) {
    H32_DEBUG("dl: %s", msg.c_str());
    t_error = std::move(msg);
    t_error_pending = true;
}

gaddr handle_for(Module* m) {
    std::lock_guard lk(g_lock);
    for (size_t i = 0; i < g_handles.size(); i++)
        if (g_handles[i] == m) return kHandleModule + gaddr(i) * 4;
    g_handles.push_back(m);
    return kHandleModule + gaddr(g_handles.size() - 1) * 4;
}

Module* module_for(gaddr h) {
    std::lock_guard lk(g_lock);
    size_t i = (h - kHandleModule) / 4;
    return h >= kHandleModule && i < g_handles.size() ? g_handles[i] : nullptr;
}

std::string base_name(const char* path) {
    std::string s = path;
    auto slash = s.find_last_of('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

// Libraries every Android app can load: their functions come from thunks.
bool is_system_library(const std::string& name) {
    static const char* const kSystem[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libstdc++.so", "libandroid.so", "libjnigraphics.so",
        "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so", "libOpenMAXAL.so",
        "libaaudio.so", "libvulkan.so", "libmediandk.so", "libcamera2ndk.so", "libnativewindow.so", "libsync.so",
    };
    for (const char* s : kSystem)
        if (name == s) return true;
    return false;
}

// void* dlopen(const char* filename, int flags)
void t_dlopen(GuestThread& t) {
    const char* path = mem().str(t.regs()[0]);
    if (!path) return set_ret32(t, kHandleDefault);
    std::string name = base_name(path);
    if (is_system_library(name)) return set_ret32(t, kHandleSystem);

    Module* m = find_module(name);
    if (!m) {
        // Look next to the app's other libraries.
        for (Module* other : all_modules()) {
            if (other->dir.empty()) continue;
            if ((m = load_sibling(other->dir, name))) break;
        }
    }
    if (!m) {
        set_error(std::string("dlopen failed: library \"") + path + "\" not found");
        return set_ret32(t, 0);
    }
    run_constructors(*m);
    set_ret32(t, handle_for(m));
}

// void* dlsym(void* handle, const char* symbol)
void t_dlsym(GuestThread& t) {
    gaddr h = t.regs()[0];
    const char* sym = mem().str(t.regs()[1]);
    if (!sym) return set_ret32(t, 0);
    std::optional<gaddr> found;
    if (Module* m = module_for(h)) {
        found = m->find(sym);
    } else if (h == kHandleSystem) {
        found = thunks::lookup(sym);
    } else {  // RTLD_DEFAULT (0), RTLD_NEXT (-1), dlopen(NULL) and unknown handles: search everything
        for (Module* m : all_modules())
            if ((found = m->find(sym))) break;
        if (!found) found = thunks::lookup(sym);
    }
    if (!found || !*found) {
        set_error(std::string("undefined symbol: ") + sym);
        return set_ret32(t, 0);
    }
    set_ret32(t, *found);
}

void t_dlclose(GuestThread& t) { set_ret32(t, 0); }  // libraries stay loaded

// char* dlerror(void) — returns the last error once, then NULL.
void t_dlerror(GuestThread& t) {
    if (!t_error_pending) return set_ret32(t, 0);
    t_error_pending = false;
    gaddr buf = thread_scratch(kScratchDlerror, t_error.size() + 1);
    std::memcpy(mem().ptr<char>(buf), t_error.c_str(), t_error.size() + 1);
    set_ret32(t, buf);
}

}  // namespace

namespace thunks {

void register_dl() {
    add("dlopen", t_dlopen);
    add("android_dlopen_ext", t_dlopen);
    add("dlsym", t_dlsym);
    add("dlclose", t_dlclose);
    add("dlerror", t_dlerror);
}

}  // namespace thunks
}  // namespace h32
