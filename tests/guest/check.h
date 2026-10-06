/* Tiny test harness for guest libraries: results go to stdout, which the
 * THUMB harness forwards. tools/run-tests.sh parses the RESULT line. */
#pragma once
#include <jni.h>
#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, name) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)
#define TEST_MAIN(...) \
    JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* r) { (void)vm; (void)r; __VA_ARGS__; printf("RESULT pass=%d fail=%d\n", g_pass, g_fail); fflush(stdout); return JNI_VERSION_1_6; }
