// <string.h>, <ctype.h>, <wchar.h>, <wctype.h>, <stdlib.h> number parsing,
// <locale.h>.
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <strings.h>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

// strchr & co return a pointer into their argument, which is guest memory,
// so the automatic pointer conversion maps them straight back.
const char* h_strchr(const char* s, int c) { return std::strchr(s, c); }
const char* h_strrchr(const char* s, int c) { return std::strrchr(s, c); }
const char* h_strstr(const char* a, const char* b) { return std::strstr(a, b); }
const char* h_strpbrk(const char* a, const char* b) { return std::strpbrk(a, b); }
const void* h_memchr(const void* s, int c, size_t n) { return std::memchr(s, c, n); }
const wchar_t* h_wmemchr(const wchar_t* s, wchar_t c, size_t n) { return std::wmemchr(s, c, n); }

size_t h_strlcpy(char* dst, const char* src, size_t size) {
    size_t n = std::strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        std::memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

void t_strdup(GuestThread& t) {
    const char* s = mem().str(t.regs()[0]);
    set_ret32(t, s ? mem().strdup(s) : 0);
}

void t_strerror(GuestThread& t) {
    const char* msg = std::strerror(int32_t(t.regs()[0]));
    gaddr buf = thread_scratch(kScratchStrerror, 256);
    std::strncpy(mem().ptr<char>(buf), msg, 255);
    mem().ptr<char>(buf)[255] = 0;
    set_ret32(t, buf);
}

void t_perror(GuestThread& t) {
    const char* s = mem().str(t.regs()[0]);
    int e = mem().read<int32_t>(t.errno_addr());
    H32_INFO("[guest perror] %s%s%s", s ? s : "", s && *s ? ": " : "", std::strerror(e));
}

// strtok keeps a pointer to the guest string between calls; that's fine as
// it is a host pointer into the arena.
char* h_strtok(char* s, const char* delim) { return std::strtok(s, delim); }

// Number parsing with a `char** endptr` (guest pointers are 4 bytes).
template <class R, R (*Fn)(const char*, char**, int)>
void t_strto_int(GuestThread& t) {
    const char* s = mem().str(t.regs()[0]);
    gaddr endp = t.regs()[1];
    char* end = nullptr;
    errno = 0;
    R v = Fn(s, &end, int32_t(t.regs()[2]));
    sync_guest_errno(t);
    if (endp) mem().write<uint32_t>(endp, mem().addr(end));
    if constexpr (sizeof(R) == 8 && (std::is_same_v<R, long long> || std::is_same_v<R, unsigned long long>)) {
        set_ret64(t, uint64_t(v));
    } else {
        // long / unsigned long: clamp to the guest's 32-bit range.
        if constexpr (std::is_signed_v<R>) {
            if (v > INT32_MAX) { v = INT32_MAX; mem().write<int32_t>(t.errno_addr(), ERANGE); }
            if (v < INT32_MIN) { v = INT32_MIN; mem().write<int32_t>(t.errno_addr(), ERANGE); }
        } else {
            if (v > UINT32_MAX && v < R(-1) - UINT32_MAX) { v = UINT32_MAX; mem().write<int32_t>(t.errno_addr(), ERANGE); }
        }
        set_ret32(t, uint32_t(v));
    }
}

void t_strtod(GuestThread& t) {
    const char* s = mem().str(t.regs()[0]);
    gaddr endp = t.regs()[1];
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s, &end);
    sync_guest_errno(t);
    if (endp) mem().write<uint32_t>(endp, mem().addr(end));
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    set_ret64(t, bits);
}

void t_strtof(GuestThread& t) {
    const char* s = mem().str(t.regs()[0]);
    gaddr endp = t.regs()[1];
    char* end = nullptr;
    float v = std::strtof(s, &end);
    if (endp) mem().write<uint32_t>(endp, mem().addr(end));
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    set_ret32(t, bits);
}

// atol returns a 32-bit long on the guest.
int h_atol(const char* s) { return int(std::atol(s)); }

// ---- locale: Android only has the C/UTF-8 locale ----
void t_setlocale(GuestThread& t) {
    gaddr buf = thread_scratch(kScratchLocale, 8);
    std::strcpy(mem().ptr<char>(buf), "C");
    set_ret32(t, buf);
}

// ---- multibyte: bionic is always UTF-8; guest mbstate_t is 4 bytes ----
// size_t mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps)
void t_mbrtowc(GuestThread& t) {
    gaddr pwc = t.regs()[0];
    const auto* s = mem().ptr<const uint8_t>(t.regs()[1]);
    size_t n = t.regs()[2];
    if (!s) return set_ret32(t, 0);
    if (n == 0) return set_ret32(t, uint32_t(-2));
    uint32_t c = s[0];
    int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!len) {
        mem().write<int32_t>(t.errno_addr(), EILSEQ);
        return set_ret32(t, uint32_t(-1));
    }
    if (n < size_t(len)) return set_ret32(t, uint32_t(-2));
    uint32_t wc = len == 1 ? c : c & (0x3F >> (len - 1));
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            mem().write<int32_t>(t.errno_addr(), EILSEQ);
            return set_ret32(t, uint32_t(-1));
        }
        wc = wc << 6 | (s[i] & 0x3F);
    }
    if (pwc) mem().write<uint32_t>(pwc, wc);
    set_ret32(t, wc ? uint32_t(len) : 0);
}

// size_t wcrtomb(char* s, wchar_t wc, mbstate_t* ps)
void t_wcrtomb(GuestThread& t) {
    auto* s = mem().ptr<uint8_t>(t.regs()[0]);
    uint32_t wc = t.regs()[1];
    if (!s) return set_ret32(t, 1);
    if (wc < 0x80) { s[0] = uint8_t(wc); return set_ret32(t, 1); }
    if (wc < 0x800) { s[0] = 0xC0 | wc >> 6; s[1] = 0x80 | (wc & 0x3F); return set_ret32(t, 2); }
    if (wc < 0x10000) {
        s[0] = 0xE0 | wc >> 12; s[1] = 0x80 | ((wc >> 6) & 0x3F); s[2] = 0x80 | (wc & 0x3F);
        return set_ret32(t, 3);
    }
    if (wc < 0x110000) {
        s[0] = 0xF0 | wc >> 18; s[1] = 0x80 | ((wc >> 12) & 0x3F); s[2] = 0x80 | ((wc >> 6) & 0x3F); s[3] = 0x80 | (wc & 0x3F);
        return set_ret32(t, 4);
    }
    mem().write<int32_t>(t.errno_addr(), EILSEQ);
    set_ret32(t, uint32_t(-1));
}

wint_t h_btowc(int c) { return (c >= 0 && c < 0x80) ? wint_t(c) : WEOF; }
int h_wctob(wint_t c) { return c < 0x80 ? int(c) : EOF; }

// wctype_t is a host pointer-sized cookie on glibc; give the guest a small
// index into this table instead.
const char* const kWctypeNames[] = {"", "alnum", "alpha", "blank", "cntrl", "digit", "graph",
                                    "lower", "print", "punct", "space", "upper", "xdigit"};
void t_wctype(GuestThread& t) {
    const char* name = mem().str(t.regs()[0]);
    for (uint32_t i = 1; i < std::size(kWctypeNames); i++)
        if (name && std::strcmp(name, kWctypeNames[i]) == 0) return set_ret32(t, i);
    set_ret32(t, 0);
}
void t_iswctype(GuestThread& t) {
    uint32_t idx = t.regs()[1];
    if (idx == 0 || idx >= std::size(kWctypeNames)) return set_ret32(t, 0);
    set_ret32(t, std::iswctype(wint_t(t.regs()[0]), std::wctype(kWctypeNames[idx])) ? 1 : 0);
}

}  // namespace

namespace thunks {

void register_libc_string() {
    H32_ADD(memcmp, int(const void*, const void*, size_t));
    H32_ADD(memcpy, void*(void*, const void*, size_t));
    H32_ADD(memmove, void*(void*, const void*, size_t));
    H32_ADD(memset, void*(void*, int, size_t));
    add("memchr", H32_WRAP(h_memchr, const void*(const void*, int, size_t)));
    H32_ADD(strlen, size_t(const char*));
    H32_ADD(strcmp, int(const char*, const char*));
    H32_ADD(strncmp, int(const char*, const char*, size_t));
    H32_ADD(strcasecmp, int(const char*, const char*));
    H32_ADD(strncasecmp, int(const char*, const char*, size_t));
    H32_ADD(strcoll, int(const char*, const char*));
    H32_ADD(strcpy, char*(char*, const char*));
    H32_ADD(strncpy, char*(char*, const char*, size_t));
    H32_ADD(stpcpy, char*(char*, const char*));
    H32_ADD(strcat, char*(char*, const char*));
    H32_ADD(strncat, char*(char*, const char*, size_t));
    H32_ADD(strspn, size_t(const char*, const char*));
    H32_ADD(strcspn, size_t(const char*, const char*));
    H32_ADD(strxfrm, size_t(char*, const char*, size_t));
    add("strlcpy", H32_WRAP(h_strlcpy, size_t(char*, const char*, size_t)));
    add("strchr", H32_WRAP(h_strchr, const char*(const char*, int)));
    add("strrchr", H32_WRAP(h_strrchr, const char*(const char*, int)));
    add("strstr", H32_WRAP(h_strstr, const char*(const char*, const char*)));
    add("strpbrk", H32_WRAP(h_strpbrk, const char*(const char*, const char*)));
    add("strtok", H32_WRAP(h_strtok, char*(char*, const char*)));
    add("strdup", t_strdup);
    add("strerror", t_strerror);
    add("perror", t_perror);

    add("strtol", t_strto_int<long, std::strtol>);
    add("strtoul", t_strto_int<unsigned long, std::strtoul>);
    add("strtoll", t_strto_int<long long, std::strtoll>);
    add("strtoull", t_strto_int<unsigned long long, std::strtoull>);
    add("strtod", t_strtod);
    add("strtof", t_strtof);
    H32_ADD(atoi, int(const char*));
    add("atol", H32_WRAP(h_atol, int(const char*)));
    H32_ADD(atof, double(const char*));

    // ctype (C locale)
    H32_ADD(isalnum, int(int));
    H32_ADD(isalpha, int(int));
    H32_ADD(iscntrl, int(int));
    H32_ADD(isdigit, int(int));
    H32_ADD(isgraph, int(int));
    H32_ADD(islower, int(int));
    H32_ADD(isprint, int(int));
    H32_ADD(ispunct, int(int));
    H32_ADD(isspace, int(int));
    H32_ADD(isupper, int(int));
    H32_ADD(isxdigit, int(int));
    H32_ADD(tolower, int(int));
    H32_ADD(toupper, int(int));

    // wide characters (wchar_t is 4 bytes on both sides)
    H32_ADD(wcslen, size_t(const wchar_t*));
    H32_ADD(wcscoll, int(const wchar_t*, const wchar_t*));
    H32_ADD(wcsxfrm, size_t(wchar_t*, const wchar_t*, size_t));
    H32_ADD(wmemcmp, int(const wchar_t*, const wchar_t*, size_t));
    H32_ADD(wmemcpy, wchar_t*(wchar_t*, const wchar_t*, size_t));
    H32_ADD(wmemmove, wchar_t*(wchar_t*, const wchar_t*, size_t));
    H32_ADD(wmemset, wchar_t*(wchar_t*, wchar_t, size_t));
    add("wmemchr", H32_WRAP(h_wmemchr, const wchar_t*(const wchar_t*, wchar_t, size_t)));
    H32_ADD(towlower, wint_t(wint_t));
    H32_ADD(towupper, wint_t(wint_t));
    add("btowc", H32_WRAP(h_btowc, wint_t(int)));
    add("wctob", H32_WRAP(h_wctob, int(wint_t)));
    add("mbrtowc", t_mbrtowc);
    add("wcrtomb", t_wcrtomb);
    add("wctype", t_wctype);
    add("iswctype", t_iswctype);

    add("setlocale", t_setlocale);
}

}  // namespace thunks
}  // namespace h32
