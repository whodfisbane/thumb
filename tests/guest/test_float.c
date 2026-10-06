#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

/* Soft-float ABI: doubles in register pairs and on the stack, floats in core regs. */
static double many(double a, float b, int c, double d, float e, long long f, double g) {
    return a + b + c + d + e + (double)f + g;
}

TEST_MAIN({
    CHECK(fabs(sqrt(2.0) - 1.41421356) < 1e-6, "sqrt");
    CHECK(fabs(pow(2.0, 10.0) - 1024.0) < 1e-9, "pow");
    CHECK(fabs(sinf(0.5f) - 0.47942554f) < 1e-5f, "sinf (float return in r0)");
    CHECK(many(1.5, 2.5f, 3, 4.25, 5.75f, 100LL, 0.5) == 117.5, "mixed float/double/int64 arguments");
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f %.1e %g", 3.14159, 12345.0, 0.5);
    CHECK(strcmp(buf, "3.142 1.2e+04 0.5") == 0, "printf doubles in varargs");
    CHECK(atof("2.75") == 2.75 && strtod("1e3", NULL) == 1000.0, "atof/strtod");
    int e; double m = frexp(8.0, &e);
    CHECK(m == 0.5 && e == 4, "frexp");
    CHECK(lrint(2.6) == 3 && (long)lrintf(-1.4f) == -1, "lrint/lrintf");
})
