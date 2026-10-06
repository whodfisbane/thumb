// Thunk registry. Every function the guest imports resolves to an 8-byte
// ARM stub "svc #n; bx lr". When the guest calls it, dynarmic raises SVC n
// and we run host handler n, which reads the arguments from guest registers,
// does the work natively, and writes the result back to r0/r1.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "common.h"

namespace h32 {

class GuestThread;
using ThunkFn = void (*)(GuestThread&);

namespace thunks {

// Registers a host handler for guest import `name`; returns its stub address.
gaddr add(std::string_view name, ThunkFn fn);
// Registers a guest data symbol (e.g. __sF) living at `addr`.
void add_data(std::string_view name, gaddr addr);

// Resolves an import. Unknown functions get a stub that logs
// "UNIMPLEMENTED name" and returns 0, so loading never fails outright.
gaddr resolve(std::string_view name, bool is_function);
std::optional<gaddr> lookup(std::string_view name);

void dispatch(GuestThread& t, uint32_t svc);
const char* name_of(uint32_t svc);
// Name of the thunk whose stub contains guest address `a`, or nullptr.
const char* name_of_stub(gaddr a);

// Logs the `top` most-called thunks and every unimplemented import that was hit.
void dump_stats(size_t top);

// Registers every built-in thunk family. Safe to call more than once.
void register_all();

// Per-family registration (one per source file).
void register_libc_core();
void register_libc_string();
void register_libc_stdio();
void register_libc_math();
void register_libc_time();
void register_libc_pthread();
void register_libc_misc();
void register_zlib();
void register_android();
void register_gles1();

}  // namespace thunks
}  // namespace h32
