// Shared types and logging for host32.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace h32 {

// A guest (32-bit ARM) virtual address.
using gaddr = uint32_t;

enum class LogLevel { Trace, Debug, Info, Warn, Error };

void log_write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void set_log_level(LogLevel level);
LogLevel log_level();

[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace h32

#define H32_TRACE(...) do { if (::h32::log_level() <= ::h32::LogLevel::Trace) ::h32::log_write(::h32::LogLevel::Trace, __VA_ARGS__); } while (0)
#define H32_DEBUG(...) do { if (::h32::log_level() <= ::h32::LogLevel::Debug) ::h32::log_write(::h32::LogLevel::Debug, __VA_ARGS__); } while (0)
#define H32_INFO(...) ::h32::log_write(::h32::LogLevel::Info, __VA_ARGS__)
#define H32_WARN(...) ::h32::log_write(::h32::LogLevel::Warn, __VA_ARGS__)
#define H32_ERROR(...) ::h32::log_write(::h32::LogLevel::Error, __VA_ARGS__)
