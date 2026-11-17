/* Shared body of the packed-relocation tests (test_relr, test_aps2,
 * test_aps2_relr): each links this with a different --pack-dyn-relocs mode.
 * Lots of pointers in writable data (hidden, so the compiler can't fold the
 * loads) give relative relocations in runs and with gaps (RELR address words
 * and bitmaps), and a few imports give symbolic ones (APS2 groups by info). */
#pragma once
#include <stdio.h>
#include <string.h>
#include "check.h"

#define HIDDEN __attribute__((visibility("hidden")))

HIDDEN int ints[64];
#define P8(b) &ints[b], &ints[b + 1], &ints[b + 2], &ints[b + 3], &ints[b + 4], &ints[b + 5], &ints[b + 6], &ints[b + 7]
HIDDEN int* ptrs[64] = {P8(0), P8(8), P8(16), P8(24), P8(32), P8(40), P8(48), P8(56)};

/* Pointer, int, pointer, int, ...: RELR bitmaps with every other bit set. */
struct mix { const char* s; int n; };
#define MX(i) {#i, i}
HIDDEN struct mix mixes[40] = {
    MX(0),  MX(1),  MX(2),  MX(3),  MX(4),  MX(5),  MX(6),  MX(7),  MX(8),  MX(9),
    MX(10), MX(11), MX(12), MX(13), MX(14), MX(15), MX(16), MX(17), MX(18), MX(19),
    MX(20), MX(21), MX(22), MX(23), MX(24), MX(25), MX(26), MX(27), MX(28), MX(29),
    MX(30), MX(31), MX(32), MX(33), MX(34), MX(35), MX(36), MX(37), MX(38), MX(39),
};

/* Imports: symbolic R_ARM_ABS32 relocations. */
HIDDEN void* funcs[3] = {(void*)&printf, (void*)&strlen, (void*)&memcpy};

static void check_relocs(void) {
    int bad = 0;
    for (int i = 0; i < 64; i++) bad += ptrs[i] != &ints[i];
    CHECK(bad == 0, "relative relocations (run of pointers)");

    bad = 0;
    for (int i = 0; i < 40; i++) {
        char want[4];
        snprintf(want, sizeof want, "%d", i);
        bad += mixes[i].n != i || strcmp(mixes[i].s, want) != 0;
    }
    CHECK(bad == 0, "relative relocations (with gaps)");

    CHECK(funcs[0] == (void*)&printf, "symbolic relocation (printf)");
    CHECK(((size_t (*)(const char*))funcs[1])("abcd") == 4, "symbolic relocation (strlen) callable");
    char dst[4] = {0};
    ((void* (*)(void*, const void*, size_t))funcs[2])(dst, "xyz", 3);
    CHECK(strcmp(dst, "xyz") == 0, "symbolic relocation (memcpy) callable");
}
