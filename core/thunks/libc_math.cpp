// <math.h> and <stdlib.h> random numbers. Soft-float ABI: floats travel in
// core registers, which the wrapper handles.
#include <cmath>
#include <cstdlib>

#include "thunks/libc_internal.h"
#include "thunks/thunks.h"

namespace h32 {
namespace {

void t_frexp(GuestThread& t) {
    ArgCursor c{t};
    uint64_t bits = c.dword();
    gaddr exp = c.word();
    double x;
    std::memcpy(&x, &bits, 8);
    int e = 0;
    double r = std::frexp(x, &e);
    if (exp) mem().write<int32_t>(exp, e);
    std::memcpy(&bits, &r, 8);
    set_ret64(t, bits);
}

float h_modff(float x, float* ip) { return std::modf(x, ip); }

}  // namespace

namespace thunks {

void register_libc_math() {
    using D1 = double(double);
    using D2 = double(double, double);
    using F1 = float(float);
    using F2 = float(float, float);
    add("sin", H32_WRAP(::sin, D1));
    add("cos", H32_WRAP(::cos, D1));
    add("tan", H32_WRAP(::tan, D1));
    add("asin", H32_WRAP(::asin, D1));
    add("acos", H32_WRAP(::acos, D1));
    add("atan", H32_WRAP(::atan, D1));
    add("atan2", H32_WRAP(::atan2, D2));
    add("exp", H32_WRAP(::exp, D1));
    add("log", H32_WRAP(::log, D1));
    add("log10", H32_WRAP(::log10, D1));
    add("pow", H32_WRAP(::pow, D2));
    add("sqrt", H32_WRAP(::sqrt, D1));
    add("fmod", H32_WRAP(::fmod, D2));
    add("floor", H32_WRAP(::floor, D1));
    add("ceil", H32_WRAP(::ceil, D1));
    add("fabs", H32_WRAP(::fabs, D1));
    add("ldexp", H32_WRAP(::ldexp, double(double, int)));
    add("frexp", t_frexp);

    add("sinf", H32_WRAP(::sinf, F1));
    add("cosf", H32_WRAP(::cosf, F1));
    add("tanf", H32_WRAP(::tanf, F1));
    add("sqrtf", H32_WRAP(::sqrtf, F1));
    add("powf", H32_WRAP(::powf, F2));
    add("atan2f", H32_WRAP(::atan2f, F2));
    add("fmodf", H32_WRAP(::fmodf, F2));
    add("floorf", H32_WRAP(::floorf, F1));
    add("ceilf", H32_WRAP(::ceilf, F1));
    add("roundf", H32_WRAP(::roundf, F1));
    add("fabsf", H32_WRAP(::fabsf, F1));
    add("modff", H32_WRAP(h_modff, float(float, float*)));

    H32_ADD(rand, int());
    H32_ADD(srand, void(unsigned));
    add("random", H32_WRAP(+[]() -> int { return int(::random()); }, int()));
    add("srandom", H32_WRAP(+[](unsigned s) { ::srandom(s); }, void(unsigned)));
}

}  // namespace thunks
}  // namespace h32
