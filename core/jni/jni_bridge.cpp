#include "jni/jni_bridge.h"

#include <ffi.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu/cpu.h"
#include "memory/arena.h"
#include "thunks/libc_internal.h"
#include "thunks/thunks.h"
#include "thunks/wrap.h"

namespace h32::jni {

namespace {

// ------------------------------------------------------------ handles ----

class HandleTable {
public:
    uint32_t to_guest(void* p) {
        if (!p) return 0;
        std::lock_guard lk(m_);
        auto [it, inserted] = index_.try_emplace(p, uint32_t(values_.size()));
        if (inserted) values_.push_back(p);
        return encode(it->second);
    }
    void* to_host(uint32_t h) {
        if (!h) return nullptr;
        uint32_t i = decode(h);
        std::lock_guard lk(m_);
        if (i >= values_.size()) {
            H32_ERROR("jni: invalid guest handle 0x%08x", h);
            return nullptr;
        }
        return values_[i];
    }

private:
    // Handles look like aligned pointers in an unmapped range, which helps
    // when reading guest crash dumps.
    static uint32_t encode(uint32_t i) { return 0xF0000000u + i * 4; }
    static uint32_t decode(uint32_t h) { return (h - 0xF0000000u) / 4; }
    std::mutex m_;
    std::vector<void*> values_{nullptr};
    std::unordered_map<void*, uint32_t> index_;
};

HandleTable g_refs, g_ids;

// --------------------------------------------------------- method info ----

struct MethodInfo {
    std::string name, sig;
    std::vector<char> args;  // one JNI type char per parameter ('L' for objects/arrays)
    char ret = 'V';
    bool blocked = false;
};

std::mutex g_methods_mutex;
std::unordered_map<jmethodID, MethodInfo> g_methods;
std::vector<std::string> g_block_patterns = {"Chartboost", "AdColony", "AdMob", "Interstitial"};

bool parse_signature(const char* sig, std::vector<char>& args, char& ret) {
    if (!sig || *sig != '(') return false;
    const char* p = sig + 1;
    while (*p && *p != ')') {
        char c = *p;
        if (c == '[') {
            while (*p == '[') p++;
            if (*p == 'L') while (*p && *p != ';') p++;
            p++;
            args.push_back('L');
        } else if (c == 'L') {
            while (*p && *p != ';') p++;
            p++;
            args.push_back('L');
        } else {
            args.push_back(c);
            p++;
        }
    }
    if (*p != ')') return false;
    ret = p[1] == '[' ? 'L' : p[1];
    return true;
}

void record_method(jmethodID id, const char* name, const char* sig) {
    if (!id) return;
    MethodInfo mi;
    mi.name = name ? name : "";
    mi.sig = sig ? sig : "";
    if (!parse_signature(sig, mi.args, mi.ret)) H32_WARN("jni: cannot parse method signature '%s'", mi.sig.c_str());
    std::lock_guard lk(g_methods_mutex);
    for (auto& pat : g_block_patterns)
        if (mi.name.find(pat) != std::string::npos) {
            mi.blocked = true;
            H32_INFO("jni: blocking calls to %s%s (matches \"%s\")", mi.name.c_str(), mi.sig.c_str(), pat.c_str());
        }
    g_methods[id] = std::move(mi);
}

const MethodInfo* method_info(jmethodID id) {
    std::lock_guard lk(g_methods_mutex);
    auto it = g_methods.find(id);
    return it == g_methods.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------ env/vm ----

JavaVM* g_host_vm = nullptr;
gaddr g_guest_vm = 0, g_guest_env = 0;
thread_local JNIEnv* t_env = nullptr;

}  // namespace

void* ref_to_host(uint32_t h) { return g_refs.to_host(h); }
uint32_t ref_to_guest(void* r) { return g_refs.to_guest(r); }
void* id_to_host(uint32_t h) { return g_ids.to_host(h); }
uint32_t id_to_guest(void* id) { return g_ids.to_guest(id); }
uint32_t guest_env() { return g_guest_env; }
gaddr guest_vm() { return g_guest_vm; }

JNIEnv* host_env() {
    if (!t_env && g_host_vm) {
        H32_DEBUG("jni: attaching guest-created thread to the JVM");
        g_host_vm->AttachCurrentThread(&t_env, nullptr);
    }
    if (!t_env) H32_ERROR("jni: no JNIEnv on this thread");
    return t_env;
}

EnvScope::EnvScope(JNIEnv* env) : prev_(t_env) { t_env = env; }
EnvScope::~EnvScope() { t_env = prev_; }

void set_blocked_methods(std::vector<std::string> patterns) {
    std::lock_guard lk(g_methods_mutex);
    g_block_patterns = std::move(patterns);
}

namespace {

// ------------------------------------------------- automatic forwarding ----

template <auto M>
struct AutoSlot;

template <class R, class... A, R (*JNINativeInterface::*M)(JNIEnv*, A...)>
struct AutoSlot<M> {
    static R call(JNIEnv* env, A... a) { return (env->functions->*M)(env, a...); }
    static void thunk(GuestThread& t) { Wrapper<JniAbi, R(JNIEnv*, A...)>::template thunk<&call>(t); }
};

// ---------------------------------------------- Call*Method / NewObject ----

// Reads one argument of JNI type `c` from C varargs or a va_list. Varargs
// promote float to double, and 64-bit values sit in aligned pairs.
jvalue read_vararg(VarArgs& va, char c) {
    jvalue v{};
    switch (c) {
    case 'Z': v.z = jboolean(va.word()); break;
    case 'B': v.b = jbyte(va.word()); break;
    case 'C': v.c = jchar(va.word()); break;
    case 'S': v.s = jshort(va.word()); break;
    case 'I': v.i = jint(va.word()); break;
    case 'J': v.j = jlong(va.dword()); break;
    case 'F': v.f = float(va.f64()); break;
    case 'D': v.d = va.f64(); break;
    default: v.l = static_cast<jobject>(ref_to_host(va.word())); break;
    }
    return v;
}

// Reads one element of a guest jvalue[] (8 bytes per element).
jvalue read_jvalue(gaddr a, char c) {
    auto& m = mem();
    jvalue v{};
    switch (c) {
    case 'Z': v.z = m.read<uint8_t>(a); break;
    case 'B': v.b = m.read<int8_t>(a); break;
    case 'C': v.c = m.read<uint16_t>(a); break;
    case 'S': v.s = m.read<int16_t>(a); break;
    case 'I': v.i = m.read<int32_t>(a); break;
    case 'J': v.j = m.read<int64_t>(a); break;
    case 'F': v.f = m.read<float>(a); break;
    case 'D': v.d = m.read<double>(a); break;
    default: v.l = static_cast<jobject>(ref_to_host(m.read<uint32_t>(a))); break;
    }
    return v;
}

#define H32_INVOKE_TYPED(R, Name, field)                                                           \
    case R:                                                                                        \
        switch (target) {                                                                          \
        case 'i': r.field = f->Call##Name##MethodA(env, obj, mid, a); break;                       \
        case 'n': r.field = f->CallNonvirtual##Name##MethodA(env, obj, cls, mid, a); break;        \
        default: r.field = f->CallStatic##Name##MethodA(env, cls, mid, a); break;                  \
        }                                                                                          \
        break;

jvalue invoke(JNIEnv* env, char target, char ret, jobject obj, jclass cls, jmethodID mid, const jvalue* a) {
    const JNINativeInterface* f = env->functions;
    jvalue r{};
    if (target == 'o') {
        r.l = f->NewObjectA(env, cls, mid, a);
        return r;
    }
    switch (ret) {
        H32_INVOKE_TYPED('L', Object, l)
        H32_INVOKE_TYPED('Z', Boolean, z)
        H32_INVOKE_TYPED('B', Byte, b)
        H32_INVOKE_TYPED('C', Char, c)
        H32_INVOKE_TYPED('S', Short, s)
        H32_INVOKE_TYPED('I', Int, i)
        H32_INVOKE_TYPED('J', Long, j)
        H32_INVOKE_TYPED('F', Float, f)
        H32_INVOKE_TYPED('D', Double, d)
    default:
        switch (target) {
        case 'i': f->CallVoidMethodA(env, obj, mid, a); break;
        case 'n': f->CallNonvirtualVoidMethodA(env, obj, cls, mid, a); break;
        default: f->CallStaticVoidMethodA(env, cls, mid, a); break;
        }
    }
    return r;
}
#undef H32_INVOKE_TYPED

void put_jvalue(GuestThread& t, char ret, jvalue v) {
    switch (ret) {
    case 'Z': set_ret32(t, v.z); break;
    case 'B': set_ret32(t, uint32_t(int32_t(v.b))); break;
    case 'C': set_ret32(t, v.c); break;
    case 'S': set_ret32(t, uint32_t(int32_t(v.s))); break;
    case 'I': set_ret32(t, uint32_t(v.i)); break;
    case 'J': set_ret64(t, uint64_t(v.j)); break;
    case 'F': { uint32_t w; std::memcpy(&w, &v.f, 4); set_ret32(t, w); break; }
    case 'D': { uint64_t w; std::memcpy(&w, &v.d, 8); set_ret64(t, w); break; }
    case 'L': set_ret32(t, ref_to_guest(v.l)); break;
    default: break;
    }
}

template <char Target, int Form, char Ret>
void call_slot(GuestThread& t) {
    JNIEnv* env = host_env();
    ArgCursor c{t, 1};  // skip env
    jobject obj = nullptr;
    jclass cls = nullptr;
    if constexpr (Target == 'i') obj = static_cast<jobject>(ref_to_host(c.word()));
    if constexpr (Target == 'n') {
        obj = static_cast<jobject>(ref_to_host(c.word()));
        cls = static_cast<jclass>(ref_to_host(c.word()));
    }
    if constexpr (Target == 's' || Target == 'o') cls = static_cast<jclass>(ref_to_host(c.word()));
    uint32_t mid_handle = c.word();
    auto mid = static_cast<jmethodID>(id_to_host(mid_handle));

    const MethodInfo* mi = method_info(mid);
    static const MethodInfo kUnknown{};
    if (!mi) {
        H32_ERROR("jni: call (%c/%d/%c) through unknown jmethodID 0x%08x (host %p) from %s; assuming no arguments", Target,
                  Form, Ret, mid_handle, (void*)mid, describe_address(t.regs()[14]).c_str());
        mi = &kUnknown;
    }
    std::vector<jvalue> a(mi->args.size());
    if constexpr (Form == 2) {
        gaddr arr = c.word();
        for (size_t i = 0; i < a.size(); i++) a[i] = read_jvalue(arr + gaddr(i * 8), mi->args[i]);
    } else if constexpr (Form == 1) {
        VarArgs va(t, c.word());
        for (size_t i = 0; i < a.size(); i++) a[i] = read_vararg(va, mi->args[i]);
    } else {
        VarArgs va(c);
        for (size_t i = 0; i < a.size(); i++) a[i] = read_vararg(va, mi->args[i]);
    }
    if (mi->blocked) {
        H32_DEBUG("jni: blocked call to %s", mi->name.c_str());
        return put_jvalue(t, Ret, jvalue{});
    }
    H32_TRACE("jni: call %s%s", mi->name.c_str(), mi->sig.c_str());
    put_jvalue(t, Ret, invoke(env, Target, Ret, obj, cls, mid, a.data()));
}

// ------------------------------------------------------ manual slots ----

template <jmethodID (*JNINativeInterface::*Get)(JNIEnv*, jclass, const char*, const char*)>
void t_get_method_id(GuestThread& t) {
    JNIEnv* env = host_env();
    auto cls = static_cast<jclass>(ref_to_host(t.regs()[1]));
    const char* name = mem().str(t.regs()[2]);
    const char* sig = mem().str(t.regs()[3]);
    jmethodID id = (env->functions->*Get)(env, cls, name, sig);
    record_method(id, name, sig);
    H32_DEBUG("jni: GetMethodID(%s, %s) -> %p", name, sig, (void*)id);
    set_ret32(t, id_to_guest(id));
}

void t_get_string_utf_chars(GuestThread& t) {
    JNIEnv* env = host_env();
    auto s = static_cast<jstring>(ref_to_host(t.regs()[1]));
    if (gaddr is_copy = t.regs()[2]) mem().write<uint8_t>(is_copy, JNI_TRUE);
    const char* h = env->GetStringUTFChars(s, nullptr);
    if (!h) return set_ret32(t, 0);
    gaddr g = mem().strdup(h);
    env->ReleaseStringUTFChars(s, h);
    set_ret32(t, g);
}

void t_get_string_chars(GuestThread& t) {
    JNIEnv* env = host_env();
    auto s = static_cast<jstring>(ref_to_host(t.regs()[1]));
    if (gaddr is_copy = t.regs()[2]) mem().write<uint8_t>(is_copy, JNI_TRUE);
    jsize n = env->GetStringLength(s);
    gaddr g = mem().malloc(size_t(n + 1) * 2);
    env->GetStringRegion(s, 0, n, mem().ptr<jchar>(g));
    mem().write<uint16_t>(g + gaddr(n) * 2, 0);
    set_ret32(t, g);
}

// Release*Chars(env, string, chars): the guest copy is all that remains.
void t_release_chars(GuestThread& t) { mem().free(t.regs()[2]); }

// Get<T>ArrayElements: copy into guest memory; Release copies back.
struct PinnedArray {
    void* host;  // host element pointer (from Get<T>ArrayElements)
    size_t bytes;
};
std::mutex g_pinned_mutex;
std::unordered_map<gaddr, PinnedArray> g_pinned;

template <class T, class Arr, T* (*JNINativeInterface::*Get)(JNIEnv*, Arr, jboolean*)>
void t_get_elements(GuestThread& t) {
    JNIEnv* env = host_env();
    auto arr = static_cast<Arr>(ref_to_host(t.regs()[1]));
    if (gaddr is_copy = t.regs()[2]) mem().write<uint8_t>(is_copy, JNI_TRUE);
    T* h = (env->functions->*Get)(env, arr, nullptr);
    if (!h) return set_ret32(t, 0);
    size_t bytes = size_t(env->GetArrayLength(arr)) * sizeof(T);
    gaddr g = mem().malloc(bytes);
    std::memcpy(mem().ptr<void>(g), h, bytes);
    {
        std::lock_guard lk(g_pinned_mutex);
        g_pinned[g] = {h, bytes};
    }
    set_ret32(t, g);
}

template <class T, class Arr, void (*JNINativeInterface::*Rel)(JNIEnv*, Arr, T*, jint)>
void t_release_elements(GuestThread& t) {
    JNIEnv* env = host_env();
    auto arr = static_cast<Arr>(ref_to_host(t.regs()[1]));
    gaddr g = t.regs()[2];
    jint mode = jint(t.regs()[3]);
    PinnedArray p{};
    {
        std::lock_guard lk(g_pinned_mutex);
        auto it = g_pinned.find(g);
        if (it == g_pinned.end()) {
            H32_ERROR("jni: Release*ArrayElements on unknown buffer 0x%08x", g);
            return;
        }
        p = it->second;
        if (mode != JNI_COMMIT) g_pinned.erase(it);
    }
    if (mode != JNI_ABORT) std::memcpy(p.host, mem().ptr<void>(g), p.bytes);
    (env->functions->*Rel)(env, arr, static_cast<T*>(p.host), mode);
    if (mode != JNI_COMMIT) mem().free(g);
}

// GetPrimitiveArrayCritical doesn't say the element type, so the element
// size comes from the array's class.
size_t element_size(JNIEnv* env, jarray arr) {
    static const std::pair<const char*, size_t> kinds[] = {{"[B", 1}, {"[Z", 1}, {"[C", 2}, {"[S", 2},
                                                           {"[I", 4}, {"[F", 4}, {"[J", 8}, {"[D", 8}};
    for (auto& [name, size] : kinds) {
        jclass c = env->FindClass(name);
        bool match = c && env->IsInstanceOf(arr, c);
        if (c) env->DeleteLocalRef(c);
        if (match) return size;
    }
    H32_ERROR("jni: GetPrimitiveArrayCritical on a non-primitive array");
    return 1;
}

void t_get_critical(GuestThread& t) {
    JNIEnv* env = host_env();
    auto arr = static_cast<jarray>(ref_to_host(t.regs()[1]));
    if (gaddr is_copy = t.regs()[2]) mem().write<uint8_t>(is_copy, JNI_TRUE);
    size_t bytes = size_t(env->GetArrayLength(arr)) * element_size(env, arr);
    void* h = env->GetPrimitiveArrayCritical(arr, nullptr);
    if (!h) return set_ret32(t, 0);
    gaddr g = mem().malloc(bytes);
    std::memcpy(mem().ptr<void>(g), h, bytes);
    env->ReleasePrimitiveArrayCritical(arr, h, JNI_ABORT);
    std::lock_guard lk(g_pinned_mutex);
    g_pinned[g] = {nullptr, bytes};
    set_ret32(t, g);
}

void t_release_critical(GuestThread& t) {
    JNIEnv* env = host_env();
    auto arr = static_cast<jarray>(ref_to_host(t.regs()[1]));
    gaddr g = t.regs()[2];
    jint mode = jint(t.regs()[3]);
    size_t bytes;
    {
        std::lock_guard lk(g_pinned_mutex);
        auto it = g_pinned.find(g);
        if (it == g_pinned.end()) return;
        bytes = it->second.bytes;
        if (mode != JNI_COMMIT) g_pinned.erase(it);
    }
    if (mode != JNI_ABORT) {
        void* h = env->GetPrimitiveArrayCritical(arr, nullptr);
        if (h) {
            std::memcpy(h, mem().ptr<void>(g), bytes);
            env->ReleasePrimitiveArrayCritical(arr, h, 0);
        }
    }
    if (mode != JNI_COMMIT) mem().free(g);
}

void t_get_java_vm(GuestThread& t) {
    if (gaddr out = t.regs()[1]) mem().write<uint32_t>(out, g_guest_vm);
    set_ret32(t, JNI_OK);
}

// ------------------------------------------------ RegisterNatives (ffi) ----

struct NativeMethod {
    RegisteredNative info;
    std::vector<char> args;
    char ret = 'V';
    ffi_cif cif;
    std::vector<ffi_type*> types;
};

std::mutex g_natives_mutex;
std::vector<NativeMethod*> g_natives;

ffi_type* ffi_for(char c) {
    switch (c) {
    case 'Z': return &ffi_type_uint8;
    case 'B': return &ffi_type_sint8;
    case 'C': return &ffi_type_uint16;
    case 'S': return &ffi_type_sint16;
    case 'I': return &ffi_type_sint32;
    case 'J': return &ffi_type_sint64;
    case 'F': return &ffi_type_float;
    case 'D': return &ffi_type_double;
    case 'V': return &ffi_type_void;
    default: return &ffi_type_pointer;
    }
}

// Host side of a guest native method: Java calls this; we call the guest.
void native_trampoline(ffi_cif*, void* ret, void** args, void* user) {
    auto* nm = static_cast<NativeMethod*>(user);
    JNIEnv* env = *static_cast<JNIEnv**>(args[0]);
    EnvScope scope(env);
    GuestArgs ga;
    ga.u32(g_guest_env);
    ga.u32(ref_to_guest(*static_cast<jobject*>(args[1])));
    for (size_t i = 0; i < nm->args.size(); i++) {
        void* a = args[i + 2];
        switch (nm->args[i]) {
        case 'Z': ga.u32(*static_cast<jboolean*>(a)); break;
        case 'B': ga.u32(uint32_t(int32_t(*static_cast<jbyte*>(a)))); break;
        case 'C': ga.u32(*static_cast<jchar*>(a)); break;
        case 'S': ga.u32(uint32_t(int32_t(*static_cast<jshort*>(a)))); break;
        case 'I': ga.u32(uint32_t(*static_cast<jint*>(a))); break;
        case 'J': ga.u64(uint64_t(*static_cast<jlong*>(a))); break;
        case 'F': ga.f32(*static_cast<jfloat*>(a)); break;
        case 'D': ga.f64(*static_cast<jdouble*>(a)); break;
        default: ga.u32(ref_to_guest(*static_cast<jobject*>(a))); break;
        }
    }
    H32_TRACE("jni: Java -> guest %s.%s", nm->info.class_name.c_str(), nm->info.name.c_str());
    GuestResult r = GuestThread::current().call(nm->info.guest_fn, ga);
    switch (nm->ret) {
    case 'V': break;
    case 'Z': *static_cast<ffi_arg*>(ret) = uint8_t(r.r0); break;
    case 'B': *static_cast<ffi_sarg*>(ret) = int8_t(r.r0); break;
    case 'C': *static_cast<ffi_arg*>(ret) = uint16_t(r.r0); break;
    case 'S': *static_cast<ffi_sarg*>(ret) = int16_t(r.r0); break;
    case 'I': *static_cast<ffi_sarg*>(ret) = int32_t(r.r0); break;
    case 'J': *static_cast<jlong*>(ret) = jlong(r.u64()); break;
    case 'F': *static_cast<jfloat*>(ret) = r.f32(); break;
    case 'D': *static_cast<jdouble*>(ret) = r.f64(); break;
    default: *static_cast<jobject*>(ret) = static_cast<jobject>(ref_to_host(r.r0)); break;
    }
}

std::string class_name(JNIEnv* env, jclass cls) {
    jclass cc = env->FindClass("java/lang/Class");
    if (!cc) return "?";
    jmethodID get_name = env->GetMethodID(cc, "getName", "()Ljava/lang/String;");
    auto s = get_name ? static_cast<jstring>(env->CallObjectMethod(cls, get_name)) : nullptr;
    std::string out = "?";
    if (s) {
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) out = c;
        env->ReleaseStringUTFChars(s, c);
    }
    return out;
}

// jint RegisterNatives(JNIEnv*, jclass, const JNINativeMethod*, jint)
void t_register_natives(GuestThread& t) {
    JNIEnv* env = host_env();
    auto cls = static_cast<jclass>(ref_to_host(t.regs()[1]));
    gaddr methods = t.regs()[2];
    jint n = jint(t.regs()[3]);
    std::string cname = class_name(env, cls);
    std::vector<JNINativeMethod> host(size_t(n > 0 ? n : 0));
    for (jint i = 0; i < n; i++) {
        gaddr e = methods + gaddr(i) * 12;
        auto* nm = new NativeMethod();
        nm->info.class_name = cname;
        nm->info.name = mem().str(mem().read<uint32_t>(e));
        nm->info.signature = mem().str(mem().read<uint32_t>(e + 4));
        nm->info.guest_fn = mem().read<uint32_t>(e + 8);
        if (!parse_signature(nm->info.signature.c_str(), nm->args, nm->ret)) {
            H32_ERROR("jni: bad native signature %s", nm->info.signature.c_str());
            return set_ret32(t, uint32_t(JNI_ERR));
        }
        nm->types.push_back(&ffi_type_pointer);  // JNIEnv*
        nm->types.push_back(&ffi_type_pointer);  // jobject / jclass
        for (char c : nm->args) nm->types.push_back(ffi_for(c));
        void* code = nullptr;
        auto* closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &code));
        if (!closure || ffi_prep_cif(&nm->cif, FFI_DEFAULT_ABI, unsigned(nm->types.size()), ffi_for(nm->ret), nm->types.data()) != FFI_OK ||
            ffi_prep_closure_loc(closure, &nm->cif, native_trampoline, nm, code) != FFI_OK)
            fatal("jni: libffi closure setup failed");
        nm->info.host_fn = code;
        host[size_t(i)] = {nm->info.name.c_str(), nm->info.signature.c_str(), code};
        H32_INFO("jni: RegisterNatives %s.%s %s -> %s", cname.c_str(), nm->info.name.c_str(), nm->info.signature.c_str(),
                 describe_address(nm->info.guest_fn).c_str());
        std::lock_guard lk(g_natives_mutex);
        g_natives.push_back(nm);
    }
    set_ret32(t, uint32_t(env->RegisterNatives(cls, host.data(), n)));
}

// --------------------------------------------------------- JavaVM table ----

void t_vm_get_env(GuestThread& t) {  // (vm, void** env, version)
    if (!t_env) return set_ret32(t, uint32_t(JNI_EDETACHED));
    if (gaddr out = t.regs()[1]) mem().write<uint32_t>(out, g_guest_env);
    set_ret32(t, JNI_OK);
}
void t_vm_attach(GuestThread& t) {  // (vm, JNIEnv** env, args)
    if (!t_env && g_host_vm) g_host_vm->AttachCurrentThread(&t_env, nullptr);
    if (gaddr out = t.regs()[1]) mem().write<uint32_t>(out, g_guest_env);
    set_ret32(t, t_env ? JNI_OK : uint32_t(JNI_ERR));
}
void t_vm_detach(GuestThread& t) {
    if (g_host_vm) g_host_vm->DetachCurrentThread();
    t_env = nullptr;
    set_ret32(t, JNI_OK);
}
void t_vm_destroy(GuestThread& t) { set_ret32(t, uint32_t(JNI_ERR)); }

// --------------------------------------------------------- slot table ----

#define H32_JNI_MANUAL_GETTERS(X) \
    X(Boolean, jboolean) X(Byte, jbyte) X(Char, jchar) X(Short, jshort) X(Int, jint) X(Long, jlong) X(Float, jfloat) X(Double, jdouble)

ThunkFn manual_handler(std::string_view name) {
    if (name == "GetMethodID") return t_get_method_id<&JNINativeInterface::GetMethodID>;
    if (name == "GetStaticMethodID") return t_get_method_id<&JNINativeInterface::GetStaticMethodID>;
    if (name == "GetStringUTFChars") return t_get_string_utf_chars;
    if (name == "GetStringChars" || name == "GetStringCritical") return t_get_string_chars;
    if (name == "ReleaseStringUTFChars" || name == "ReleaseStringChars" || name == "ReleaseStringCritical") return t_release_chars;
    if (name == "GetPrimitiveArrayCritical") return t_get_critical;
    if (name == "ReleasePrimitiveArrayCritical") return t_release_critical;
    if (name == "RegisterNatives") return t_register_natives;
    if (name == "GetJavaVM") return t_get_java_vm;
#define H32_ARRAY_CASE(Name, T)                                                                                       \
    if (name == "Get" #Name "ArrayElements")                                                                          \
        return t_get_elements<T, T##Array, &JNINativeInterface::Get##Name##ArrayElements>;                            \
    if (name == "Release" #Name "ArrayElements")                                                                      \
        return t_release_elements<T, T##Array, &JNINativeInterface::Release##Name##ArrayElements>;
    H32_JNI_MANUAL_GETTERS(H32_ARRAY_CASE)
#undef H32_ARRAY_CASE
    fatal("jni: no manual handler for %.*s", int(name.size()), name.data());
}

}  // namespace

void init(JavaVM* host_vm) {
    static std::once_flag once;
    g_host_vm = host_vm;
    std::call_once(once, [] {
        GuestThread::global_init();
        constexpr size_t kSlots = sizeof(JNINativeInterface) / sizeof(void*);
        gaddr table = mem().alloc_static(kSlots * 4, 4);
        auto set = [&](uint32_t idx, const char* name, ThunkFn fn) {
            std::string full = std::string("JNI::") + name;
            mem().write<uint32_t>(table + idx * 4, thunks::add(full, fn));
        };
#define JNI_AUTO(i, N) set(i, #N, &AutoSlot<&JNINativeInterface::N>::thunk);
#define JNI_CALL(i, N, target, form, ret) set(i, #N, &call_slot<target, form, ret>);
#define JNI_MANUAL(i, N) set(i, #N, manual_handler(#N));
#define JNI_NONE(i, N) set(i, #N, nullptr);
#include "jni/jni_slots.inc"
#undef JNI_AUTO
#undef JNI_CALL
#undef JNI_MANUAL
#undef JNI_NONE
        static_assert(sizeof(JNINativeInterface) / sizeof(void*) == 233, "jni.h changed: rerun tools/gen_jni_slots.py");

        // JNIEnv is a pointer to the function table.
        g_guest_env = mem().alloc_static(4, 4);
        mem().write<uint32_t>(g_guest_env, table);

        // JavaVM: reserved0-2, DestroyJavaVM, AttachCurrentThread,
        // DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon.
        gaddr vm_table = mem().alloc_static(8 * 4, 4);
        const std::pair<const char*, ThunkFn> vm_slots[] = {
            {"reserved0", nullptr}, {"reserved1", nullptr}, {"reserved2", nullptr},
            {"DestroyJavaVM", t_vm_destroy}, {"AttachCurrentThread", t_vm_attach},
            {"DetachCurrentThread", t_vm_detach}, {"GetEnv", t_vm_get_env},
            {"AttachCurrentThreadAsDaemon", t_vm_attach}};
        for (uint32_t i = 0; i < 8; i++)
            mem().write<uint32_t>(vm_table + i * 4, thunks::add(std::string("JavaVM::") + vm_slots[i].first, vm_slots[i].second));
        g_guest_vm = mem().alloc_static(4, 4);
        mem().write<uint32_t>(g_guest_vm, vm_table);
        H32_INFO("jni: guest JavaVM at 0x%08x, JNIEnv at 0x%08x", g_guest_vm, g_guest_env);
    });
}

jint call_JNI_OnLoad(gaddr fn, JNIEnv* env) {
    EnvScope scope(env);
    GuestArgs a;
    a.u32(g_guest_vm).u32(0);
    jint v = jint(GuestThread::current().call(fn, a).r0);
    H32_INFO("jni: guest JNI_OnLoad returned 0x%x", v);
    return v;
}

std::vector<RegisteredNative> registered_natives() {
    std::lock_guard lk(g_natives_mutex);
    std::vector<RegisteredNative> out;
    for (auto* n : g_natives) out.push_back(n->info);
    return out;
}

}  // namespace h32::jni
