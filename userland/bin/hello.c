// The smallest real userland program: greet, exit. No libc, no crt0 --
// just a freestanding ELF64 binary that makes two syscalls and returns
// a clean exit code. This is the one to read first when you want to see
// what a toy-os `/bin` binary minimally is.
//
// It used to be something else: the original proof that a separately
// compiled ELF could run in ring 3 at all, back before syscalls
// existed. With no way to print, it wrote a marker value to a fixed
// address the kernel would read back (USERLAND_MARKER_ADDR) and then
// executed `hlt` -- a privileged instruction -- so the resulting fault
// was how the kernel learned it had run. That worked because the old
// `elftest` command mapped a page at the marker address specially.
// The ELF64-to-`/bin` migration folded `elftest` into the generic `run
// hello` path (see docs/decisions.md), and that path maps no such page
// -- so `run hello` page-faulted on the marker write, one instruction
// before the `hlt` it was supposed to demonstrate. It looked like a
// crashing binary; it was a binary whose harness had been removed from
// under it.
//
// Nothing was lost by making it a real program instead of restoring
// that harness: `ring3test` still exercises the raw-`iretq` ring-3
// entry path, and `crash_test`/`nx_test` still cover deliberate faults
// and their recovery. What's gained is that the binary named `hello`
// now does what its name says.
#include <stdint.h>
#include "rt/sys.h"

// Same two inline syscall stubs every other userland program here
// carries its own copy of -- there's no libc and no shared userland
// runtime to put them in (see userland/write_test.c, which this
// mirrors).








static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

int main(void) {
    const char *msg = "Hello from a real ELF64 binary in ring 3!\n";
    sys_write(1, msg, my_strlen(msg));
    sys_exit(0);
}
