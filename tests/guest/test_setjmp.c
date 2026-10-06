#include <setjmp.h>
#include "check.h"

static jmp_buf env;
static int depth;
static void deep(int n) {
    volatile char pad[64]; pad[0] = (char)n;
    if (n == 0) longjmp(env, 42);
    deep(n - 1);
    depth += pad[0];
}

TEST_MAIN({
    volatile int counter = 0;
    int r = setjmp(env);
    if (r == 0) { counter = 1; deep(10); }
    CHECK(r == 42 && counter == 1, "longjmp out of deep recursion");
    r = setjmp(env);
    if (r == 0) longjmp(env, 0);
    CHECK(r == 1, "longjmp(0) returns 1");
    volatile double x = 1.5;  /* volatile: C only guarantees volatile locals after longjmp */
    r = setjmp(env);
    if (r == 0) { x = 2.5; longjmp(env, 1); }
    CHECK(x == 2.5, "VFP state across longjmp");
})
