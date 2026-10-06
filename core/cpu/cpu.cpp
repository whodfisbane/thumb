#include "cpu/cpu.h"

#include <atomic>
#include <mutex>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "loader/elf_loader.h"
#include "thunks/thunks.h"

namespace h32 {

namespace {

constexpr size_t kMaxProcessors = 256;
constexpr size_t kStackSize = 8 * 1024 * 1024;
constexpr uint32_t kCpsrThumb = 1u << 5;
constexpr uint32_t kCpsrUserMode = 0x10;

gaddr g_return_stub = 0;

Dynarmic::ExclusiveMonitor& monitor() {
    static Dynarmic::ExclusiveMonitor m(kMaxProcessors);
    return m;
}

// Each live JIT needs a unique processor id for the exclusive monitor.
class ProcessorIds {
public:
    size_t acquire() {
        std::lock_guard lk(m_);
        for (size_t i = 0; i < kMaxProcessors; i++)
            if (!used_[i]) {
                used_[i] = true;
                return i;
            }
        fatal("cpu: more than %zu concurrent guest JITs", kMaxProcessors);
    }
    void release(size_t id) {
        std::lock_guard lk(m_);
        used_[id] = false;
    }

private:
    std::mutex m_;
    bool used_[kMaxProcessors] = {};
};
ProcessorIds g_ids;

}  // namespace

struct Callbacks final : Dynarmic::A32::UserCallbacks {
    GuestThread* thread = nullptr;
    GuestThread::Frame* frame = nullptr;

    bool check(uint32_t vaddr, const char* what);

    uint8_t MemoryRead8(uint32_t a) override { return check(a, "read8") ? mem().read<uint8_t>(a) : 0; }
    uint16_t MemoryRead16(uint32_t a) override { return check(a, "read16") ? mem().read<uint16_t>(a) : 0; }
    uint32_t MemoryRead32(uint32_t a) override { return check(a, "read32") ? mem().read<uint32_t>(a) : 0; }
    uint64_t MemoryRead64(uint32_t a) override { return check(a, "read64") ? mem().read<uint64_t>(a) : 0; }
    void MemoryWrite8(uint32_t a, uint8_t v) override { if (check(a, "write8")) mem().write(a, v); }
    void MemoryWrite16(uint32_t a, uint16_t v) override { if (check(a, "write16")) mem().write(a, v); }
    void MemoryWrite32(uint32_t a, uint32_t v) override { if (check(a, "write32")) mem().write(a, v); }
    void MemoryWrite64(uint32_t a, uint64_t v) override { if (check(a, "write64")) mem().write(a, v); }

    bool MemoryWriteExclusive8(uint32_t a, uint8_t v, uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t e) override { return cas(a, v, e); }

    template <class T>
    bool cas(uint32_t a, T v, T expected) {
        if (!check(a, "exclusive write")) return false;
        auto* p = mem().ptr<T>(a);
        return __atomic_compare_exchange_n(p, &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    void InterpreterFallback(uint32_t pc, size_t n) override;
    void CallSVC(uint32_t swi) override;
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override;
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~uint64_t(0); }
};

struct GuestThread::Frame {
    Callbacks cb;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    size_t processor_id = 0;
    bool returned = false;
    bool faulted = false;
    // longjmp support: applied after the current block halts.
    bool jump_pending = false;
    std::array<uint32_t, 16> jump_regs{};
    uint32_t jump_cpsr = 0;
};

bool Callbacks::check(uint32_t vaddr, const char* what) {
    if (vaddr >= Arena::kNullGuardEnd) return true;
    H32_ERROR("guest %s of address 0x%08x (null pointer)", what, vaddr);
    frame->faulted = true;
    frame->jit->HaltExecution();
    return false;
}

void Callbacks::InterpreterFallback(uint32_t pc, size_t n) {
    H32_ERROR("dynarmic asked for interpreter fallback at %s", describe_address(pc).c_str());
    frame->faulted = true;
    frame->jit->HaltExecution();
}

void Callbacks::CallSVC(uint32_t swi) {
    if (swi == kSvcReturnToHost) {
        frame->returned = true;
        frame->jit->HaltExecution();
        return;
    }
    thunks::dispatch(*thread, swi);
}

void Callbacks::ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    switch (e) {
    case E::SendEvent:
    case E::SendEventLocal:
    case E::WaitForInterrupt:
    case E::WaitForEvent:
    case E::Yield:
    case E::PreloadData:
    case E::PreloadDataWithIntentToWrite:
    case E::PreloadInstruction:
        return;  // hints: nothing to do
    default:
        break;
    }
    H32_ERROR("guest exception %d at %s", int(e), describe_address(pc).c_str());
    frame->faulted = true;
    frame->jit->HaltExecution();
}

// Linux/Android map "kuser helpers" at fixed addresses in the top page and
// old ARM code (libgcc atomics, TLS access) calls them directly.
void install_kuser_helpers() {
    auto& m = mem();
    const gaddr page = 0xFFFF0000;
    auto put = [&](gaddr a, std::initializer_list<uint32_t> words) {
        for (uint32_t w : words) {
            m.write<uint32_t>(a, w);
            a += 4;
        }
    };
    // Jump to a host thunk: ldr pc, [pc, #-4]; .word stub
    auto jump_to_thunk = [&](gaddr a, const char* name, ThunkFn fn) { put(a, {0xE51FF004, thunks::add(name, fn)}); };

    // 0xffff0fa0 __kuser_memory_barrier: dmb sy; bx lr
    put(page + 0x0FA0, {0xF57FF05F, 0xE12FFF1E});
    // 0xffff0fc0 __kuser_cmpxchg(old, new, ptr): ldrex/strex loop, r0=0 and C=1 on success
    put(page + 0x0FC0, {0xE1923F9F, 0xE0533000, 0x01823F91, 0x03330001, 0x0AFFFFFA, 0xE2730000, 0xE12FFF1E});
    // 0xffff0fe0 __kuser_get_tls
    jump_to_thunk(page + 0x0FE0, "__kuser_get_tls", [](GuestThread& t) { t.regs()[0] = t.tls_area(); });
    // 0xffff0f60 __kuser_cmpxchg64(const int64* old, const int64* new, int64* ptr)
    jump_to_thunk(page + 0x0F60, "__kuser_cmpxchg64", [](GuestThread& t) {
        auto& r = t.regs();
        uint64_t expected = mem().read<uint64_t>(r[0]);
        uint64_t desired = mem().read<uint64_t>(r[1]);
        auto* p = mem().ptr<uint64_t>(r[2]);
        r[0] = __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
    });
    // 0xffff0ffc __kuser_helper_version
    m.write<uint32_t>(page + 0x0FFC, 5);
}

void GuestThread::global_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        thunks::register_all();
        install_kuser_helpers();
        g_return_stub = mem().alloc_static(8, 8);
        mem().write<uint32_t>(g_return_stub, 0xEF000000 | kSvcReturnToHost);  // svc #0xFFFFFF
        mem().write<uint32_t>(g_return_stub + 4, 0xE7F000F0);                  // udf (never reached)
    });
}

GuestThread& GuestThread::current() {
    thread_local std::unique_ptr<GuestThread> t;
    if (!t) {
        global_init();
        t.reset(new GuestThread());
    }
    return *t;
}

GuestThread::GuestThread() {
    stack_base_ = mem().memalign(16, kStackSize);
    if (!stack_base_) fatal("cpu: cannot allocate guest stack");
    stack_top_ = stack_base_ + kStackSize;
    errno_addr_ = mem().calloc(1, 4);
    tls_area_ = mem().calloc(64, 4);
    mem().write<uint32_t>(tls_area_, tls_area_);  // slot 0: self
}

GuestThread::~GuestThread() {
    for (auto& f : frames_) g_ids.release(f->processor_id);
    mem().free(stack_base_);
    mem().free(errno_addr_);
    mem().free(tls_area_);
}

GuestThread::Frame& GuestThread::frame_at(int depth) {
    while (int(frames_.size()) <= depth) {
        auto f = std::make_unique<Frame>();
        f->cb.thread = this;
        f->cb.frame = f.get();
        f->processor_id = g_ids.acquire();

        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = &f->cb;
        cfg.processor_id = f->processor_id;
        cfg.global_monitor = &monitor();
        cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem().base());
        cfg.recompile_on_fastmem_failure = true;
        cfg.enable_cycle_counting = false;
        cfg.code_cache_size = frames_.empty() ? 64 * 1024 * 1024 : 16 * 1024 * 1024;
        cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
        f->jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
        frames_.push_back(std::move(f));
    }
    return *frames_[depth];
}

GuestResult GuestThread::call(gaddr fn, const GuestArgs& args) {
    const int depth = active_;
    // A nested call starts below the interrupted frame's SP, leaving a gap.
    gaddr sp = depth == 0 ? stack_top_ : (frames_[depth - 1]->jit->Regs()[13] - 256);
    Frame& f = frame_at(depth);
    auto& r = f.jit->Regs();

    const auto& w = args.words();
    size_t stack_words = w.size() > 4 ? w.size() - 4 : 0;
    sp = (sp - gaddr(stack_words * 4)) & ~gaddr(7);
    for (size_t i = 0; i < w.size(); i++) {
        if (i < 4) r[i] = w[i];
        else mem().write<uint32_t>(sp + gaddr((i - 4) * 4), w[i]);
    }
    for (size_t i = w.size(); i < 4; i++) r[i] = 0;
    r[13] = sp;
    r[14] = g_return_stub;
    r[15] = fn & ~1u;
    f.jit->SetCpsr(kCpsrUserMode | ((fn & 1) ? kCpsrThumb : 0));
    f.jit->SetFpscr(0);
    f.returned = f.faulted = f.jump_pending = false;

    active_++;
    while (true) {
        f.jit->Run();
        if (f.returned) break;
        if (f.faulted) {
            dump_state(f, "guest fault");
            fatal("aborting after guest fault");
        }
        if (f.jump_pending) {
            f.jump_pending = false;
            f.jit->Regs() = f.jump_regs;
            f.jit->SetCpsr(f.jump_cpsr);
            continue;
        }
        H32_WARN("guest halted for no known reason, resuming");
    }
    active_--;
    return {r[0], r[1]};
}

std::array<uint32_t, 16>& GuestThread::regs() { return frames_[active_ - 1]->jit->Regs(); }
std::array<uint32_t, 64>& GuestThread::ext_regs() { return frames_[active_ - 1]->jit->ExtRegs(); }
uint32_t GuestThread::cpsr() { return frames_[active_ - 1]->jit->Cpsr(); }

uint32_t GuestThread::arg_word(size_t i) {
    auto& r = regs();
    if (i < 4) return r[i];
    return mem().read<uint32_t>(r[13] + gaddr((i - 4) * 4));
}

void GuestThread::request_jump(const std::array<uint32_t, 16>& regs, uint32_t cpsr) {
    Frame& f = *frames_[active_ - 1];
    f.jump_pending = true;
    f.jump_regs = regs;
    f.jump_cpsr = cpsr;
    f.jit->HaltExecution();
}

void GuestThread::dump_state(Frame& f, const char* why) {
    auto& r = f.jit->Regs();
    H32_ERROR("=== %s (depth %d) ===", why, active_);
    H32_ERROR("pc %s", describe_address(r[15]).c_str());
    H32_ERROR("lr %s", describe_address(r[14]).c_str());
    for (int i = 0; i < 16; i += 4)
        H32_ERROR("r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    H32_ERROR("cpsr %08x", f.jit->Cpsr());
}

std::string describe_address(gaddr a) {
    char buf[64];
    snprintf(buf, sizeof buf, "0x%08x", a);
    std::string s = buf;
    if (const char* thunk = thunks::name_of_stub(a)) return s + " [thunk " + thunk + "]";
    if (auto sym = symbolize(a); !sym.empty()) s += " " + sym;
    return s;
}

}  // namespace h32
