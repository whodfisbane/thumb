#include "common.h"

#include <atomic>
#include <cstdarg>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace h32 {

static std::atomic<LogLevel> g_level{LogLevel::Info};

void set_log_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level.load(std::memory_order_relaxed); }

static void vlog(LogLevel level, const char* fmt, va_list ap) {
#ifdef __ANDROID__
    static const int prio[] = {ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_vprint(prio[int(level)], "thumb", fmt, ap);
#else
    static const char* tag[] = {"T", "D", "I", "W", "E"};
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "[%s] %s\n", tag[int(level)], buf);
#endif
}

void log_write(LogLevel level, const char* fmt, ...) {
    if (level < log_level()) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(LogLevel::Error, fmt, ap);
    va_end(ap);
    abort();
}

}  // namespace h32
