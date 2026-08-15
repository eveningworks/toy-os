// Runs off the bottom of the user stack ON PURPOSE, to prove the kernel
// names the resulting fault "Stack overflow" rather than reporting a
// bare page fault at an address that points at nothing.
//
// EXCLUDED from tools/usertest_run.py, and it must stay that way: it
// faults deliberately, so it has no exit code and no output of its own
// to assert on. The assertion is on the KERNEL's report --
//
//     python3 tools/vm.py run "run stackovf_test" "dmesg"
//
// must show `RING-3 CRASH: Stack overflow`, and the console must show
// the range line beside it. A kernel without the guard classifier says
// `Page fault` there instead, which is the positive control: the fault
// happens either way, so a test that only checks "it crashed" proves
// nothing about this feature.
//
// Recursion, not one huge frame, is the right shape: it walks DOWN a
// page at a time and so cannot skip over the guard region the way a
// single oversized frame could (see uaddr.h's UADDR_GUARD_PAGES note --
// that hole is real, and this test would hide it if it jumped).
#include "rt/sys.h"

// **The recursion has to be invisible to the optimizer, and making the
// frame `volatile` is not enough.** Written the obvious way --
// `return frame[0] + burn(depth + 1);` -- GCC's accumulator variant of
// tail-recursion elimination rewrites it into a LOOP with one reused
// frame at -O2: the binary spins forever, never touches the stack, and
// the test hangs looking exactly like the guard not working. (It did;
// `objdump -d` on this file is what settled it, not reading the C.)
//
// Two things stop it, and both are needed. The call goes through a
// `volatile` function pointer, so the compiler cannot see that this
// function calls itself at all; and `frame` is read AFTER the call
// returns, so it must survive the call in a real stack frame rather
// than being folded into the caller's.
static unsigned long burn(unsigned long depth);
static unsigned long (*volatile burn_indirect)(unsigned long) = burn;

static unsigned long burn(unsigned long depth) {
    volatile unsigned long frame[64]; // 512 bytes, so 8 frames per page
    for (int i = 0; i < 64; i++) frame[i] = depth + (unsigned long)i;
    unsigned long deeper = burn_indirect(depth + 1);
    return frame[depth & 63] + deeper;
}

int main(void) {
    sys_print("stackovf_test: recursing until the stack runs out\n");
    sys_print("stackovf_test: expect the kernel to report a stack overflow\n");
    burn(1);
    // Unreachable: the recursion above always faults first. If this
    // line is ever printed, the stack grew instead of faulting and the
    // guard is gone.
    sys_print("stackovf_test: BUG -- returned instead of overflowing\n");
    return 1;
}
