// Helpers shared by the libc thunk families.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "thunks/wrap.h"

namespace h32 {

// ---- bionic (32-bit) struct layouts the guest expects ----
namespace bionic {
constexpr size_t kFileSize = 84;  // sizeof(FILE) on 32-bit bionic
constexpr size_t kStatSize = 104;
constexpr size_t kTmSize = 44;    // struct tm with tm_gmtoff + tm_zone
constexpr int kEaiFail = 4;
}  // namespace bionic

// ---- filesystem path mapping (Linux harness: Android paths -> local dirs) ----
void add_path_mapping(const std::string& guest_prefix, const std::string& host_prefix);
std::string map_path(const char* guest_path);

// ---- shared with the raw syscall layer (thunks/syscalls.cpp); return -errno on failure ----
int32_t guest_mmap(gaddr hint, uint32_t len, uint32_t prot, int32_t flags, int fd, uint64_t offset);
int32_t guest_munmap(gaddr a);
int32_t guest_open(int dirfd, const char* guest_path, int flags, int mode);
int32_t guest_raw_syscall(GuestThread& t, uint32_t nr, const uint32_t (&args)[6]);

// ---- FILE* handles ----
// Guest FILE* values are guest addresses of small placeholder structs; this
// maps them to host FILE*. stdin/stdout/stderr live in the guest __sF array.
FILE* host_file(gaddr f);
gaddr guest_file_wrap(FILE* host);  // registers a new guest FILE*
void guest_file_forget(gaddr f);
bool is_guest_console(gaddr f);     // guest stdout/stderr -> our log

// ---- varargs ----
// Source of variadic arguments: either the remaining arguments of the current
// call (registers then stack) or an ARM va_list (a pointer into memory).
class VarArgs {
public:
    explicit VarArgs(ArgCursor c) : cursor_(c), from_cursor_(true) {}
    explicit VarArgs(GuestThread& t, gaddr va_list) : cursor_{t}, ap_(va_list), from_cursor_(false) {}
    uint32_t word();
    uint64_t dword();
    double f64() {
        uint64_t v = dword();
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }

private:
    ArgCursor cursor_;
    gaddr ap_ = 0;
    bool from_cursor_;
};

// printf-family formatting with guest arguments (32-bit long, guest %s/%p).
std::string guest_format(const char* fmt, VarArgs& va);

// scanf-family; `input` is either a string (sscanf) or a FILE (fscanf).
int guest_sscanf(const char* input, const char* fmt, VarArgs& va);
int guest_fscanf(FILE* f, const char* fmt, VarArgs& va);

// Per-thread scratch buffer in guest memory for functions that return
// pointers to static storage (strerror, localtime, ...). `slot` picks a
// separate buffer per function so their results don't clobber each other.
gaddr thread_scratch(int slot, size_t size);
enum ScratchSlot { kScratchStrerror, kScratchTm, kScratchLocale, kScratchInetNtoa, kScratchGai, kScratchDlerror, kScratchEnv, kScratchCount };

}  // namespace h32
