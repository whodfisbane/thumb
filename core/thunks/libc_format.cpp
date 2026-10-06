// printf/scanf engines that pull arguments from guest registers/memory.
#include <cwchar>
#include <string>

#include "thunks/libc_internal.h"

namespace h32 {

uint32_t VarArgs::word() {
    if (from_cursor_) return cursor_.word();
    uint32_t v = mem().read<uint32_t>(ap_);
    ap_ += 4;
    return v;
}

uint64_t VarArgs::dword() {
    if (from_cursor_) return cursor_.dword();
    ap_ = (ap_ + 7) & ~7u;
    uint64_t v = mem().read<uint64_t>(ap_);
    ap_ += 8;
    return v;
}

namespace {

struct Spec {
    std::string flags;
    int width = -1;       // -1 = none
    int precision = -1;   // -1 = none
    enum Len { None, HH, H, L, LL, BigL, Z, J, T } len = None;
    char conv = 0;
};

// Parses one conversion starting after '%'. Returns chars consumed.
size_t parse_spec(const char* p, Spec& s, VarArgs* va) {
    const char* start = p;
    while (*p && std::strchr("-+ #0'", *p)) s.flags += *p++;
    if (*p == '*') {
        s.width = va ? int32_t(va->word()) : 0;
        if (s.width < 0) {
            s.flags += '-';
            s.width = -s.width;
        }
        p++;
    } else if (*p >= '0' && *p <= '9') {
        s.width = 0;
        while (*p >= '0' && *p <= '9') s.width = s.width * 10 + (*p++ - '0');
    }
    if (*p == '.') {
        p++;
        if (*p == '*') {
            s.precision = va ? int32_t(va->word()) : 0;
            p++;
        } else {
            s.precision = 0;
            while (*p >= '0' && *p <= '9') s.precision = s.precision * 10 + (*p++ - '0');
        }
    }
    switch (*p) {
    case 'h':
        if (p[1] == 'h') { s.len = Spec::HH; p += 2; } else { s.len = Spec::H; p++; }
        break;
    case 'l':
        if (p[1] == 'l') { s.len = Spec::LL; p += 2; } else { s.len = Spec::L; p++; }
        break;
    case 'q': s.len = Spec::LL; p++; break;
    case 'L': s.len = Spec::BigL; p++; break;
    case 'z': s.len = Spec::Z; p++; break;
    case 'j': s.len = Spec::J; p++; break;
    case 't': s.len = Spec::T; p++; break;
    default: break;
    }
    s.conv = *p ? *p++ : 0;
    return size_t(p - start);
}

std::string host_spec(const Spec& s, const char* length) {
    std::string f = "%" + s.flags;
    if (s.width >= 0) f += std::to_string(s.width);
    if (s.precision >= 0) f += "." + std::to_string(s.precision);
    f += length;
    f += s.conv;
    return f;
}

template <class T>
void append(std::string& out, const std::string& spec, T v) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, spec.c_str(), v);
    if (n < 0) return;
    if (size_t(n) < sizeof buf) {
        out.append(buf, size_t(n));
    } else {
        std::string big(size_t(n) + 1, '\0');
        snprintf(big.data(), big.size(), spec.c_str(), v);
        out.append(big.data(), size_t(n));
    }
}

}  // namespace

std::string guest_format(const char* fmt, VarArgs& va) {
    std::string out;
    if (!fmt) return out;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        Spec s;
        p += parse_spec(p, s, &va);
        const bool wide64 = s.len == Spec::LL || s.len == Spec::J;
        switch (s.conv) {
        case 'd':
        case 'i': {
            if (wide64) append(out, host_spec(s, "ll"), (long long)va.dword());
            else {
                int32_t v = int32_t(va.word());
                if (s.len == Spec::HH) v = int8_t(v);
                else if (s.len == Spec::H) v = int16_t(v);
                append(out, host_spec(s, ""), v);
            }
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            if (wide64) append(out, host_spec(s, "ll"), (unsigned long long)va.dword());
            else {
                uint32_t v = va.word();
                if (s.len == Spec::HH) v = uint8_t(v);
                else if (s.len == Spec::H) v = uint16_t(v);
                append(out, host_spec(s, ""), v);
            }
            break;
        }
        case 'c':
            if (s.len == Spec::L) append(out, host_spec(s, "l"), wint_t(va.word()));
            else append(out, host_spec(s, ""), int(va.word()));
            break;
        case 's': {
            gaddr a = va.word();
            if (s.len == Spec::L) append(out, host_spec(s, "l"), a ? mem().ptr<const wchar_t>(a) : L"(null)");
            else append(out, host_spec(s, ""), a ? mem().str(a) : "(null)");
            break;
        }
        case 'p': {
            Spec h = s;
            h.conv = 'x';
            h.flags += '#';
            append(out, host_spec(h, ""), va.word());
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            // long double is the same as double on 32-bit ARM.
            append(out, host_spec(s, ""), va.f64());
            break;
        case 'n': {
            gaddr a = va.word();
            if (a) mem().write<int32_t>(a, int32_t(out.size()));
            break;
        }
        case 'm':
            out += std::strerror(errno);
            break;
        case 0:
            return out;
        default:
            H32_WARN("printf: unsupported conversion '%c' in \"%s\"", s.conv, fmt);
            break;
        }
    }
    return out;
}

namespace {

// Generic scanf driver. Each conversion is run on its own through the host
// scanf with a trailing %n to learn how much input it consumed.
template <class Source>
int scan_impl(Source& src, const char* fmt, VarArgs& va) {
    int assigned = 0;
    std::string chunk;
    for (const char* p = fmt; *p;) {
        chunk.clear();
        // Literal text and whitespace are matched by the host as part of the
        // chunk preceding the next conversion.
        while (*p && !(*p == '%' && p[1] != '%')) {
            if (*p == '%') {
                chunk += "%%";
                p += 2;
            } else {
                chunk += *p++;
            }
        }
        if (!*p) {
            if (!chunk.empty() && src.run_literal(chunk) < 0 && assigned == 0) return EOF;
            break;
        }
        p++;  // '%'
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            p++;
        }
        Spec s;
        const char* spec_start = p;
        size_t used = parse_spec(p, s, nullptr);
        std::string set;  // for %[...]
        if (s.conv == '[') {
            const char* q = p + used;
            if (*q == '^') q++;
            if (*q == ']') q++;
            while (*q && *q != ']') q++;
            set.assign(p + used, q);
            if (*q) q++;
            used = size_t(q - spec_start);
        }
        p = spec_start + used;

        std::string hs = chunk + "%" + (suppress ? "*" : "");
        if (s.width >= 0) hs += std::to_string(s.width);
        const bool wide64 = s.len == Spec::LL || s.len == Spec::J;
        void* target = nullptr;
        gaddr ga = 0;
        int64_t tmp64 = 0;
        int32_t* n_target = nullptr;
        switch (s.conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X':
            hs += wide64 ? "ll" : (s.len == Spec::H ? "h" : s.len == Spec::HH ? "hh" : "");
            hs += s.conv;
            break;
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a':
            hs += (s.len == Spec::L || s.len == Spec::BigL) ? "l" : "";
            hs += s.conv;
            break;
        case 's': case 'c':
            hs += s.conv;
            break;
        case '[':
            hs += set;
            break;
        case 'p':
            hs += "llx";
            target = &tmp64;
            break;
        case 'n':
            if (!suppress) n_target = mem().ptr<int32_t>(va.word());
            if (n_target) *n_target = src.consumed();
            continue;
        default:
            H32_WARN("scanf: unsupported conversion '%c' in \"%s\"", s.conv, fmt);
            return assigned;
        }
        if (!suppress) {
            ga = va.word();
            if (!target) target = mem().ptr<void>(ga);
        }
        int r = src.run(hs, suppress ? nullptr : target);
        if (r == EOF) return assigned ? assigned : EOF;
        if (r == 0 && !suppress) return assigned;
        if (r < 0) return assigned;
        if (!suppress) {
            if (s.conv == 'p') mem().write<uint32_t>(ga, uint32_t(tmp64));
            assigned++;
        }
    }
    return assigned;
}

struct StringSource {
    const char* base;
    const char* cur;
    int consumed() const { return int(cur - base); }
    // Returns host scanf result; advances on success. -2 = no match.
    int run(const std::string& spec, void* target) {
        int n = -1;
        std::string f = spec + "%n";
        int r = target ? sscanf(cur, f.c_str(), target, &n) : sscanf(cur, f.c_str(), &n);
        if (n < 0) return r == EOF ? EOF : (target ? 0 : -2);
        cur += n;
        return target ? r : 1;
    }
    int run_literal(const std::string& lit) {
        int n = -1;
        std::string f = lit + "%n";
        sscanf(cur, f.c_str(), &n);
        if (n < 0) return -1;
        cur += n;
        return 0;
    }
};

struct FileSource {
    FILE* f;
    long start;
    int consumed() const { return int(ftell(f) - start); }
    int run(const std::string& spec, void* target) {
        int n = -1;
        std::string fs = spec + "%n";
        int r = target ? fscanf(f, fs.c_str(), target, &n) : fscanf(f, fs.c_str(), &n);
        if (n < 0) return r == EOF ? EOF : (target ? 0 : -2);
        return target ? r : 1;
    }
    int run_literal(const std::string& lit) {
        int n = -1;
        std::string fs = lit + "%n";
        fscanf(f, fs.c_str(), &n);
        return n < 0 ? -1 : 0;
    }
};

}  // namespace

int guest_sscanf(const char* input, const char* fmt, VarArgs& va) {
    if (!input || !fmt) return EOF;
    StringSource s{input, input};
    return scan_impl(s, fmt, va);
}

int guest_fscanf(FILE* f, const char* fmt, VarArgs& va) {
    if (!f || !fmt) return EOF;
    FileSource s{f, ftell(f)};
    return scan_impl(s, fmt, va);
}

}  // namespace h32
