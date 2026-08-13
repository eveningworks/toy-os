#ifndef KTEST_RUN_H
#define KTEST_RUN_H

// The one piece of the in-kernel test harness that apps/ can see: "run
// the tests and tell me how many failed". The shell's `ktest` command
// is the only caller.
//
// Writing a test needs kernel/include/kernel/ktest.h (the KTEST macro,
// the assertions, the .ktests registration) and that header is
// deliberately NOT on apps/'s include path -- tests are kernel code,
// living next to the subsystem they exercise. This split is the
// convention CLAUDE.md describes in action: a capability an app needs
// gets a narrow function exposed through kapi.h, rather than the app
// reaching into kernel internals.

// Runs every registered test, or only those in `suite_filter` when it's
// non-NULL and non-empty. Prints its own report through vga_write(), so
// it lands wherever the caller's output goes -- physical console, GUI
// terminal window, or the serial debug console. Returns the number of
// FAILED tests: 0 means everything that ran passed.
int ktest_run_all(const char *suite_filter);

#endif
