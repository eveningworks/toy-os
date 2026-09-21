// The dlopen proof plugin: /lib/libplug.so.
//
// DELIBERATELY LINKED BY NOTHING. libhello.so is a DT_NEEDED of
// dyn_test, so dlopen()ing it would take the already-loaded path and
// prove only that the table lookup works. This one is reached at
// RUNTIME or not at all, which is what the plugin case actually is.
#include <stdint.h>
#include <string.h>

// Calls into libc.so, so the plugin has a DT_NEEDED of its own that
// dlopen has to satisfy -- the half a bare "load one file" would miss.
int plug_len(const char *s) { return (int)strlen(s); }

int plug_answer(void) { return 42; }

// A data symbol, fetched with dlsym() and read through the handle.
int plug_counter = 7;
