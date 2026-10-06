// Starts THUMB's in-game menu (dev.thumb.overlay.ThumbOverlay, added to the
// patched app as an extra dex) and provides its native methods.
#include <jni.h>

#include <cstdlib>
#include <string>

#include "common.h"
#include "compat.h"
#include "jni/jni_bridge.h"
#include "thunks/timescale.h"

jobject thumb_current_application(JNIEnv* env);  // shim/obb_handover.cpp

namespace {

void JNICALL set_speed(JNIEnv*, jclass, jfloat speed) { h32::timescale::set_scale(speed); }
jfloat JNICALL get_fps(JNIEnv*, jclass) { return jfloat(h32::timescale::fps()); }
void JNICALL set_fps_limit(JNIEnv*, jclass, jint fps) { h32::timescale::set_fps_limit(fps); }
void JNICALL set_ad_block(JNIEnv*, jclass, jboolean on) { h32::jni::set_ad_block(on); }

// Reads assets/<path> of the running app into a string ("" if absent).
std::string read_asset(JNIEnv* env, jobject app, const char* path) {
    jclass ctx = env->FindClass("android/content/Context");
    jobject assets = env->CallObjectMethod(app, env->GetMethodID(ctx, "getAssets", "()Landroid/content/res/AssetManager;"));
    jclass am = env->FindClass("android/content/res/AssetManager");
    jobject in = env->CallObjectMethod(assets, env->GetMethodID(am, "open", "(Ljava/lang/String;)Ljava/io/InputStream;"), env->NewStringUTF(path));
    if (env->ExceptionCheck() || !in) {
        env->ExceptionClear();
        return {};
    }
    jclass is = env->FindClass("java/io/InputStream");
    jmethodID read = env->GetMethodID(is, "read", "([B)I");
    jbyteArray buf = env->NewByteArray(4096);
    std::string out;
    for (jint n; (n = env->CallIntMethod(in, read, buf)) > 0;) {
        std::string chunk(size_t(n), '\0');
        env->GetByteArrayRegion(buf, 0, n, reinterpret_cast<jbyte*>(chunk.data()));
        out += chunk;
    }
    env->ExceptionClear();
    env->CallVoidMethod(in, env->GetMethodID(is, "close", "()V"));
    env->ExceptionClear();
    return out;
}

}  // namespace

void thumb_start_overlay(JNIEnv* env) {
    static bool done = false;
    if (done) return;
    done = true;
    env->PushLocalFrame(32);
    jobject app = thumb_current_application(env);
    std::string options = app ? read_asset(env, app, "thumb/options.json") : "";
    if (options.empty()) {  // patched without the THUMB app (tools/repack.py): no overlay
        env->PopLocalFrame(nullptr);
        return;
    }
    // Build options that apply with or without the overlay (top-level keys of
    // a small JSON object written by THUMB).
    std::string compact;
    for (char c : options) if (c != ' ' && c != '\n' && c != '\t') compact += c;
    auto value_at = [&](const char* key) -> std::string::size_type {
        std::string k = std::string("\"") + key + "\":";
        auto at = compact.find(k);
        return at == std::string::npos ? at : at + k.size();
    };
    auto flag = [&](const char* key, bool fallback) {
        auto at = value_at(key);
        return at == std::string::npos ? fallback : compact.compare(at, 4, "true") == 0;
    };
    auto number = [&](const char* key, int fallback) {
        auto at = value_at(key);
        return at == std::string::npos ? fallback : std::atoi(compact.c_str() + at);
    };
    h32::jni::set_ad_block(flag("adblock", true));
    // FPS build mode: "compat" (default) caps at 60 for old game physics;
    // "default" leaves the frame rate alone. The overlay's FPS unlock can
    // change it while playing.
    {
        auto at = value_at("fps_mode");
        bool compat = at == std::string::npos || compact.compare(at, 8, "\"compat\"") == 0;
        h32::timescale::set_fps_limit(compat ? 60 : 0);
    }
    h32::compat::legacy_fs = flag("legacy_fs", true);
    if (!flag("overlay", false)) {
        env->PopLocalFrame(nullptr);
        return;
    }
    jclass overlay = env->FindClass("dev/thumb/overlay/ThumbOverlay");
    if (env->ExceptionCheck() || !overlay) {
        env->ExceptionClear();
        H32_DEBUG("overlay: not included in this app");
        env->PopLocalFrame(nullptr);
        return;
    }
    const JNINativeMethod natives[] = {
        {"nativeSetSpeed", "(F)V", reinterpret_cast<void*>(set_speed)},
        {"nativeGetFps", "()F", reinterpret_cast<void*>(get_fps)},
        {"nativeSetAdBlock", "(Z)V", reinterpret_cast<void*>(set_ad_block)},
        {"nativeSetFpsLimit", "(I)V", reinterpret_cast<void*>(set_fps_limit)},
    };
    if (env->RegisterNatives(overlay, natives, 4) != JNI_OK) {
        env->ExceptionClear();
        H32_ERROR("overlay: RegisterNatives failed");
        env->PopLocalFrame(nullptr);
        return;
    }
    jmethodID install = env->GetStaticMethodID(overlay, "install", "(Landroid/app/Application;Ljava/lang/String;)V");
    env->CallStaticVoidMethod(overlay, install, app, env->NewStringUTF(options.c_str()));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else {
        H32_INFO("overlay: started (%s)", options.c_str());
    }
    env->PopLocalFrame(nullptr);
}
