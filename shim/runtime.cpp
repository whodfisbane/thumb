// libarm32revive.so — the translator runtime on Android. Stub libraries that
// replace a game's 32-bit .so files call revive_load() from their JNI_OnLoad.
#include <dlfcn.h>
#include <jni.h>

#include <mutex>
#include <string>

#include "common.h"
#include "cpu/cpu.h"
#include "jni/jni_bridge.h"
#include "loader/elf_loader.h"

using namespace h32;

// Loads <dir>/<guest_name> (an original armeabi-v7a library), runs its
// constructors and JNI_OnLoad, and returns the JNI version it reports.
extern "C" __attribute__((visibility("default"))) jint revive_load(JavaVM* vm, const char* dir, const char* guest_name) {
    static std::mutex m;
    std::lock_guard lk(m);

    H32_INFO("revive_load(%s/%s)", dir, guest_name);
    GuestThread::global_init();
    jni::init(vm);

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        H32_ERROR("revive_load: no JNIEnv on the loading thread");
        return JNI_ERR;
    }

    if (Module* existing = find_module(guest_name)) {
        H32_WARN("%s already loaded", guest_name);
        (void)existing;
        return JNI_VERSION_1_6;
    }

    Module* mod = load_module_file(std::string(dir) + "/" + guest_name);
    if (!mod) {
        H32_ERROR("revive_load: could not load %s", guest_name);
        return JNI_ERR;
    }

    jni::EnvScope scope(env);
    run_constructors(*mod);
    auto onload = mod->find("JNI_OnLoad");
    if (!onload) return JNI_VERSION_1_6;
    return jni::call_JNI_OnLoad(*onload, env);
}
