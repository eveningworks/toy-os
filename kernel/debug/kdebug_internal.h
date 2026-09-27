#ifndef KDEBUG_INTERNAL_H
#define KDEBUG_INTERNAL_H

// State shared by the debugger's core (kdebug.c), its GDB protocol
// (gdbstub.c) and its KTESTs. Nothing outside kernel/debug/ includes it.

#include <stdint.h>
#include "kdebug_arch.h"

// WHERE THE BYTES GO. Serial today; the network transport planned in
// docs/kdebug-design.md is a second one, and the KTEST's script a third.
// getc() never blocks: -1 means nothing yet.
struct kdb_transport {
    int  (*getc)(void);
    void (*putc)(char c);
};

// GDB's own signal numbers, which are not Linux's past 9.
#define KDB_SIGINT  2
#define KDB_SIGILL  4
#define KDB_SIGTRAP 5
#define KDB_SIGABRT 6
#define KDB_SIGFPE  8
#define KDB_SIGBUS  10
#define KDB_SIGSEGV 11

enum kdb_resume { KDB_CONTINUE, KDB_STEP, KDB_DETACH };

// Software breakpoints are PATCHED ONLY WHILE THE KERNEL RUNS: lifted on
// every stop, written back on every resume. So memory reads while stopped
// show the real bytes, and the debugger's own code -- the serial poll a
// breakpoint could land in -- never meets one while it is talking.
#define KDB_SWBP_MAX 32
struct kdb_swbp {
    uint64_t addr;
    uint8_t  saved, used, patched;
};

struct kdb_state {
    int armed;                  // `kdebug=` claimed a port at boot
    int active;                 // stopped, talking to the debugger
    int connected;              // a debugger has spoken since the last detach
    int stepping;               // a single step is outstanding
    int fatal_seen;             // already stopped for this panic
    int pending_sig;            // what a compiled-in int3 reports, or 0
    int pushback;               // a byte read ahead by the poll, or -1
    const struct kdb_transport *io;
    uint64_t *regs;             // the stopped frame
    int sig;                    // why it stopped
    char watch_kind;            // 'w'/'a' when a watchpoint fired, else 0
    uint64_t watch_addr;
    uint32_t stops;
    struct kdb_swbp sw[KDB_SWBP_MAX];
    struct kdb_hw hw[KDB_HW_SLOTS];
};
extern struct kdb_state kdb;

// kdebug.c. insert: 1 done, 0 refused, -1 a type this CPU cannot do.
int  kdb_bp_insert(int type, uint64_t addr, int len);
int  kdb_bp_remove(int type, uint64_t addr, int len);
void kdb_bp_clear_all(void);

// gdbstub.c: talk until the debugger resumes the machine.
enum kdb_resume kdb_gdb_session(void);
uint8_t kdb_checksum(const char *s, int n);
int     kdb_hexval(int c);

#endif
