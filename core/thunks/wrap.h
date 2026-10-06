// Automatic marshalling between the guest ABI (32-bit ARM, AAPCS soft-float)
// and ordinary host functions. Writing
//
//     thunks::add("strlen", H32_WRAP(::strlen, size_t(const char*)));
//
// produces a handler that reads guest args, converts pointers (guest address
// -> host pointer), calls the host function, and converts the result back.
//
// Conversion rules (LibcAbi):
//   - pointers: guest address <-> host pointer (pointer-to-pointer is rejected
//     at compile time because guest pointers are 4 bytes, host ones 8)
//   - long / unsigned long / size_t: 32-bit on the guest
//   - long long: 64-bit, passed in an aligned register pair
//   - float: one word; double: aligned word pair (soft-float)
// JniAbi differs only in treating `long` as 64-bit, because jni.h defines
// jlong as int64_t (which is `long` on 64-bit hosts) and JNI never uses C long.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <type_traits>

#include "cpu/cpu.h"
#include "jni.h"
#include "memory/arena.h"

namespace h32 {

// Guest argument reader following AAPCS word placement.
struct ArgCursor {
    GuestThread& t;
    size_t i = 0;
    uint32_t word() { return t.arg_word(i++); }
    uint64_t dword() {
        if (i & 1) ++i;
        uint64_t lo = word();
        uint64_t hi = word();
        return hi << 32 | lo;
    }
};

inline void set_ret32(GuestThread& t, uint32_t v) { t.regs()[0] = v; }
inline void set_ret64(GuestThread& t, uint64_t v) {
    t.regs()[0] = uint32_t(v);
    t.regs()[1] = uint32_t(v >> 32);
}

// JNI reference translation (implemented in jni/jni_bridge.cpp).
namespace jni {
void* ref_to_host(uint32_t handle);
uint32_t ref_to_guest(void* ref);
void* id_to_host(uint32_t handle);
uint32_t id_to_guest(void* id);
JNIEnv* host_env();
uint32_t guest_env();
}  // namespace jni

// Guest FILE* -> host FILE* (implemented in thunks/libc_stdio.cpp).
FILE* host_file(gaddr f);

// jni.h declares _jmethodID/_jfieldID as incomplete types, so references are
// matched against an explicit list rather than with is_base_of.
template <class T>
inline constexpr bool is_jni_ref_v =
    std::is_same_v<T, jobject> || std::is_same_v<T, jclass> || std::is_same_v<T, jstring> ||
    std::is_same_v<T, jarray> || std::is_same_v<T, jobjectArray> || std::is_same_v<T, jthrowable> ||
    std::is_same_v<T, jbooleanArray> || std::is_same_v<T, jbyteArray> || std::is_same_v<T, jcharArray> ||
    std::is_same_v<T, jshortArray> || std::is_same_v<T, jintArray> || std::is_same_v<T, jlongArray> ||
    std::is_same_v<T, jfloatArray> || std::is_same_v<T, jdoubleArray>;
template <class T>
inline constexpr bool is_jni_id_v = std::is_same_v<T, jmethodID> || std::is_same_v<T, jfieldID>;

struct LibcAbi {
    static constexpr bool long_is_64 = false;
    static constexpr bool sync_errno = true;
};
struct JniAbi {
    static constexpr bool long_is_64 = true;
    static constexpr bool sync_errno = false;
};

template <class Abi, class T>
constexpr bool is_wide_int() {
    if constexpr (!std::is_integral_v<T> || sizeof(T) != 8) return false;
    else if constexpr (std::is_same_v<std::remove_cv_t<T>, long> || std::is_same_v<std::remove_cv_t<T>, unsigned long>)
        return Abi::long_is_64;
    else return true;  // long long, int64_t-as-long-long
}

template <class Abi, class T>
T get_arg(ArgCursor& c) {
    if constexpr (std::is_same_v<T, JNIEnv*>) {
        c.word();
        return jni::host_env();
    } else if constexpr (std::is_same_v<T, FILE*>) {
        return host_file(c.word());
    } else if constexpr (is_jni_ref_v<T>) {
        return static_cast<T>(jni::ref_to_host(c.word()));
    } else if constexpr (is_jni_id_v<T>) {
        return static_cast<T>(jni::id_to_host(c.word()));
    } else if constexpr (std::is_pointer_v<T>) {
        static_assert(!std::is_pointer_v<std::remove_cv_t<std::remove_pointer_t<T>>>,
                      "pointer-to-pointer args differ in size between guest and host; write this thunk by hand");
        return mem().ptr<std::remove_pointer_t<T>>(c.word());
    } else if constexpr (std::is_same_v<T, float>) {
        uint32_t w = c.word();
        float f;
        std::memcpy(&f, &w, 4);
        return f;
    } else if constexpr (std::is_same_v<T, double>) {
        uint64_t w = c.dword();
        double d;
        std::memcpy(&d, &w, 8);
        return d;
    } else if constexpr (is_wide_int<Abi, T>()) {
        return T(c.dword());
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        uint32_t w = c.word();
        if constexpr (std::is_signed_v<T>) return T(int32_t(w));
        else return T(w);
    } else {
        static_assert(sizeof(T) == 0, "unsupported thunk argument type");
    }
}

template <class Abi, class T>
void put_ret(GuestThread& t, T v) {
    if constexpr (is_jni_ref_v<T>) {
        set_ret32(t, jni::ref_to_guest(v));
    } else if constexpr (is_jni_id_v<T>) {
        set_ret32(t, jni::id_to_guest(v));
    } else if constexpr (std::is_pointer_v<T>) {
        set_ret32(t, mem().addr(v));
    } else if constexpr (std::is_same_v<T, float>) {
        uint32_t w;
        std::memcpy(&w, &v, 4);
        set_ret32(t, w);
    } else if constexpr (std::is_same_v<T, double>) {
        uint64_t w;
        std::memcpy(&w, &v, 8);
        set_ret64(t, w);
    } else if constexpr (is_wide_int<Abi, T>()) {
        set_ret64(t, uint64_t(v));
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        set_ret32(t, uint32_t(v));
    } else {
        static_assert(sizeof(T) == 0, "unsupported thunk return type");
    }
}

void sync_guest_errno(GuestThread& t);

template <class Abi, class Sig>
struct Wrapper;

template <class Abi, class R, class... A>
struct Wrapper<Abi, R(A...)> {
    template <R (*Fn)(A...)>
    static void thunk(GuestThread& t) {
        ArgCursor c{t};
        // Braced init guarantees left-to-right argument evaluation.
        std::tuple<A...> args{get_arg<Abi, A>(c)...};
        if constexpr (Abi::sync_errno) errno = 0;
        if constexpr (std::is_void_v<R>) {
            std::apply(Fn, args);
        } else {
            R r = std::apply(Fn, args);
            put_ret<Abi, R>(t, r);
        }
        if constexpr (Abi::sync_errno) sync_guest_errno(t);
    }
};

}  // namespace h32

// H32_WRAP(fn, signature): handler for a host function with that exact
// signature (the cast picks one overload, e.g. ::sqrt(double)).
#define H32_WRAP(fn, ...) \
    (&::h32::Wrapper<::h32::LibcAbi, __VA_ARGS__>::template thunk<static_cast<std::add_pointer_t<__VA_ARGS__>>(fn)>)
#define H32_WRAP_JNI(fn, ...) \
    (&::h32::Wrapper<::h32::JniAbi, __VA_ARGS__>::template thunk<static_cast<std::add_pointer_t<__VA_ARGS__>>(fn)>)
// Same-name shorthand: H32_ADD(strlen, size_t(const char*))
#define H32_ADD(name, ...) ::h32::thunks::add(#name, H32_WRAP(::name, __VA_ARGS__))
