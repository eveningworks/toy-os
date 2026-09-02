#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

#include <stdint.h>

// A RING-3 CRASH REPORT: written by the fault handler on the way to
// killing a process, into /var/crash/<program>-<pid>.crash. A text
// header (program, pid, fault, registers, memory map, the tail of the
// kernel log) followed by the raw bytes of the user stack from RSP to
// its top, so tools/panic_resolve.py --crash can name every return
// address against the ELF's DWARF -- the scan the kernel refuses to do
// on a user stack. macOS's crash report plus Windows' minidump, in one
// file.
//
// THIS IS ONLY EVER A PROCESS THAT FAULTED; a kernel panic writes
// nothing here (docs/roadmap.md's crash-reporting milestone keeps the
// two apart on purpose, and a kernel panic's record is a RAM store
// recovered on the next boot, not built yet).
//
// Refuses -- and says why in the kernel log -- when any filesystem
// operation is in flight (scheduler_preempt_depth() != 0): the fault
// may be inside a copy helper under an FS_OP, and writing a file from
// there re-enters the backend's scratch state. Same rule as the mmap
// fault-in.
//
// `regs` is isr_common's frame: regs[0..14] r15..rax, [15] vector,
// [16] error code, [17] RIP, [18] CS, [19] RFLAGS, [20] RSP, [21] SS.
// Returns the byte count written, or 0 with the reason logged.
uint32_t crash_report_write(const char *what, const uint64_t *regs, uint64_t cr2,
                            int stack_overflow);

#endif
