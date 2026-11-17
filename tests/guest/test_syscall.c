#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
#include "check.h"

/* The raw exit syscall (1) ends only the calling thread, not the process. */
static void* exit_thread(void* arg) {
    *(volatile int*)arg = 1;
    syscall(__NR_exit, 0);
    *(volatile int*)arg = 2; /* never reached */
    return NULL;
}

TEST_MAIN({
    int progress = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, exit_thread, &progress) == 0, "create thread");
    CHECK(pthread_join(th, NULL) == 0, "join thread that called exit");
    CHECK(progress == 1, "exit syscall ended only that thread");
    CHECK(syscall(__NR_getpid) == getpid(), "syscall(getpid)");
    char* p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "anonymous mmap");
    if (p != MAP_FAILED) {
        memset(p, 7, 8192);
        CHECK(p[8191] == 7, "mmap memory usable");
        CHECK(mprotect(p, 4096, PROT_READ) == 0, "mprotect");
        CHECK(munmap(p, 8192) == 0, "munmap");
    }
    FILE* maps = fopen("/proc/self/maps", "r");
    char line[256]; int sees_self = 0;
    while (maps && fgets(line, sizeof line, maps)) if (strstr(line, "libthumbtest_syscall.so")) sees_self = 1;
    if (maps) fclose(maps);
    CHECK(sees_self, "/proc/self/maps shows guest modules");
})
