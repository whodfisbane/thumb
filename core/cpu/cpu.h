// Guest execution. Each host thread that runs guest code has a GuestThread,
// which owns a guest stack and a stack of dynarmic JIT instances: a JIT can't
// be re-entered, so a nested call (guest -> thunk -> Java -> native -> guest)
// runs on a fresh JIT one level deeper, sharing the same memory.
#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common.h"
#include "memory/arena.h"

namespace Dynarmic::A32 {
class Jit;
}

namespace h32 {

// SVC number of the stub that returns control from guest code to the host.
constexpr uint32_t kSvcReturnToHost = 0xFFFFFF;

// Arguments for a host->guest call, laid out per the AAPCS soft-float ABI
// used by armeabi-v7a: a flat list of words where 64-bit values start at an
// even index. Words 0-3 go in r0-r3, the rest on the stack.
class GuestArgs {
public:
    GuestArgs& u32(uint32_t v) {
        words_.push_back(v);
        return *this;
    }
    GuestArgs& u64(uint64_t v) {
        if (words_.size() & 1) words_.push_back(0);
        words_.push_back(uint32_t(v));
        words_.push_back(uint32_t(v >> 32));
        return *this;
    }
    GuestArgs& f32(float v) {
        uint32_t w;
        std::memcpy(&w, &v, 4);
        return u32(w);
    }
    GuestArgs& f64(double v) {
        uint64_t w;
        std::memcpy(&w, &v, 8);
        return u64(w);
    }
    const std::vector<uint32_t>& words() const { return words_; }

private:
    std::vector<uint32_t> words_;
};

struct GuestResult {
    uint32_t r0 = 0, r1 = 0;
    uint64_t u64() const { return uint64_t(r1) << 32 | r0; }
    float f32() const {
        float f;
        std::memcpy(&f, &r0, 4);
        return f;
    }
    double f64() const {
        double d;
        uint64_t v = u64();
        std::memcpy(&d, &v, 8);
        return d;
    }
};

class GuestThread {
public:
    // The calling host thread's guest context, created on first use.
    static GuestThread& current();
    // Called once at startup (writes the return stub).
    static void global_init();

    ~GuestThread();

    // Calls guest function `fn` (bit 0 set = Thumb) and runs until it returns.
    GuestResult call(gaddr fn, const GuestArgs& args = {});

    // --- accessors for thunks (valid while a thunk is executing) ---
    std::array<uint32_t, 16>& regs();
    std::array<uint32_t, 64>& ext_regs();  // s0-s31 / d0-d15 (VFP)
    uint32_t cpsr();
    // Reads argument word `i` of the call that entered the current thunk.
    uint32_t arg_word(size_t i);
    // Redirects guest execution when the current thunk returns (longjmp).
    void request_jump(const std::array<uint32_t, 16>& regs, uint32_t cpsr);

    gaddr errno_addr() const { return errno_addr_; }
    // Per-thread block returned by __kuser_get_tls (bionic-style TLS slots).
    gaddr tls_area() const { return tls_area_; }
    gaddr stack_top() const { return stack_top_; }
    int depth() const { return active_; }

    // pthread_getspecific/setspecific storage (see thunks/libc_pthread.cpp).
    static constexpr int kMaxKeys = 256;
    std::array<gaddr, kMaxKeys> tls_values{};

    struct Frame;

private:
    GuestThread();
    Frame& frame_at(int depth);
    void dump_state(Frame& f, const char* why);
    friend struct Callbacks;

    std::vector<std::unique_ptr<Frame>> frames_;
    int active_ = 0;
    gaddr stack_base_ = 0, stack_top_ = 0;
    gaddr errno_addr_ = 0;
    gaddr tls_area_ = 0;
};

// Disassembly-free crash context: "module+offset (symbol)".
std::string describe_address(gaddr a);

}  // namespace h32
