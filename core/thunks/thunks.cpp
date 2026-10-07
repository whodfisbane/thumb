#include "thunks/thunks.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu/cpu.h"
#include "memory/arena.h"

namespace h32::thunks {

namespace {

struct Entry {
    std::string name;
    ThunkFn fn;  // nullptr = unimplemented
    gaddr stub;
    std::atomic<uint64_t> calls{0};
};

constexpr size_t kMaxThunks = 16384;

struct Registry {
    std::mutex m;
    std::vector<Entry*> entries;  // index = SVC number
    std::unordered_map<std::string, uint32_t> by_name;
    std::unordered_map<std::string, gaddr> data;
    std::unordered_map<gaddr, uint32_t> by_stub;
    Registry() { entries.reserve(kMaxThunks); }
};

Registry& reg() {
    static Registry r;
    return r;
}

// Import names may carry a version suffix ("memcpy@LIBC").
std::string_view base_name(std::string_view n) {
    auto at = n.find('@');
    return at == std::string_view::npos ? n : n.substr(0, at);
}

gaddr add_locked(Registry& r, std::string_view name, ThunkFn fn) {
    std::string key(base_name(name));
    if (auto it = r.by_name.find(key); it != r.by_name.end()) {
        Entry* e = r.entries[it->second];
        if (fn) e->fn = fn;  // later registration overrides
        return e->stub;
    }
    if (r.entries.size() >= kMaxThunks) fatal("thunks: too many thunks");
    uint32_t svc = uint32_t(r.entries.size());
    gaddr stub = mem().alloc_static(8, 8);
    mem().write<uint32_t>(stub, 0xEF000000 | svc);  // svc #n
    mem().write<uint32_t>(stub + 4, 0xE12FFF1E);    // bx lr
    auto* e = new Entry{key, fn, stub};
    r.entries.push_back(e);
    r.by_name.emplace(key, svc);
    r.by_stub.emplace(stub, svc);
    return stub;
}

}  // namespace

gaddr add(std::string_view name, ThunkFn fn) {
    auto& r = reg();
    std::lock_guard lk(r.m);
    return add_locked(r, name, fn);
}

void add_data(std::string_view name, gaddr addr) {
    auto& r = reg();
    std::lock_guard lk(r.m);
    r.data[std::string(base_name(name))] = addr;
}

std::optional<gaddr> lookup(std::string_view name) {
    auto& r = reg();
    std::lock_guard lk(r.m);
    std::string key(base_name(name));
    if (auto it = r.data.find(key); it != r.data.end()) return it->second;
    if (auto it = r.by_name.find(key); it != r.by_name.end()) {
        if (r.entries[it->second]->fn) return r.entries[it->second]->stub;
    }
    return std::nullopt;
}

gaddr resolve(std::string_view name, bool is_function) {
    if (auto a = lookup(name)) return *a;
    if (!is_function) {
        H32_WARN("unresolved data import '%.*s' -> 0", int(name.size()), name.data());
        return 0;
    }
    auto& r = reg();
    std::lock_guard lk(r.m);
    H32_DEBUG("no thunk for '%.*s' yet (stubbed)", int(name.size()), name.data());
    return add_locked(r, name, nullptr);
}

void dispatch(GuestThread& t, uint32_t svc) {
    auto& r = reg();
    if (svc >= r.entries.size()) {
        H32_ERROR("guest executed svc #0x%x which is not a thunk", svc);
        t.regs()[0] = 0;
        return;
    }
    Entry* e = r.entries[svc];
    uint64_t n = e->calls.fetch_add(1, std::memory_order_relaxed);
    t.last_svc.store(svc, std::memory_order_relaxed);
    t.thunk_calls.fetch_add(1, std::memory_order_relaxed);
    if (!e->fn) {
        if (n == 0)
            H32_WARN("UNIMPLEMENTED %s (r0=%08x r1=%08x r2=%08x) called from %s — returning 0", e->name.c_str(),
                     t.regs()[0], t.regs()[1], t.regs()[2], describe_address(t.regs()[14]).c_str());
        t.regs()[0] = 0;
        t.regs()[1] = 0;
        return;
    }
    H32_TRACE("-> %s(%08x, %08x, %08x, %08x)", e->name.c_str(), t.regs()[0], t.regs()[1], t.regs()[2], t.regs()[3]);
    e->fn(t);
}

const char* name_of(uint32_t svc) {
    auto& r = reg();
    return svc < r.entries.size() ? r.entries[svc]->name.c_str() : nullptr;
}

const char* name_of_stub(gaddr a) {
    auto& r = reg();
    std::lock_guard lk(r.m);
    auto it = r.by_stub.find(a & ~gaddr(7));
    return it == r.by_stub.end() ? nullptr : r.entries[it->second]->name.c_str();
}

std::vector<std::string> implemented() {
    auto& r = reg();
    std::lock_guard lk(r.m);
    std::vector<std::string> out;
    for (Entry* e : r.entries)
        if (e->fn && e->name.find("::") == std::string::npos) out.push_back(e->name);
    for (auto& [name, addr] : r.data) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

void dump_stats(size_t top) {
    auto& r = reg();
    std::vector<std::pair<uint64_t, Entry*>> called;
    {
        std::lock_guard lk(r.m);
        for (Entry* e : r.entries)
            if (uint64_t n = e->calls.load()) called.emplace_back(n, e);
    }
    std::sort(called.begin(), called.end(), [](auto& a, auto& b) { return a.first > b.first; });
    H32_INFO("--- thunk calls (%zu distinct) ---", called.size());
    for (size_t i = 0; i < called.size() && i < top; i++)
        H32_INFO("%10llu  %s", (unsigned long long)called[i].first, called[i].second->name.c_str());
    for (auto& [n, e] : called)
        if (!e->fn) H32_WARN("unimplemented but called: %s (%llu times)", e->name.c_str(), (unsigned long long)n);
}

void register_all() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_syscalls();
        register_libc_core();
        register_libc_string();
        register_libc_stdio();
        register_libc_math();
        register_libc_time();
        register_libc_pthread();
        register_libc_misc();
        register_libc_net();
        register_libc_extra();
        register_zlib();
        register_android();
        register_gles1();
        register_gles2();
        register_egl();
        register_native_window();
        register_dl();
    });
}

}  // namespace h32::thunks
