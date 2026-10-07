// 32-bit handles for opaque host pointers (EGL objects, ANativeWindow*, ...).
// The guest stores these in 4-byte slots, host pointers are 8 bytes, so each
// pointer gets a small stable number instead. 0 <-> nullptr.
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace h32 {

class HandleTable {
public:
    // [tag]: high bits that make handles of different tables easy to tell apart in logs.
    explicit HandleTable(uint32_t tag) : tag_(tag) {}

    uint32_t to_guest(const void* p) {
        if (!p) return 0;
        std::lock_guard lk(m_);
        auto [it, added] = by_ptr_.try_emplace(p, 0);
        if (added) {
            ptrs_.push_back(const_cast<void*>(p));
            it->second = tag_ | uint32_t(ptrs_.size());
        }
        return it->second;
    }

    void* to_host(uint32_t h) {
        if (!h) return nullptr;
        std::lock_guard lk(m_);
        uint32_t i = h & 0x00FFFFFF;
        if ((h & 0xFF000000) != tag_ || i == 0 || i > ptrs_.size()) return nullptr;
        return ptrs_[i - 1];
    }

private:
    uint32_t tag_;
    std::mutex m_;
    std::unordered_map<const void*, uint32_t> by_ptr_;
    std::vector<void*> ptrs_;
};

// ANativeWindow* handles, shared by libandroid (android.cpp) and EGL (egl.cpp).
inline HandleTable& window_handles() {
    static HandleTable t(0x7A000000);
    return t;
}

}  // namespace h32
