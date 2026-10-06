// JNI bridge between 32-bit guest code and the 64-bit host JVM (ART on the
// phone, a fake JNIEnv in the Linux harness).
//
// - The guest sees its own JavaVM/JNIEnv whose function tables point at
//   thunk stubs ("JNI::FindClass", ...). Most slots are forwarded
//   automatically; Call*Method & co. decode varargs using the signature
//   recorded at GetMethodID time.
// - Host references (64-bit pointers) are swapped for 32-bit handles.
// - RegisterNatives from the guest installs libffi trampolines on the host,
//   so Java calls land in guest code.
// - Methods whose names match a block pattern (ads, by default) are never
//   called; the guest gets a zero/false/null result instead.
#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "jni.h"

namespace h32::jni {

void init(JavaVM* host_vm);
gaddr guest_vm();

// Marks `env` as the current thread's JNIEnv while guest code runs.
class EnvScope {
public:
    explicit EnvScope(JNIEnv* env);
    ~EnvScope();
    EnvScope(const EnvScope&) = delete;
    EnvScope& operator=(const EnvScope&) = delete;

private:
    JNIEnv* prev_;
};

// Calls a guest JNI_OnLoad(JavaVM*, void*) and returns its result.
jint call_JNI_OnLoad(gaddr fn, JNIEnv* env);

// Registers exported "Java_<class>_<method>" functions (the pre-RegisterNatives
// way of binding natives) with the JVM; signatures come from reflection.
// Returns how many were registered.
int register_java_exports(JNIEnv* env, const std::vector<std::pair<std::string, gaddr>>& exports);

// Turns blocking of ad-related Java methods on or off at runtime.
void set_ad_block(bool on);

// Java methods whose name contains any of these substrings are blocked.
void set_blocked_methods(std::vector<std::string> patterns);

struct RegisteredNative {
    std::string class_name, name, signature;
    gaddr guest_fn;
    void* host_fn;  // libffi trampoline registered with the JVM
};
std::vector<RegisteredNative> registered_natives();

}  // namespace h32::jni
