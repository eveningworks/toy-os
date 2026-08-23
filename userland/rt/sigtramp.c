// The signal restorer: where a handler returns to.
//
// TWO INSTRUCTIONS, AND THE SECOND ONE IS THE POINT. The kernel pushes
// the address of `__sigrestore` as a handler's return address (see
// abi/signal_abi.h's `struct k_sigaction`), so when an ordinary C handler
// does an ordinary `ret`, it lands here -- and this asks the kernel to
// put the interrupted process back the way it was.
//
// **NOTHING HERE MAY TOUCH THE STACK, and that is a hard requirement
// rather than an optimisation.** The kernel finds the signal frame by
// arithmetic from RSP: the handler was entered with RSP pointing at the
// frame, its `ret` popped one word, so the frame starts at RSP-8 and
// signal_restore_frame() reads it from exactly there. A push, a call, or
// a compiler-inserted prologue would move RSP and the kernel would read
// eight bytes of something else.
//
// WHICH IS WHY IT IS `naked`, and why that attribute is doing real work
// rather than saving two instructions: a normal C function is entitled
// to a frame, and at -O2 this one would not have had one -- until the
// day somebody added a line to it, or a build gained -fno-omit-frame-
// pointer, at which point the kernel would silently read the wrong eight
// bytes. `naked` makes "no prologue" a property the compiler enforces
// instead of one that happens to hold.
//
// C RATHER THAN A .asm FILE, for one reason: SYS_SIGRETURN's number.
// An assembly restorer would have to write `67`, which is a number
// another file has to keep true -- the exact shape CLAUDE.md rules out.
// Here it comes from the header, so renumbering the syscall cannot leave
// this behind.
//
// This is x86-64 Linux's `__restore_rt` in glibc, doing the same job for
// the same reason. i386 Linux puts the equivalent in the vDSO instead;
// abi/signal_abi.h says why toy-os took this side. No CFI directives:
// glibc's carries a hand-written unwind table so a debugger can walk
// THROUGH a signal frame, and nothing here unwinds, so adding one would
// be a claim this tree cannot check.
#include "syscall_abi.h"

#define STR2(x) #x
#define STR(x) STR2(x)

__attribute__((naked, used)) void __sigrestore(void) {
    // `int $0x80` does not return -- the trapframe it returns through is
    // the interrupted one, not this one. `ud2` rather than a fallthrough
    // into whatever the linker put next: if it ever DOES return, the
    // kernel rejected the frame, and stopping here is more useful than
    // executing a neighbouring function's first instruction.
    __asm__ volatile (
        "mov $" STR(SYS_SIGRETURN) ", %eax\n\t"
        "int $0x80\n\t"
        "ud2"
    );
}
