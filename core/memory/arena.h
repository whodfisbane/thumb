// The guest address space: one 4 GB host reservation where guest address X
// lives at host address base() + X. Converting pointers is a single add, and
// dynarmic's fastmem uses the same base so guest loads/stores hit it directly.
//
// Layout:
//   0x00000000 - 0x0000FFFF  null guard (no access)
//   0x00010000 - 0x00FFFFFF  static data: thunk stubs, JNI tables, libc globals
//   0x01000000 - 0x3FFFFFFF  loaded ELF images
//   0x40000000 - 0xEFFFFFFF  guest heap (TLSF): malloc, stacks, copies
//   0xF0000000 - 0xFFFFFFFF  unused
#pragma once

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string_view>

#include "common.h"

namespace h32 {

class Arena {
public:
    static constexpr gaddr kNullGuardEnd = 0x00010000;
    static constexpr gaddr kStaticBase = 0x00010000;
    static constexpr gaddr kStaticEnd = 0x01000000;
    static constexpr gaddr kImageBase = 0x01000000;
    static constexpr gaddr kImageEnd = 0x40000000;
    static constexpr gaddr kHeapBase = 0x40000000;
    static constexpr gaddr kHeapEnd = 0xF0000000;

    static Arena& get();

    uint8_t* base() const { return base_; }

    template <class T = void>
    T* ptr(gaddr a) const {
        return a ? reinterpret_cast<T*>(base_ + a) : nullptr;
    }
    bool contains(const void* p) const {
        auto u = reinterpret_cast<uintptr_t>(p);
        auto b = reinterpret_cast<uintptr_t>(base_);
        return u >= b && u - b < (uint64_t(1) << 32);
    }
    // Host pointer -> guest address. nullptr maps to 0; pointers outside the
    // arena are a bug in a thunk and are reported (returns 0).
    gaddr addr(const void* p) const;

    // Typed guest memory access (unaligned-safe).
    template <class T>
    T read(gaddr a) const {
        T v;
        std::memcpy(&v, base_ + a, sizeof(T));
        return v;
    }
    template <class T>
    void write(gaddr a, T v) const {
        std::memcpy(base_ + a, &v, sizeof(T));
    }
    // Guest C string (nullptr for address 0).
    const char* str(gaddr a) const { return ptr<const char>(a); }

    // Bump allocation of permanent guest data (never freed).
    gaddr alloc_static(size_t size, size_t align = 16);
    // Copies a host string into permanent guest memory.
    gaddr static_string(std::string_view s);
    // Reserves address space for an ELF image.
    gaddr alloc_image(size_t size);

    // Guest heap.
    gaddr malloc(size_t size);
    gaddr calloc(size_t n, size_t size);
    gaddr realloc(gaddr p, size_t size);
    gaddr memalign(size_t align, size_t size);
    void free(gaddr p);
    size_t usable_size(gaddr p);
    // Copies a host string into a fresh guest heap block.
    gaddr strdup(std::string_view s);

private:
    Arena();
    uint8_t* base_ = nullptr;
    void* tlsf_ = nullptr;
    std::mutex heap_mutex_;
    std::mutex static_mutex_;
    gaddr static_next_ = kStaticBase;
    gaddr image_next_ = kImageBase;
};

inline Arena& mem() { return Arena::get(); }

}  // namespace h32
