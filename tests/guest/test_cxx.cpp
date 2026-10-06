#include <map>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include "check.h"
}

struct Base { virtual ~Base() = default; virtual int f() const { return 1; } };
struct Derived : Base { int f() const override { return 2; } };
static std::string g_static = std::string("static ") + "init";

static int thrower(int n) {
    if (n == 3) throw std::runtime_error("boom");
    return n;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) {
    bool caught = false;
    try { thrower(3); } catch (const std::runtime_error& e) { caught = std::string(e.what()) == "boom"; }
    CHECK(caught, "C++ exception through frames (ARM EHABI unwinding)");
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        try { sum += thrower(i); } catch (...) { sum += 100; }
    }
    CHECK(sum == 0 + 1 + 2 + 100 + 4, "repeated throw/catch");
    std::vector<std::string> v;
    for (int i = 0; i < 1000; i++) v.push_back(std::to_string(i));
    CHECK(v.size() == 1000 && v[999] == "999", "vector<string> growth");
    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    m["c"] = 3;
    CHECK(m.size() == 3 && m["b"] == 2, "std::map");
    Derived d; const Base& b = d;
    CHECK(b.f() == 2, "virtual dispatch");
    CHECK(g_static == "static init", "static constructors ran");
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    fflush(stdout);
    return JNI_VERSION_1_6;
}
