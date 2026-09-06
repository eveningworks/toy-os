// tolibc as a SHARED LIBRARY: this binary links /lib/libc.so and no
// libc.a (the Makefile's dynlibc_test rule). Every check drives a real
// libc path through the PLT -- stdio through the FILE layer, malloc
// through the shared allocator, errno through the EXE's
// __errno_location (libc.so imports it back from us, which is the
// whole cross-module errno design).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "tmppath.h"
#include "lib/utmppath.h"

static int g_fail;
static FILE *g_out;

static void check(int ok, const char *what) {
    // Through libc.so's own fprintf -- the report is itself a check.
    fprintf(g_out, "  %s %s\n", ok ? "ok  " : "FAIL", what);
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    g_out = fopen(utest_path(TMP_VOLATILE, "dynlibc_test.out"), "w");
    if (!g_out) return 2;
    fprintf(g_out, "dynlibc_test: tolibc via /lib/libc.so\n");
    printf("dynlibc_test: tolibc via /lib/libc.so\n");

    // malloc/free -- heap_core.o inside libc.so, sbrk through the
    // exe's exported sys_sbrk.
    char *m = malloc(4096);
    check(m != NULL, "malloc in the shared library");
    if (m) { memset(m, 0x5A, 4096); check(m[4095] == 0x5A, "...and the memory is real"); free(m); }
    else check(0, "...and the memory is real");

    // snprintf incl. a float conversion (printf_float.o).
    char buf[64];
    snprintf(buf, sizeof buf, "%d %.3f %s", 42, 1.0 / 3.0, "x");
    check(strcmp(buf, "42 0.333 x") == 0, "snprintf with %d %.3f %s");

    // The FILE layer round trip: write, reopen, read back.
    FILE *f = fopen(utest_path(TMP_VOLATILE, "dynlibc_rt.txt"), "w");
    check(f != NULL, "fopen for write");
    if (f) { fputs("line one\nline two\n", f); fclose(f); }
    f = fopen(utest_path(TMP_VOLATILE, "dynlibc_rt.txt"), "r");
    if (f) {
        char l1[32] = {0}, l2[32] = {0};
        fgets(l1, sizeof l1, f);
        fgets(l2, sizeof l2, f);
        fclose(f);
        check(strcmp(l1, "line one\n") == 0 && strcmp(l2, "line two\n") == 0,
              "fgets reads back both lines");
    } else check(0, "fgets reads back both lines");
    remove(utest_path(TMP_VOLATILE, "dynlibc_rt.txt"));

    // errno crosses the module boundary: set by libc.so's fopen, read
    // through the same __errno_location the exe exported to it.
    errno = 0;
    check(fopen("/no/such/file", "r") == NULL && errno == ENOENT,
          "errno crosses the exe/libc.so boundary");

    // qsort takes a callback INTO this executable from library code.
    int v[7] = { 5, 1, 4, 7, 2, 6, 3 };
    qsort(v, 7, sizeof v[0], cmp_int);
    int sorted = 1;
    for (int i = 0; i < 7; i++) if (v[i] != i + 1) sorted = 0;
    check(sorted, "qsort calls back into the executable");

    if (g_fail) {
        fprintf(g_out, "dynlibc_test: %d FAILED\n", g_fail);
        printf("dynlibc_test: %d FAILED\n", g_fail);
    } else {
        fprintf(g_out, "dynlibc_test: all checks passed\n");
        printf("dynlibc_test: all checks passed\n");
    }
    fclose(g_out);
    return g_fail;
}
