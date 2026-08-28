// The first DYNAMIC executable: linked against /lib/libhello.so with
// PT_INTERP = /lib/ld-toy.so (see the Makefile's explicit dyn_test
// rule -- the one test with its own link line). Every check is a
// relocation class the loader must apply:
//   - exe -> lib call            (the exe's JUMP_SLOT/PLT)
//   - exe -> lib data            (GLOB_DAT through the GOT)
//   - lib-internal pointer table (R_X86_64_RELATIVE inside the lib)
//   - lib -> exe call            (the lib resolving against the exe,
//                                 which is what a shared libc needs)
// Plus: statically-linked tolibc still works in the same binary
// (printf/snprintf below), TLS still works (errno), and the region
// list shows the library as a FILE mapping.
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "syscall_abi.h"

int hello_dl_add(int a, int b);
int hello_dl_via_table(int which, int x);
int hello_dl_callback(int x);
extern int hello_dl_counter;

// What the LIBRARY calls back into -- must survive --export-dynamic.
int dyn_test_callback(int x) { return x + 100; }

static int g_fail;
static char g_log[2048];
static int g_len;

static void put(const char *s) {
    sys_write(1, s, strlen(s));
    int n = (int)strlen(s);
    if (g_len + n < (int)sizeof g_log) {
        memcpy(g_log + g_len, s, (size_t)n);
        g_len += n;
    }
}

static void flush_verdict(void) {
    int fd = sys_open("/tmp/dyn_test.out",
                      SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return;
    sys_write(fd, g_log, (uint64_t)g_len);
    sys_close(fd);
}

static void check(int ok, const char *what) {
    put(ok ? "  ok   " : "  FAIL ");
    put(what);
    put("\n");
    if (!ok) g_fail++;
}

int main(void) {
    put("dyn_test: dynamic linking through /lib/ld-toy.so\n");

    check(hello_dl_add(2, 40) == 42, "exe calls a library function (PLT)");
    check(hello_dl_counter == 41, "exe reads a library global (GOT)");
    hello_dl_counter++;
    check(hello_dl_counter == 42, "...and writes it");
    check(hello_dl_via_table(0, 10) == 11 && hello_dl_via_table(1, 10) == 12,
          "library-internal pointer table (RELATIVE relocs)");
    check(hello_dl_callback(5) == 210,
          "library calls back into the executable");

    errno = 0;
    void *bad = sys_mmap((void *)0, 0, 1, 0x22, -1, 0);
    check(bad == (void *)-1 && sys_errno() == EINVAL,
          "static tolibc + TLS errno still work in a dynamic binary");

    // The library shows up in this process's own map as a FILE region.
    {
        int me = sys_getpid(), found = 0;
        struct query_procmap q;
        for (unsigned i = 0; ; i++) {
            if (sys_query_record(QUERY_PROCMAP, i, &q, sizeof q) <
                (int)sizeof q) break;
            if ((int)q.pid == me && q.kind == QUERY_PROCMAP_FILE &&
                strcmp(q.path, "/lib/libhello.so") == 0)
                found = 1;
        }
        check(found, "pmap shows /lib/libhello.so mapped");
    }

    if (g_fail) {
        char m[48];
        snprintf(m, sizeof m, "dyn_test: %d FAILED\n", g_fail);
        put(m);
        flush_verdict();
        return g_fail;
    }
    put("dyn_test: all checks passed\n");
    flush_verdict();
    return 0;
}
