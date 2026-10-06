#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

TEST_MAIN({
    const char* dir = getenv("THUMB_TEST_DIR");
    if (!dir) dir = "/tmp";
    char path[512], sub[512];
    snprintf(sub, sizeof sub, "%s/thumb-test-dir", dir);
    snprintf(path, sizeof path, "%s/data.txt", sub);
    mkdir(sub, 0755);

    FILE* f = fopen(path, "w");
    CHECK(f != NULL, "fopen for writing");
    fprintf(f, "line one %d\nline two\n", 1);
    CHECK(fwrite("tail", 1, 4, f) == 4, "fwrite");
    fclose(f);

    struct stat st;
    int sr = stat(path, &st);
    printf("stat: r=%d size=%lld mode=%o nlink=%u blksize=%lu\n", sr, (long long)st.st_size, (unsigned)st.st_mode, (unsigned)st.st_nlink, (unsigned long)st.st_blksize);
    CHECK(sr == 0 && st.st_size == 24 && S_ISREG(st.st_mode) && st.st_nlink == 1, "stat size/mode/nlink (bionic32 struct stat)");

    f = fopen(path, "r");
    char line[64]; int n = 0;
    CHECK(fgets(line, sizeof line, f) && strcmp(line, "line one 1\n") == 0, "fgets");
    CHECK(fscanf(f, "line %63s", line) == 1 && strcmp(line, "two") == 0, "fscanf");
    fseek(f, -4, SEEK_END);
    CHECK(fread(line, 1, 10, f) == 4 && memcmp(line, "tail", 4) == 0, "fseek/fread");
    CHECK(fread(line, 1, 1, f) == 0 && feof(f) && !ferror(f), "feof after end");
    clearerr(f);
    CHECK(!feof(f), "clearerr");
    rewind(f);
    CHECK(ftell(f) == 0 && fgetc(f) == 'l', "rewind/ftell/fgetc");
    fclose(f);

    DIR* d = opendir(sub);
    int found = 0; struct dirent* e;
    while (d && (e = readdir(d))) if (strcmp(e->d_name, "data.txt") == 0) { found = 1; n = e->d_type; }
    if (d) closedir(d);
    CHECK(found && n == DT_REG, "opendir/readdir (bionic dirent)");

    CHECK(unlink(path) == 0 && access(path, F_OK) != 0, "unlink/access");
    CHECK(rmdir(sub) == 0, "rmdir");
})
