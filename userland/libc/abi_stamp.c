// What the SHARED LIBRARY side of the ABI contract says it is.
//
// One record, in libc.so, read by /lib/ld-toy.so before it relocates
// anything (abi/toyabi.h says why). libuapp.so does not carry its own:
// there is one userland here, built and shipped together, and two
// version numbers that can only ever agree is a second thing to keep
// true for no benefit.
//
// It lands in the static libc.a too, which is harmless -- a static
// program never runs the loader, and nothing else reads this symbol.
#include "toyabi.h"

const struct toy_abi_stamp __toy_abi_provided = {
    TOY_ABI_MAGIC, TOY_ABI_VERSION,
};
