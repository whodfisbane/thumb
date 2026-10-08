// Linux test harness: loads a 32-bit Android .so, runs its constructors and
// JNI_OnLoad against a fake JVM, and reports what it did.
//
//   harness [options] <lib.so>
//     -v / -vv        debug / trace logging (trace logs every thunk call)
//     --map A=B       map guest path prefix A to host path B (repeatable)
//     --no-block      don't block ad-related Java methods
//     --boot N        Worms 3: boot the game like GERenderer does and render N frames
//     --game DIR      game data dir (data/, android/obb/..., apk_assets/)
#include <sys/stat.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common.h"
#include "cpu/cpu.h"
#include "fake_jni.h"
#include "jni/jni_bridge.h"
#include "loader/elf_loader.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

using namespace h32;

static int usage() {
    fprintf(stderr, "usage: harness [-v|-vv] [--map GUEST=HOST]... [--no-block] [--game DIR] [--boot N] <lib.so>\n");
    return 2;
}

static constexpr int kWidth = 1280, kHeight = 720;

// Answers the Java methods Worms 3 calls on GERenderer, the way a phone would.
static void install_worms3_java(const std::string& game_dir) {
    fakejni::set_java_handler([game_dir](const fakejni::Method& m, jobject, const jvalue* a, jvalue& r) {
        const std::string& n = m.name;
        if (n == "GetAPKPathMain") {
            r.l = fakejni::new_string("/data/app/com.worms3.app/base.apk");
        } else if (n == "GetExpansionPathMain") {
            r.l = fakejni::new_string("/storage/emulated/0/Android/obb/com.worms3.app/main.90.com.worms3.app.obb");
        } else if (n == "GetPatchPathMain") {
            r.l = fakejni::new_string("");
        } else if (n == "GetAppPath" || n == "GetSDCardPath" || n == "GetInternalStoragePathMain") {
            r.l = fakejni::new_string("/data/data/com.worms3.app/files");
        } else if (n == "GetExternalStoragePath") {
            r.l = fakejni::new_string("/storage/emulated/0/Android/data/com.worms3.app/files");
        } else if (n == "GetScreenWidth") {
            r.i = kWidth;
        } else if (n == "GetScreenHeight") {
            r.i = kHeight;
        } else if (n == "LowResCompensate") {
            r.z = JNI_FALSE;
        } else if (n == "FileSize" || n == "LoadFile") {
            // APK assets/ files, extracted to <game>/apk_assets.
            std::string path = game_dir + "/apk_assets/" + fakejni::string_value(a[0].l);
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                H32_DEBUG("asset %s: not found", path.c_str());
                if (n == "FileSize") r.j = 0; else r.i = 0;
                return true;
            }
            std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (n == "FileSize") {
                r.j = jlong(bytes.size());
            } else {
                fakejni::Arr* arr = fakejni::array(a[1].l);
                size_t n2 = arr ? std::min(arr->data.size(), bytes.size()) : 0;
                if (n2) std::memcpy(arr->data.data(), bytes.data(), n2);
                r.i = jint(n2);
            }
        } else {
            return false;
        }
        return true;
    });
    add_path_mapping("/data/app/com.worms3.app/base.apk", game_dir + "/base.apk");
    add_path_mapping("/data/data/com.worms3.app", game_dir + "/data");
    add_path_mapping("/storage/emulated/0/Android", game_dir + "/android");
    add_path_mapping("/sdcard/Android", game_dir + "/android");
    mkdir((game_dir + "/data").c_str(), 0755);
    mkdir((game_dir + "/data/files").c_str(), 0755);
}

template <class Fn>
static Fn native(const char* key) {
    auto it = fakejni::natives().find(key);
    if (it == fakejni::natives().end()) fatal("native %s was not registered", key);
    return reinterpret_cast<Fn>(it->second.fn);
}

// Mirrors GERenderer's constructor, onSurfaceCreated, onSurfaceChanged and onDrawFrame.
static void boot_worms3(int frames) {
    JNIEnv* env = fakejni::env();
    jobject renderer = fakejni::new_object("com/worms3/app/GERenderer");
    using V_SS = void (*)(JNIEnv*, jobject, jstring, jstring);
    using V_ZZ = void (*)(JNIEnv*, jobject, jboolean, jboolean);
    using V_SII = void (*)(JNIEnv*, jobject, jstring, jint, jint);
    using V_II = void (*)(JNIEnv*, jobject, jint, jint);
    using V = void (*)(JNIEnv*, jobject);

    // Main.onCreate()
    jobject main_activity = fakejni::new_object("com/worms3/app/Main");
    H32_INFO("boot: Main.nativeOnCreateCallback");
    native<V>("com/worms3/app/Main.nativeOnCreateCallback")(env, main_activity);
    // GERenderer constructor
    H32_INFO("boot: nativeSetDeviceID");
    native<V_SS>("com/worms3/app/GERenderer.nativeSetDeviceID")(env, renderer, fakejni::new_string("thumb"),
                                                                fakejni::new_string("harness"));
    H32_INFO("boot: nativeSetExternalStorageState(true, true)");
    native<V_ZZ>("com/worms3/app/GERenderer.nativeSetExternalStorageState")(env, renderer, JNI_TRUE, JNI_TRUE);
    // onSurfaceCreated()
    H32_INFO("boot: nativeInit(\"en\", %d, %d)", kWidth, kHeight);
    native<V_SII>("com/worms3/app/GERenderer.nativeInit")(env, renderer, fakejni::new_string("en"), kWidth, kHeight);
    native<V>("com/worms3/app/GERenderer.nativePurgeGL")(env, renderer);
    native<V>("com/worms3/app/GERenderer.nativeRestoreGL")(env, renderer);
    H32_INFO("boot: nativeResize(%d, %d)", kWidth, kHeight);
    native<V_II>("com/worms3/app/GERenderer.nativeResize")(env, renderer, kWidth, kHeight);
    auto keyboard = native<V>("com/worms3/app/GERenderer.nativeHandleKeyboard");
    auto render = native<V>("com/worms3/app/GERenderer.nativeRender");
    for (int i = 0; i < frames; i++) {
        keyboard(env, renderer);
        render(env, renderer);
        if ((i + 1) % 60 == 0) H32_INFO("boot: rendered %d frames (%zu live JNI handles)", i + 1, jni::live_refs());
    }
    H32_INFO("boot: done, %d frames, %zu live JNI handles", frames, jni::live_refs());
}

int main(int argc, char** argv) {
    std::string lib;
    bool block = true;
    int boot_frames = -1;
    std::string game_dir;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") set_log_level(LogLevel::Debug);
        else if (a == "-vv") set_log_level(LogLevel::Trace);
        else if (a == "--no-block") block = false;
        else if (a == "--list-thunks") {
            GuestThread::global_init();
            for (auto& n : thunks::implemented()) printf("%s\n", n.c_str());
            return 0;
        }
        else if (a == "--boot" && i + 1 < argc) boot_frames = atoi(argv[++i]);
        else if (a == "--game" && i + 1 < argc) game_dir = argv[++i];
        else if (a == "--map" && i + 1 < argc) {
            std::string m = argv[++i];
            auto eq = m.find('=');
            if (eq == std::string::npos) return usage();
            add_path_mapping(m.substr(0, eq), m.substr(eq + 1));
        } else if (a[0] == '-') return usage();
        else lib = a;
    }
    if (lib.empty()) return usage();
    if (!block) jni::set_blocked_methods({});

    GuestThread::global_init();
    jni::init(fakejni::vm());
    if (!game_dir.empty()) install_worms3_java(game_dir);

    Module* m = load_module_file(lib);
    if (!m) return 1;

    {
        jni::EnvScope env(fakejni::env());
        run_constructors(*m);
    }

    auto onload = m->find("JNI_OnLoad");
    if (!onload) {
        H32_WARN("%s has no JNI_OnLoad", m->name.c_str());
    } else {
        jint v = jni::call_JNI_OnLoad(*onload, fakejni::env());
        printf("\nJNI_OnLoad returned 0x%x\n", v);
    }

    if (boot_frames >= 0) {
        if (game_dir.empty()) return usage();
        boot_worms3(boot_frames);
    }

    auto natives = jni::registered_natives();
    printf("\n=== %zu native methods registered ===\n", natives.size());
    for (auto& n : natives) printf("  %s.%s %s\n", n.class_name.c_str(), n.name.c_str(), n.signature.c_str());

    printf("\n=== Java methods called by native code ===\n");
    for (auto& [k, v] : fakejni::java_calls()) printf("  %4d  %s\n", v, k.c_str());
    printf("\n");
    thunks::dump_stats(25);
    return 0;
}
