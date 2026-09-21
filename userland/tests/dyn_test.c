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
#include <dlfcn.h>
#include <stdio.h>
#include <errno.h>
#include "syscall_abi.h"

int hello_dl_add(int a, int b);
int hello_dl_via_table(int which, int x);
int hello_dl_callback(int x);
extern int hello_dl_counter;

// What the LIBRARY calls back into -- must survive --export-dynamic.
int dyn_test_callback(int x) { return x + 100; }

#include "lib/utest.h"

int main(void) {
    utest_begin("dyn_test", "dynamic linking through /lib/ld-toy.so", UTEST_VERDICT_FILE);

    utest_check(hello_dl_add(2, 40) == 42, "exe calls a library function (PLT)");
    utest_check(hello_dl_counter == 41, "exe reads a library global (GOT)");
    hello_dl_counter++;
    utest_check(hello_dl_counter == 42, "...and writes it");
    utest_check(hello_dl_via_table(0, 10) == 11 && hello_dl_via_table(1, 10) == 12,
          "library-internal pointer table (RELATIVE relocs)");
    utest_check(hello_dl_callback(5) == 210,
          "library calls back into the executable");

    errno = 0;
    void *bad = sys_mmap((void *)0, 0, 1, 0x22, -1, 0);
    utest_check(bad == (void *)-1 && sys_errno() == EINVAL,
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
        utest_check(found, "pmap shows /lib/libhello.so mapped");
    }

    // --- dlopen (dynlink stage 4) -------------------------------------
    //
    // /lib/libplug.so is linked by NOTHING, so every check below fails
    // if runtime loading does not work -- the point DT_NEEDED cannot
    // make.
    {
        void *h = dlopen("/lib/libplug.so", RTLD_NOW);
        utest_check(h != NULL, "dlopen() loaded a library nothing links against");
        if (h) {
            int (*answer)(void) = dlsym(h, "plug_answer");
            utest_check(answer && answer() == 42, "dlsym() found a function");

            // The plugin's OWN DT_NEEDED had to be loaded and its
            // relocations applied, or this calls through a null GOT.
            int (*plen)(const char *) = dlsym(h, "plug_len");
            utest_check(plen && plen("abcd") == 4,
                        "the plugin's own DT_NEEDED (libc.so) was resolved");

            int *counter = dlsym(h, "plug_counter");
            utest_check(counter && *counter == 7, "dlsym() found a data symbol");

            utest_check(dlsym(h, "plug_no_such_symbol") == NULL,
                        "dlsym() answers NULL for a symbol that is not there");
            utest_check(dlclose(h) == 0, "dlclose() reports success");

            // The SAME object, not a second copy mapped over itself.
            utest_check(dlopen("/lib/libplug.so", RTLD_NOW) == h,
                        "dlopen() of an already-loaded object is the same handle");
        }
        utest_check(dlopen("/lib/libnope.so", RTLD_NOW) == NULL,
                    "dlopen() of a missing file answers NULL, and does not die");
        utest_check(dlerror() != NULL, "...and dlerror() says why");
    }

    return utest_end();
}
