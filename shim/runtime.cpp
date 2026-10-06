// libthumb.so — the translator runtime on Android. Stub libraries that
// replace a game's 32-bit .so files call thumb_load() from their JNI_OnLoad.
#include <dlfcn.h>
#include <jni.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "cpu/cpu.h"
#include "jni/jni_bridge.h"
#include "loader/elf_loader.h"

using namespace h32;

// Loads <dir>/<guest_name> (an original armeabi-v7a library), runs its
// constructors and JNI_OnLoad, and returns the JNI version it reports.
extern "C" __attribute__((visibility("default"))) jint thumb_load(JavaVM* vm, const char* dir, const char* guest_name) {
    static std::mutex m;
    std::lock_guard lk(m);

    H32_INFO("thumb_load(%s/%s)", dir, guest_name);
    GuestThread::global_init();
    jni::init(vm);

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) {
        H32_ERROR("thumb_load: no JNIEnv on the loading thread");
        return JNI_ERR;
    }

    // The guest should see itself under its original name: libfoo_arm32.so -> libfoo.so
    std::string original = guest_name;
    if (auto pos = original.rfind("_arm32.so"); pos != std::string::npos) original.replace(pos, 9, ".so");

    if (Module* existing = find_module(original)) {
        H32_WARN("%s already loaded", guest_name);
        (void)existing;
        return JNI_VERSION_1_6;
    }

    Module* mod = load_module_file(std::string(dir) + "/" + guest_name, original);
    if (!mod) {
        H32_ERROR("thumb_load: could not load %s", guest_name);
        return JNI_ERR;
    }

    jni::EnvScope scope(env);
    run_constructors(*mod);
    // Natives bound by symbol name: Java can't find them in our stub, so
    // register them explicitly.
    std::vector<std::pair<std::string, gaddr>> java_exports;
    for (auto& [name, addr] : mod->exports)
        if (name.compare(0, 5, "Java_") == 0) java_exports.emplace_back(name, addr);
    if (!java_exports.empty())
        H32_INFO("%s: registered %d of %zu exported Java_ natives", original.c_str(), jni::register_java_exports(env, java_exports),
                 java_exports.size());

    auto onload = mod->find("JNI_OnLoad");
    if (!onload) return JNI_VERSION_1_6;
    return jni::call_JNI_OnLoad(*onload, env);
}
