#include "memory/arena.h"

#include <sys/mman.h>

#include "tlsf.h"

namespace h32 {

static constexpr uint64_t kSpan = uint64_t(1) << 32;
// Extra no-access tail so an 8-byte access at 0xFFFFFFFC can't escape.
static constexpr uint64_t kTailGuard = 0x10000;

Arena& Arena::get() {
    static Arena arena;
    return arena;
}

Arena::Arena() {
    void* p = mmap(nullptr, kSpan + kTailGuard, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) fatal("arena: cannot reserve 4 GB of address space");
    base_ = static_cast<uint8_t*>(p);
    // Everything but the null guard is ordinary read/write memory; pages are
    // only committed when touched. Guest code doesn't need PROT_EXEC because
    // dynarmic translates it rather than running it.
    if (mprotect(base_ + kNullGuardEnd, kSpan - kNullGuardEnd, PROT_READ | PROT_WRITE) != 0)
        fatal("arena: mprotect failed");
    tlsf_ = tlsf_create_with_pool(base_ + kHeapBase, kHeapEnd - kHeapBase);
    if (!tlsf_) fatal("arena: tlsf init failed");
    H32_INFO("arena: guest 4 GB at host %p", base_);
}

gaddr Arena::addr(const void* p) const {
    if (!p) return 0;
    if (!contains(p)) {
        H32_ERROR("arena: host pointer %p is outside guest memory", p);
        return 0;
    }
    return gaddr(static_cast<const uint8_t*>(p) - base_);
}

gaddr Arena::alloc_static(size_t size, size_t align) {
    std::lock_guard lk(static_mutex_);
    gaddr a = (static_next_ + gaddr(align - 1)) & ~gaddr(align - 1);
    if (uint64_t(a) + size > kStaticEnd) fatal("arena: static region exhausted");
    static_next_ = a + gaddr(size);
    return a;
}

gaddr Arena::static_string(std::string_view s) {
    gaddr a = alloc_static(s.size() + 1, 1);
    std::memcpy(base_ + a, s.data(), s.size());
    base_[a + s.size()] = 0;
    return a;
}

gaddr Arena::alloc_image(size_t size) {
    std::lock_guard lk(static_mutex_);
    gaddr a = image_next_;
    uint64_t end = (uint64_t(a) + size + 0xFFFF) & ~uint64_t(0xFFFF);
    if (end > kImageEnd) fatal("arena: image region exhausted");
    image_next_ = gaddr(end) + 0x10000;  // gap between images
    return a;
}

gaddr Arena::malloc(size_t size) {
    std::lock_guard lk(heap_mutex_);
    void* p = tlsf_malloc(tlsf_, size ? size : 1);
    return p ? addr(p) : 0;
}

gaddr Arena::calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return 0;
    gaddr a = malloc(n * size);
    if (a) std::memset(base_ + a, 0, n * size);
    return a;
}

gaddr Arena::realloc(gaddr p, size_t size) {
    std::lock_guard lk(heap_mutex_);
    void* r = tlsf_realloc(tlsf_, ptr(p), size);
    return r ? addr(r) : 0;
}

gaddr Arena::memalign(size_t align, size_t size) {
    std::lock_guard lk(heap_mutex_);
    void* p = tlsf_memalign(tlsf_, align < 8 ? 8 : align, size ? size : 1);
    return p ? addr(p) : 0;
}

void Arena::free(gaddr p) {
    if (!p) return;
    if (p < kHeapBase || p >= kHeapEnd) {
        H32_WARN("free(0x%08x): not a heap pointer, ignored", p);
        return;
    }
    std::lock_guard lk(heap_mutex_);
    tlsf_free(tlsf_, ptr(p));
}

size_t Arena::usable_size(gaddr p) {
    return p ? tlsf_block_size(ptr(p)) : 0;
}

gaddr Arena::strdup(std::string_view s) {
    gaddr a = malloc(s.size() + 1);
    if (!a) return 0;
    std::memcpy(base_ + a, s.data(), s.size());
    base_[a + s.size()] = 0;
    return a;
}

}  // namespace h32
