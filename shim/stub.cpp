// Stub that stands in for one of a game's 32-bit libraries. The same binary is
// installed under every original name (libWorms3Android.so, libgvradio.so...):
// it finds its own file name and asks the runtime to load "<name>_arm32.so"
// from the same directory.
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#include <cstring>
#include <string>

namespace {

using LoadFn = jint (*)(JavaVM*, const char*, const char*);

void log_error(const char* msg, const char* detail) {
    __android_log_print(ANDROID_LOG_ERROR, "host32", "stub: %s %s", msg, detail ? detail : "");
}

}  // namespace

extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM* vm, void*) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&JNI_OnLoad), &info) || !info.dli_fname) {
        log_error("cannot find own path", nullptr);
        return JNI_ERR;
    }
    std::string self = info.dli_fname;  // .../lib/arm64/libWorms3Android.so
    size_t slash = self.find_last_of('/');
    std::string dir = self.substr(0, slash);
    std::string name = self.substr(slash + 1);
    std::string guest = name.substr(0, name.size() - 3) + "_arm32.so";

    void* rt = dlopen("libarm32revive.so", RTLD_NOW | RTLD_GLOBAL);
    if (!rt) {
        log_error("cannot load libarm32revive.so:", dlerror());
        return JNI_ERR;
    }
    auto load = reinterpret_cast<LoadFn>(dlsym(rt, "revive_load"));
    if (!load) {
        log_error("revive_load missing", nullptr);
        return JNI_ERR;
    }
    return load(vm, dir.c_str(), guest.c_str());
}
