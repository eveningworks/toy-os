// The Stage-2 proof library: one function of every shape the loader
// must get right. Built -fpic into /lib/libhello.so (see the Makefile's
// dynlib rules); userland/tests/dyn_test.c is the caller.
#include <stdint.h>

// A data symbol the exe reads through a GLOB_DAT/GOT indirection.
int hello_dl_counter = 41;

// A lib-LOCAL pointer table -- forces R_X86_64_RELATIVE relocations
// inside the library itself.
static int add_one(int x) { return x + 1; }
static int add_two(int x) { return x + 2; }
static int (*const g_ops[2])(int) = { add_one, add_two };

// The exe defines dyn_test_callback() and exports it (--export-dynamic)
// -- calling it proves a library resolves symbols AGAINST THE
// EXECUTABLE, which is what a shared libc calling the program's
// __errno_location will need.
int dyn_test_callback(int x);

int hello_dl_add(int a, int b) { return a + b; }

int hello_dl_via_table(int which, int x) { return g_ops[which & 1](x); }

int hello_dl_callback(int x) { return dyn_test_callback(x) * 2; }
