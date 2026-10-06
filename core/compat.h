// Switches for THUMB's legacy-Android compatibility shims (per-app option
// "Old Android file paths", on by default).
#pragma once

#include <atomic>

namespace h32::compat {

// Virtual listings for folders modern Android hides, positioned asset fds.
inline std::atomic<bool> legacy_fs{true};

}  // namespace h32::compat
