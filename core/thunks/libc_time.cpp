// <time.h>, <sys/time.h>. On 32-bit bionic time_t and long are 32-bit, and
// struct tm carries tm_gmtoff (long) and tm_zone (pointer) after the ints.
#include <sys/time.h>

#include <ctime>
#include <cwchar>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

void tm_to_guest(gaddr g, const std::tm& t) {
    auto& m = mem();
    const int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; i++) m.write<int32_t>(g + i * 4, v[i]);
    m.write<int32_t>(g + 36, int32_t(t.tm_gmtoff));
    m.write<uint32_t>(g + 40, 0);  // tm_zone: not provided
}

std::tm tm_from_guest(gaddr g) {
    auto& m = mem();
    std::tm t{};
    int* v[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; i++) *v[i] = m.read<int32_t>(g + i * 4);
    t.tm_gmtoff = m.read<int32_t>(g + 36);
    return t;
}

void t_time(GuestThread& t) {
    time_t now = std::time(nullptr);
    if (gaddr p = t.regs()[0]) mem().write<int32_t>(p, int32_t(now));
    set_ret32(t, uint32_t(now));
}

void t_gettimeofday(GuestThread& t) {
    timeval tv;
    ::gettimeofday(&tv, nullptr);
    if (gaddr p = t.regs()[0]) {
        mem().write<int32_t>(p, int32_t(tv.tv_sec));
        mem().write<int32_t>(p + 4, int32_t(tv.tv_usec));
    }
    set_ret32(t, 0);
}

void t_clock(GuestThread& t) { set_ret32(t, uint32_t(std::clock())); }

void t_difftime(GuestThread& t) {
    double d = double(int32_t(t.regs()[0])) - double(int32_t(t.regs()[1]));
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    set_ret64(t, bits);
}

template <std::tm* (*Fn)(const time_t*, std::tm*)>
void t_tm_conv(GuestThread& t) {
    gaddr src = t.regs()[0];
    if (!src) return set_ret32(t, 0);
    time_t tt = mem().read<int32_t>(src);
    std::tm out{};
    if (!Fn(&tt, &out)) return set_ret32(t, 0);
    gaddr buf = thread_scratch(kScratchTm, bionic::kTmSize);
    tm_to_guest(buf, out);
    set_ret32(t, buf);
}

void t_mktime(GuestThread& t) {
    gaddr g = t.regs()[0];
    std::tm tm = tm_from_guest(g);
    time_t r = std::mktime(&tm);
    tm_to_guest(g, tm);  // mktime normalizes its argument
    set_ret32(t, uint32_t(r));
}

// size_t strftime(char* s, size_t max, const char* fmt, const struct tm*)
void t_strftime(GuestThread& t) {
    std::tm tm = tm_from_guest(t.regs()[3]);
    set_ret32(t, uint32_t(std::strftime(mem().ptr<char>(t.regs()[0]), t.regs()[1], mem().str(t.regs()[2]), &tm)));
}
void t_wcsftime(GuestThread& t) {
    std::tm tm = tm_from_guest(t.regs()[3]);
    set_ret32(t, uint32_t(std::wcsftime(mem().ptr<wchar_t>(t.regs()[0]), t.regs()[1], mem().ptr<const wchar_t>(t.regs()[2]), &tm)));
}

}  // namespace

namespace thunks {

void register_libc_time() {
    add("time", t_time);
    add("gettimeofday", t_gettimeofday);
    add("clock", t_clock);
    add("difftime", t_difftime);
    add("localtime", t_tm_conv<::localtime_r>);
    add("gmtime", t_tm_conv<::gmtime_r>);
    add("mktime", t_mktime);
    add("strftime", t_strftime);
    add("wcsftime", t_wcsftime);
}

}  // namespace thunks
}  // namespace h32
