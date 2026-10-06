#include <dlfcn.h>
#include <string.h>
#include "check.h"

TEST_MAIN({
    void* h = dlopen("libthumbtest_helper.so", RTLD_NOW);
    CHECK(h != NULL, "dlopen sibling library");
    int (*answer)(void) = h ? (int (*)(void))dlsym(h, "helper_answer") : NULL;
    CHECK(answer && answer() == 42, "dlsym + call into sibling");
    CHECK(dlsym(h, "does_not_exist") == NULL && dlerror() != NULL, "dlsym missing -> dlerror");
    void* libc = dlopen("libc.so", RTLD_NOW);
    size_t (*my_strlen)(const char*) = libc ? (size_t (*)(const char*))dlsym(libc, "strlen") : NULL;
    CHECK(my_strlen && my_strlen("four") == 4, "dlsym system library function");
    CHECK(dlopen("libnope.so", RTLD_NOW) == NULL, "dlopen missing library fails");
})
