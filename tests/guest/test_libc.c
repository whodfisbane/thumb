#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <time.h>
#include "check.h"

static int cmp_int(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static void strings(void) {
    char buf[128];
    snprintf(buf, sizeof buf, "%d|%s|%5.2f|%lld|%x|%c|%%|%ld", -42, "hi", 3.14159, 1234567890123LL, 255, 'Z', 7L);
    CHECK(strcmp(buf, "-42|hi| 3.14|1234567890123|ff|Z|%|7") == 0, "snprintf mixed varargs");
    const char* six = "abcdef";
    snprintf(buf, 4, "%s", six);
    CHECK(strcmp(buf, "abc") == 0, "snprintf truncation");
    CHECK(snprintf(NULL, 0, "%s", "hello") == 5, "snprintf length");
    int a = 0; char word[16]; double d = 0;
    CHECK(sscanf("12 apples 2.5", "%d %15s %lf", &a, word, &d) == 3 && a == 12 && !strcmp(word, "apples") && d == 2.5, "sscanf");
    char* end = NULL;
    CHECK(strtol("  -123xyz", &end, 10) == -123 && *end == 'x', "strtol endptr");
    CHECK(strtoll("9000000000", NULL, 10) == 9000000000LL, "strtoll 64-bit");
    CHECK(strstr("hello world", "wor") != NULL && strchr("abc", 'c') != NULL, "strstr/strchr");
    char* dup = strdup("copy me");
    CHECK(dup && strcmp(dup, "copy me") == 0, "strdup");
    free(dup);
}

static void memory(void) {
    void* blocks[64];
    for (int i = 0; i < 64; i++) { blocks[i] = malloc(16 + i * 100); memset(blocks[i], i, 16 + i * 100); }
    int ok = 1;
    for (int i = 0; i < 64; i++) { blocks[i] = realloc(blocks[i], 32 + i * 200); ok &= ((unsigned char*)blocks[i])[10] == i; }
    for (int i = 0; i < 64; i++) free(blocks[i]);
    CHECK(ok, "malloc/realloc keep contents");
    int* z = calloc(100, sizeof(int)); int zero = 1;
    for (int i = 0; i < 100; i++) zero &= z[i] == 0;
    CHECK(zero, "calloc zeroes");
    free(z);
}

static void sorting(void) {
    int v[] = {5, 3, 9, 1, 7, 2, 8};
    qsort(v, 7, sizeof(int), cmp_int);
    CHECK(v[0] == 1 && v[3] == 5 && v[6] == 9, "qsort with guest comparator");
    int key = 7;
    int* found = bsearch(&key, v, 7, sizeof(int), cmp_int);
    CHECK(found && *found == 7, "bsearch with guest comparator");
}

static void misc(void) {
    setenv("THUMB_TEST_VAR", "yes", 1);
    CHECK(getenv("THUMB_TEST_VAR") && strcmp(getenv("THUMB_TEST_VAR"), "yes") == 0, "setenv/getenv");
    errno = 0;
    CHECK(strtol("99999999999999999999", NULL, 10) == 2147483647L && errno == ERANGE, "strtol overflow -> ERANGE (32-bit long)");
    time_t t = 86400 * 365;
    struct tm tm;
    CHECK(gmtime_r(&t, &tm) && tm.tm_year == 71 && tm.tm_yday == 0, "gmtime_r (32-bit struct tm)");
    wchar_t wc = 0;
    CHECK(mbrtowc(&wc, "\xc3\xa9", 2, NULL) == 2 && wc == 0xe9, "mbrtowc UTF-8");
}

TEST_MAIN(strings(); memory(); sorting(); misc())
