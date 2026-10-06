// OBB handover: the THUMB app can't write into a game's Android/obb folder,
// so it keeps imported OBBs privately and grants the game read access. On the
// first library load, this copies the OBB into the game's own OBB folder
// (which the game may write) and asks THUMB to delete its copy.
//
// Runs in the patched app's process with the host (64-bit) JNIEnv.
#include <jni.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common.h"

namespace {

constexpr const char* kAuthority = "dev.thumb.app.obb";

bool clear(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

std::string to_string(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    env->ReleaseStringUTFChars(s, c);
    return out;
}

}  // namespace

jobject thumb_current_application(JNIEnv* env);

namespace {

jobject current_application(JNIEnv* env) { return thumb_current_application(env); }

}  // namespace

jobject thumb_current_application(JNIEnv* env) {
    jclass at = env->FindClass("android/app/ActivityThread");
    if (clear(env) || !at) return nullptr;
    jmethodID m = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
    if (clear(env) || !m) return nullptr;
    jobject app = env->CallStaticObjectMethod(at, m);
    return clear(env) ? nullptr : app;
}

namespace {
}  // namespace

// Copies content://dev.thumb.app.obb/<pkg>/main.<versionCode>.<pkg>.obb into the
// app's OBB folder if it isn't there yet. Safe to call more than once.
void thumb_obb_handover(JNIEnv* env) {
    static bool done = false;
    if (done) return;
    done = true;

    env->PushLocalFrame(64);
    jobject app = current_application(env);
    if (!app) {
        H32_DEBUG("obb: no Application yet, skipping handover");
        env->PopLocalFrame(nullptr);
        return;
    }
    jclass ctx_cls = env->FindClass("android/content/Context");
    jstring jpkg = static_cast<jstring>(env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getPackageName", "()Ljava/lang/String;")));
    jobject obb_dir = env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getObbDir", "()Ljava/io/File;"));
    if (clear(env) || !jpkg || !obb_dir) {
        env->PopLocalFrame(nullptr);
        return;
    }
    std::string pkg = to_string(env, jpkg);
    jclass file_cls = env->FindClass("java/io/File");
    std::string dir = to_string(env, static_cast<jstring>(env->CallObjectMethod(obb_dir, env->GetMethodID(file_cls, "getAbsolutePath", "()Ljava/lang/String;"))));

    // Version code -> the OBB name Android games expect.
    jobject pm = env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    jclass pm_cls = env->FindClass("android/content/pm/PackageManager");
    jobject info = env->CallObjectMethod(pm, env->GetMethodID(pm_cls, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;"), jpkg, 0);
    if (clear(env) || !info) {
        env->PopLocalFrame(nullptr);
        return;
    }
    jclass pi_cls = env->FindClass("android/content/pm/PackageInfo");
    jlong version = env->CallLongMethod(info, env->GetMethodID(pi_cls, "getLongVersionCode", "()J"));
    std::string name = "main." + std::to_string(version) + "." + pkg + ".obb";
    std::string target = dir + "/" + name;

    struct stat st;
    if (stat(target.c_str(), &st) == 0 && st.st_size > 0) {
        env->PopLocalFrame(nullptr);
        return;  // already in place
    }

    // content://dev.thumb.app.obb/<pkg>/<name>
    jclass uri_cls = env->FindClass("android/net/Uri");
    std::string uri_str = std::string("content://") + kAuthority + "/" + pkg + "/" + name;
    jobject uri = env->CallStaticObjectMethod(uri_cls, env->GetStaticMethodID(uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                              env->NewStringUTF(uri_str.c_str()));
    jobject resolver = env->CallObjectMethod(app, env->GetMethodID(ctx_cls, "getContentResolver", "()Landroid/content/ContentResolver;"));
    jclass cr_cls = env->FindClass("android/content/ContentResolver");
    jobject in = env->CallObjectMethod(resolver, env->GetMethodID(cr_cls, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;"), uri);
    if (env->ExceptionCheck() || !in) {
        env->ExceptionClear();  // nothing imported in THUMB for this app: the normal case
        H32_DEBUG("obb: THUMB has no %s to hand over", name.c_str());
        env->PopLocalFrame(nullptr);
        return;
    }

    H32_INFO("obb: receiving %s from THUMB", name.c_str());
    ::mkdir(dir.c_str(), 0770);
    std::string part = target + ".part";
    FILE* out = std::fopen(part.c_str(), "wb");
    jclass is_cls = env->FindClass("java/io/InputStream");
    jmethodID read = env->GetMethodID(is_cls, "read", "([B)I");
    jmethodID close = env->GetMethodID(is_cls, "close", "()V");
    bool ok = out != nullptr;
    long long total = 0;
    if (ok) {
        constexpr jsize kChunk = 1 << 20;
        jbyteArray buf = env->NewByteArray(kChunk);
        std::vector<jbyte> host(kChunk);
        while (true) {
            jint n = env->CallIntMethod(in, read, buf);
            if (clear(env)) {
                ok = false;
                break;
            }
            if (n < 0) break;
            env->GetByteArrayRegion(buf, 0, n, host.data());
            if (std::fwrite(host.data(), 1, size_t(n), out) != size_t(n)) {
                ok = false;
                break;
            }
            total += n;
        }
        ok = (std::fclose(out) == 0) && ok;
    }
    env->CallVoidMethod(in, close);
    clear(env);
    if (!ok || total == 0 || std::rename(part.c_str(), target.c_str()) != 0) {
        H32_ERROR("obb: handover of %s failed", name.c_str());
        std::remove(part.c_str());
        env->PopLocalFrame(nullptr);
        return;
    }
    H32_INFO("obb: %s in place (%lld MB)", name.c_str(), total >> 20);

    // Free THUMB's copy.
    env->CallIntMethod(resolver, env->GetMethodID(cr_cls, "delete", "(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I"), uri, nullptr, nullptr);
    clear(env);
    env->PopLocalFrame(nullptr);
}
