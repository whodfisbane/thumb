// A fake JVM for the Linux harness: just enough of JNI for native code to
// look classes and methods up, make strings/arrays, and register natives.
// Java method calls are logged and return zero/null.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "jni.h"

namespace fakejni {

struct Obj {
    enum Kind { Class, Object, String, Array } kind;
    struct Klass* cls = nullptr;
    virtual ~Obj() = default;
    explicit Obj(Kind k) : kind(k) {}
};
struct Klass : Obj {
    std::string name;  // "com/foo/Bar"
    Klass() : Obj(Class) {}
};
struct Str : Obj {
    std::string utf8;
    Str() : Obj(String) {}
};
struct Arr : Obj {
    char elem;  // JNI type char
    size_t elem_size;
    std::vector<uint8_t> data;
    std::vector<jobject> objects;
    Arr() : Obj(Array) {}
    size_t length() const { return elem == 'L' ? objects.size() : data.size() / elem_size; }
};
struct Method {
    Klass* cls;
    std::string name, sig;
    bool is_static;
};
struct Field {
    Klass* cls;
    std::string name, sig;
};

struct Native {
    std::string cls, name, sig;
    void* fn;
};

JavaVM* vm();
JNIEnv* env();
Klass* find_class(const std::string& name);
jstring new_string(const std::string& s);
// Natives registered via RegisterNatives, by "class.name".
std::map<std::string, Native>& natives();
// Java method calls the native code made: "Class.name(sig)" -> count.
std::map<std::string, int>& java_calls();

// Lets the harness answer Java calls (e.g. GetAppPath). Return true if handled.
using JavaHandler = std::function<bool(const Method& m, jobject self, const jvalue* args, jvalue& result)>;
void set_java_handler(JavaHandler h);
jobject new_object(const std::string& class_name);
std::string string_value(jobject s);
Arr* array(jobject a);

}  // namespace fakejni
