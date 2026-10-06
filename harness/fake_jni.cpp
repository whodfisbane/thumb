#include "fake_jni.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "common.h"

namespace fakejni {

namespace {

constexpr size_t kSlots = sizeof(JNINativeInterface) / sizeof(void*);

const char* const kSlotNames[] = {
#define JNI_AUTO(i, N) #N,
#define JNI_CALL(i, N, a, b, c) #N,
#define JNI_MANUAL(i, N) #N,
#define JNI_NONE(i, N) #N,
#include "jni/jni_slots.inc"
#undef JNI_AUTO
#undef JNI_CALL
#undef JNI_MANUAL
#undef JNI_NONE
};
static_assert(std::size(kSlotNames) == kSlots);

// Fallback for every slot without a fake: log once, return 0.
template <size_t N>
long unimplemented() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        H32_WARN("fake JNI: %s not implemented, returning 0", kSlotNames[N]);
    }
    return 0;
}

template <size_t... I>
void fill_defaults(void** table, std::index_sequence<I...>) {
    ((table[I] = reinterpret_cast<void*>(&unimplemented<I>)), ...);
}

std::recursive_mutex g_mutex;
std::map<std::string, Klass*> g_classes;
std::map<std::pair<Klass*, std::string>, Method*> g_methods;
std::map<std::pair<Klass*, std::string>, Field*> g_fields;

Obj* O(jobject o) { return reinterpret_cast<Obj*>(o); }
jobject J(Obj* o) { return reinterpret_cast<jobject>(o); }

std::string dotted(std::string s) {
    for (auto& c : s)
        if (c == '/') c = '.';
    return s;
}

// ---- classes / methods / fields ----
jclass FindClass(JNIEnv*, const char* name) { return reinterpret_cast<jclass>(find_class(name)); }

jclass GetObjectClass(JNIEnv*, jobject o) {
    if (!o) return nullptr;
    Obj* ob = O(o);
    if (ob->cls) return reinterpret_cast<jclass>(ob->cls);
    switch (ob->kind) {
    case Obj::Class: return reinterpret_cast<jclass>(find_class("java/lang/Class"));
    case Obj::String: return reinterpret_cast<jclass>(find_class("java/lang/String"));
    default: return reinterpret_cast<jclass>(find_class("java/lang/Object"));
    }
}

jmethodID get_method(jclass c, const char* name, const char* sig, bool is_static) {
    std::lock_guard lk(g_mutex);
    auto* k = reinterpret_cast<Klass*>(c);
    auto& m = g_methods[{k, std::string(name) + sig}];
    if (!m) m = new Method{k, name, sig, is_static};
    return reinterpret_cast<jmethodID>(m);
}
jmethodID GetMethodID(JNIEnv*, jclass c, const char* n, const char* s) { return get_method(c, n, s, false); }
jmethodID GetStaticMethodID(JNIEnv*, jclass c, const char* n, const char* s) { return get_method(c, n, s, true); }

jfieldID get_field(jclass c, const char* name, const char* sig) {
    std::lock_guard lk(g_mutex);
    auto* k = reinterpret_cast<Klass*>(c);
    auto& f = g_fields[{k, name}];
    if (!f) f = new Field{k, name, sig};
    return reinterpret_cast<jfieldID>(f);
}
jfieldID GetFieldID(JNIEnv*, jclass c, const char* n, const char* s) { return get_field(c, n, s); }
jfieldID GetStaticFieldID(JNIEnv*, jclass c, const char* n, const char* s) { return get_field(c, n, s); }

// ---- fields: object-typed fields hold a stable fake object of their type ----
std::map<std::pair<jobject, Field*>, jobject> g_field_values;

jobject object_field(jobject o, jfieldID fid) {
    auto* f = reinterpret_cast<Field*>(fid);
    std::lock_guard lk(g_mutex);
    auto& v = g_field_values[{o, f}];
    if (!v && f->sig.size() > 2 && f->sig[0] == 'L') {
        v = new_object(f->sig.substr(1, f->sig.size() - 2));
        H32_INFO("fake JNI: field %s.%s -> new %s", f->cls->name.c_str(), f->name.c_str(), f->sig.c_str());
    }
    return v;
}
jobject GetObjectField(JNIEnv*, jobject o, jfieldID f) { return object_field(o, f); }
jobject GetStaticObjectField(JNIEnv*, jclass c, jfieldID f) { return object_field(reinterpret_cast<jobject>(c), f); }
void SetObjectField(JNIEnv*, jobject o, jfieldID f, jobject v) {
    std::lock_guard lk(g_mutex);
    g_field_values[{o, reinterpret_cast<Field*>(f)}] = v;
}
void SetStaticObjectField(JNIEnv*, jclass c, jfieldID f, jobject v) {
    std::lock_guard lk(g_mutex);
    g_field_values[{reinterpret_cast<jobject>(c), reinterpret_cast<Field*>(f)}] = v;
}

// ---- method calls: logged, return zero (with a couple of useful answers) ----
JavaHandler g_handler;

jvalue java_call(jobject obj, jmethodID mid, const jvalue* args = nullptr) {
    auto* m = reinterpret_cast<Method*>(mid);
    jvalue r{};
    if (!m) return r;
    if (g_handler && g_handler(*m, obj, args, r)) {
        std::lock_guard lk(g_mutex);
        java_calls()[(m->cls ? m->cls->name : "?") + "." + m->name + m->sig]++;
        return r;
    }
    std::string key = (m->cls ? m->cls->name : "?") + "." + m->name + m->sig;
    {
        std::lock_guard lk(g_mutex);
        if (java_calls()[key]++ == 0) H32_INFO("fake JNI: Java call %s", key.c_str());
    }
    if (m->name == "getName" && obj && O(obj)->kind == Obj::Class)
        r.l = new_string(dotted(static_cast<Klass*>(O(obj))->name));
    else if (m->name == "<init>")
        r.l = obj;
    return r;
}

#define FAKE_CALLS(R, Name, field)                                                                               \
    R Call##Name##MethodA(JNIEnv*, jobject o, jmethodID m, const jvalue* a) { return java_call(o, m, a).field; }  \
    R CallStatic##Name##MethodA(JNIEnv*, jclass, jmethodID m, const jvalue* a) { return java_call(nullptr, m, a).field; } \
    R CallNonvirtual##Name##MethodA(JNIEnv*, jobject o, jclass, jmethodID m, const jvalue* a) { return java_call(o, m, a).field; }
FAKE_CALLS(jobject, Object, l)
FAKE_CALLS(jboolean, Boolean, z)
FAKE_CALLS(jbyte, Byte, b)
FAKE_CALLS(jchar, Char, c)
FAKE_CALLS(jshort, Short, s)
FAKE_CALLS(jint, Int, i)
FAKE_CALLS(jlong, Long, j)
FAKE_CALLS(jfloat, Float, f)
FAKE_CALLS(jdouble, Double, d)
#undef FAKE_CALLS
void CallVoidMethodA(JNIEnv*, jobject o, jmethodID m, const jvalue* a) { java_call(o, m, a); }
void CallStaticVoidMethodA(JNIEnv*, jclass, jmethodID m, const jvalue* a) { java_call(nullptr, m, a); }
void CallNonvirtualVoidMethodA(JNIEnv*, jobject o, jclass, jmethodID m, const jvalue* a) { java_call(o, m, a); }

// The host-side varargs forms are only used by our own bridge helpers
// (class_name()), which call CallObjectMethod(cls, getName).
jobject CallObjectMethod(JNIEnv*, jobject o, jmethodID m, ...) { return java_call(o, m).l; }
jobject CallObjectMethodV(JNIEnv*, jobject o, jmethodID m, va_list) { return java_call(o, m).l; }

jobject NewObjectA(JNIEnv*, jclass c, jmethodID m, const jvalue* a) {
    auto* o = new Obj(Obj::Object);
    o->cls = reinterpret_cast<Klass*>(c);
    java_call(J(o), m, a);
    return J(o);
}
jobject AllocObject(JNIEnv*, jclass c) {
    auto* o = new Obj(Obj::Object);
    o->cls = reinterpret_cast<Klass*>(c);
    return J(o);
}

// ---- references ----
jobject ident(JNIEnv*, jobject o) { return o; }
void drop(JNIEnv*, jobject) {}
jint zero_int(JNIEnv*, jint) { return 0; }
jobject pop_frame(JNIEnv*, jobject r) { return r; }
jboolean IsSameObject(JNIEnv*, jobject a, jobject b) { return a == b; }
jboolean IsInstanceOf(JNIEnv*, jobject o, jclass c) {
    if (!o) return JNI_TRUE;
    auto* k = reinterpret_cast<Klass*>(c);
    if (O(o)->kind == Obj::Array && k->name.size() == 2 && k->name[0] == '[')
        return static_cast<Arr*>(O(o))->elem == k->name[1];
    return O(o)->cls == k;
}
jint GetVersion(JNIEnv*) { return JNI_VERSION_1_6; }
jboolean ExceptionCheck(JNIEnv*) { return JNI_FALSE; }
jthrowable ExceptionOccurred(JNIEnv*) { return nullptr; }
void ExceptionClear(JNIEnv*) {}
void ExceptionDescribe(JNIEnv*) {}
jint ThrowNew(JNIEnv*, jclass c, const char* msg) {
    H32_WARN("fake JNI: native code threw %s: %s", reinterpret_cast<Klass*>(c)->name.c_str(), msg ? msg : "");
    return 0;
}

// ---- strings ----
jstring NewStringUTF(JNIEnv*, const char* s) { return new_string(s ? s : ""); }
const char* GetStringUTFChars(JNIEnv*, jstring s, jboolean* copy) {
    if (copy) *copy = JNI_FALSE;
    return s ? static_cast<Str*>(O(s))->utf8.c_str() : nullptr;
}
void ReleaseStringUTFChars(JNIEnv*, jstring, const char*) {}
jsize GetStringUTFLength(JNIEnv*, jstring s) { return jsize(static_cast<Str*>(O(s))->utf8.size()); }
jsize GetStringLength(JNIEnv*, jstring s) { return jsize(static_cast<Str*>(O(s))->utf8.size()); }  // ASCII only
void GetStringRegion(JNIEnv*, jstring s, jsize start, jsize len, jchar* buf) {
    auto& u = static_cast<Str*>(O(s))->utf8;
    for (jsize i = 0; i < len; i++) buf[i] = jchar(uint8_t(u[size_t(start + i)]));
}
void GetStringUTFRegion(JNIEnv*, jstring s, jsize start, jsize len, char* buf) {
    std::memcpy(buf, static_cast<Str*>(O(s))->utf8.data() + start, size_t(len));
    buf[len] = 0;
}
jstring NewString(JNIEnv*, const jchar* c, jsize n) {
    std::string s;
    for (jsize i = 0; i < n; i++) s += char(c[i]);
    return new_string(s);
}

// ---- arrays ----
Arr* new_array(char elem, size_t size, jsize n) {
    auto* a = new Arr();
    a->elem = elem;
    a->elem_size = size;
    if (elem == 'L') a->objects.resize(size_t(n));
    else a->data.resize(size_t(n) * size);
    return a;
}
jsize GetArrayLength(JNIEnv*, jarray a) { return a ? jsize(static_cast<Arr*>(O(a))->length()) : 0; }
jobjectArray NewObjectArray(JNIEnv*, jsize n, jclass, jobject init) {
    Arr* a = new_array('L', sizeof(jobject), n);
    for (auto& o : a->objects) o = init;
    return reinterpret_cast<jobjectArray>(a);
}
jobject GetObjectArrayElement(JNIEnv*, jobjectArray a, jsize i) { return static_cast<Arr*>(O(a))->objects.at(size_t(i)); }
void SetObjectArrayElement(JNIEnv*, jobjectArray a, jsize i, jobject v) { static_cast<Arr*>(O(a))->objects.at(size_t(i)) = v; }
void* GetPrimitiveArrayCritical(JNIEnv*, jarray a, jboolean* copy) {
    if (copy) *copy = JNI_FALSE;
    return static_cast<Arr*>(O(a))->data.data();
}
void ReleasePrimitiveArrayCritical(JNIEnv*, jarray, void*, jint) {}

#define FAKE_ARRAYS(T, Name, C)                                                                          \
    T##Array New##Name##Array(JNIEnv*, jsize n) { return reinterpret_cast<T##Array>(new_array(C, sizeof(T), n)); } \
    T* Get##Name##ArrayElements(JNIEnv*, T##Array a, jboolean* copy) {                                   \
        if (copy) *copy = JNI_FALSE;                                                                     \
        return reinterpret_cast<T*>(static_cast<Arr*>(O(a))->data.data());                               \
    }                                                                                                    \
    void Release##Name##ArrayElements(JNIEnv*, T##Array, T*, jint) {}                                    \
    void Get##Name##ArrayRegion(JNIEnv*, T##Array a, jsize s, jsize n, T* buf) {                         \
        std::memcpy(buf, static_cast<Arr*>(O(a))->data.data() + size_t(s) * sizeof(T), size_t(n) * sizeof(T)); \
    }                                                                                                    \
    void Set##Name##ArrayRegion(JNIEnv*, T##Array a, jsize s, jsize n, const T* buf) {                   \
        std::memcpy(static_cast<Arr*>(O(a))->data.data() + size_t(s) * sizeof(T), buf, size_t(n) * sizeof(T)); \
    }
FAKE_ARRAYS(jboolean, Boolean, 'Z')
FAKE_ARRAYS(jbyte, Byte, 'B')
FAKE_ARRAYS(jchar, Char, 'C')
FAKE_ARRAYS(jshort, Short, 'S')
FAKE_ARRAYS(jint, Int, 'I')
FAKE_ARRAYS(jlong, Long, 'J')
FAKE_ARRAYS(jfloat, Float, 'F')
FAKE_ARRAYS(jdouble, Double, 'D')
#undef FAKE_ARRAYS

// ---- natives ----
jint RegisterNatives(JNIEnv*, jclass c, const JNINativeMethod* m, jint n) {
    auto* k = reinterpret_cast<Klass*>(c);
    for (jint i = 0; i < n; i++) natives()[k->name + "." + m[i].name] = {k->name, m[i].name, m[i].signature, m[i].fnPtr};
    return JNI_OK;
}

JNINativeInterface g_table;
JNIEnv g_env;

// ---- JavaVM ----
jint vm_get_env(JavaVM*, void** env, jint) {
    *env = &g_env;
    return JNI_OK;
}
jint vm_attach(JavaVM*, JNIEnv** env, void*) {
    *env = &g_env;
    return JNI_OK;
}
jint vm_detach(JavaVM*) { return JNI_OK; }
jint vm_destroy(JavaVM*) { return JNI_ERR; }
JNIInvokeInterface g_vm_table;
JavaVM g_vm;

void build() {
    auto** slots = reinterpret_cast<void**>(&g_table);
    fill_defaults(slots, std::make_index_sequence<kSlots>{});
    auto& t = g_table;
    t.GetVersion = GetVersion;
    t.FindClass = FindClass;
    t.GetObjectClass = GetObjectClass;
    t.GetMethodID = GetMethodID;
    t.GetStaticMethodID = GetStaticMethodID;
    t.GetFieldID = GetFieldID;
    t.GetStaticFieldID = GetStaticFieldID;
    t.GetObjectField = GetObjectField;
    t.GetStaticObjectField = GetStaticObjectField;
    t.SetObjectField = SetObjectField;
    t.SetStaticObjectField = SetStaticObjectField;
#define SET_CALLS(Name)                                \
    t.Call##Name##MethodA = Call##Name##MethodA;       \
    t.CallStatic##Name##MethodA = CallStatic##Name##MethodA; \
    t.CallNonvirtual##Name##MethodA = CallNonvirtual##Name##MethodA;
    SET_CALLS(Object) SET_CALLS(Boolean) SET_CALLS(Byte) SET_CALLS(Char) SET_CALLS(Short)
    SET_CALLS(Int) SET_CALLS(Long) SET_CALLS(Float) SET_CALLS(Double) SET_CALLS(Void)
#undef SET_CALLS
    t.CallObjectMethod = CallObjectMethod;
    t.CallObjectMethodV = CallObjectMethodV;
    t.NewObjectA = NewObjectA;
    t.AllocObject = AllocObject;
    t.NewGlobalRef = ident;
    t.NewLocalRef = ident;
    t.NewWeakGlobalRef = reinterpret_cast<jweak (*)(JNIEnv*, jobject)>(ident);
    t.DeleteGlobalRef = drop;
    t.DeleteLocalRef = drop;
    t.DeleteWeakGlobalRef = drop;
    t.PushLocalFrame = zero_int;
    t.EnsureLocalCapacity = zero_int;
    t.PopLocalFrame = pop_frame;
    t.IsSameObject = IsSameObject;
    t.IsInstanceOf = IsInstanceOf;
    t.ExceptionCheck = ExceptionCheck;
    t.ExceptionOccurred = ExceptionOccurred;
    t.ExceptionClear = ExceptionClear;
    t.ExceptionDescribe = ExceptionDescribe;
    t.ThrowNew = ThrowNew;
    t.NewStringUTF = NewStringUTF;
    t.NewString = NewString;
    t.GetStringUTFChars = GetStringUTFChars;
    t.ReleaseStringUTFChars = ReleaseStringUTFChars;
    t.GetStringUTFLength = GetStringUTFLength;
    t.GetStringLength = GetStringLength;
    t.GetStringRegion = GetStringRegion;
    t.GetStringUTFRegion = GetStringUTFRegion;
    t.GetArrayLength = GetArrayLength;
    t.NewObjectArray = NewObjectArray;
    t.GetObjectArrayElement = GetObjectArrayElement;
    t.SetObjectArrayElement = SetObjectArrayElement;
    t.GetPrimitiveArrayCritical = GetPrimitiveArrayCritical;
    t.ReleasePrimitiveArrayCritical = ReleasePrimitiveArrayCritical;
#define SET_ARRAYS(Name)                                     \
    t.New##Name##Array = New##Name##Array;                   \
    t.Get##Name##ArrayElements = Get##Name##ArrayElements;   \
    t.Release##Name##ArrayElements = Release##Name##ArrayElements; \
    t.Get##Name##ArrayRegion = Get##Name##ArrayRegion;       \
    t.Set##Name##ArrayRegion = Set##Name##ArrayRegion;
    SET_ARRAYS(Boolean) SET_ARRAYS(Byte) SET_ARRAYS(Char) SET_ARRAYS(Short)
    SET_ARRAYS(Int) SET_ARRAYS(Long) SET_ARRAYS(Float) SET_ARRAYS(Double)
#undef SET_ARRAYS
    t.RegisterNatives = RegisterNatives;
    g_env.functions = &g_table;

    g_vm_table.GetEnv = vm_get_env;
    g_vm_table.AttachCurrentThread = vm_attach;
    g_vm_table.AttachCurrentThreadAsDaemon = vm_attach;
    g_vm_table.DetachCurrentThread = vm_detach;
    g_vm_table.DestroyJavaVM = vm_destroy;
    g_vm.functions = &g_vm_table;
}

void ensure_built() {
    static std::once_flag once;
    std::call_once(once, build);
}

}  // namespace

JavaVM* vm() {
    ensure_built();
    return &g_vm;
}

JNIEnv* env() {
    ensure_built();
    return &g_env;
}

Klass* find_class(const std::string& name) {
    std::lock_guard lk(g_mutex);
    auto& k = g_classes[name];
    if (!k) {
        k = new Klass();
        k->name = name;
    }
    return k;
}

jstring new_string(const std::string& s) {
    auto* o = new Str();
    o->utf8 = s;
    o->cls = find_class("java/lang/String");
    return reinterpret_cast<jstring>(o);
}

std::map<std::string, Native>& natives() {
    static std::map<std::string, Native> m;
    return m;
}

void set_java_handler(JavaHandler h) { g_handler = std::move(h); }

jobject new_object(const std::string& class_name) {
    auto* o = new Obj(Obj::Object);
    o->cls = find_class(class_name);
    return J(o);
}

std::string string_value(jobject s) { return s && O(s)->kind == Obj::String ? static_cast<Str*>(O(s))->utf8 : ""; }
Arr* array(jobject a) { return a && O(a)->kind == Obj::Array ? static_cast<Arr*>(O(a)) : nullptr; }

std::map<std::string, int>& java_calls() {
    static std::map<std::string, int> m;
    return m;
}

}  // namespace fakejni
