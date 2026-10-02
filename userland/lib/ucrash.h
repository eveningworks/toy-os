#ifndef ULIB_UCRASH_H
#define ULIB_UCRASH_H

// ucrash -- a ring-3 crash report from /var/crash, read: its header, the
// kernel log it carries, and a BACKTRACE recovered from the saved stack.
// The Crash Reports window and `crashlog` both read reports through this.
//
// THE FORMAT is kernel/proc/crash_report.c's: a text header of `key:
// value` lines and `map:` lines, `klog:` and the log's tail, then
// `---- stack ----` and the raw stack from RSP's page up.
//
// **THE BACKTRACE IS FOUND BY SCANNING THE STACK**, Breakpad's fallback
// when there is no unwind information (and these executables carry
// none): each 8-byte word from RSP up that points into code, AND whose
// preceding bytes are a call instruction, is taken as a return address,
// and is named from that binary's own symbol table on disk. Words BELOW
// RSP are dead frames and never read. A frame can still be a stale
// return address left in a local -- the list is evidence, not proof --
// and a binary REPLACED since the crash names the wrong functions, which
// `stale` says.
#include <stdint.h>
#include <time.h>

#define UCRASH_MAPS   64
#define UCRASH_FRAMES 32
#define UCRASH_LOG    6144   // the kernel log's tail, as the report carries it

struct ucrash_map {
    uint64_t a, b;
    char kind[8];        // image, heap, stack, file, anon, mmio
    unsigned prot;       // 1 read, 2 write, 4 exec; 5 for the image
    char path[64];       // the file's, or the program's for the image
};

struct ucrash_frame {
    uint64_t addr;       // the runtime address
    uint64_t slot;       // where on the stack it was found; 0 for rip
    char module[40];     // the binary's file name
    char func[64];       // "" when no symbol covers it
    uint64_t off;        // into func, or into the module when unnamed
};

struct ucrash {
    char path[96], file[48];
    char program[64], fault[48], kernel[96];
    int pid;
    unsigned vector;
    uint64_t err, rip, rsp, cr2, rflags, cs, ss;
    uint64_t reg[15];                 // r15 .. rax, the kernel's order
    struct ucrash_map map[UCRASH_MAPS];
    int nmap;
    uint64_t stack_va;
    uint32_t stack_len;
    long stack_off;                   // where the stack bytes start in the file
    char log[UCRASH_LOG];
    time_t when;                      // the file's mtime
    unsigned size;

    // ucrash_backtrace()'s.
    struct ucrash_frame frame[UCRASH_FRAMES];
    int nframe;
    int stale;                        // a binary on disk is newer than the report
};

extern const char *const ucrash_reg_names[15];

// NOT REENTRANT: both calls work in static buffers (a backtrace's are
// kilobytes, past a ring-3 stack frame's budget). One thread at a time.

// The header and the log; no stack, no ELF files read. 0, or -1 for a
// file that is not a crash report.
int ucrash_load(struct ucrash *r, const char *path);

// Read the saved stack and the binaries it points into, and fill
// `frame` with rip and every return address found. The frame count.
int ucrash_backtrace(struct ucrash *r);

// "sys_futex_wait +0xa in notepad", "libuapp.so +0x59da9", or the map
// the address fell in. From frame 0 after a backtrace, else the maps.
void ucrash_where(const struct ucrash *r, char *out, int cap);

// The kernel log's lines about this process -- its name or its pid --
// into `out`, newline-separated. Returns how many there were.
int ucrash_log_lines(const struct ucrash *r, char *out, int cap);

// What happened, as a sentence a person reads: "It was killed by
// SIGSEGV, sent by pid 20.", "It tried to write to 0x18f, which is not
// mapped." -- a page fault's error bits decoded.
void ucrash_explain(const struct ucrash *r, char *out, int cap);

// "Killed by SIGSEGV" -> "pid 20", from the log's `kill(pid N, ...) by
// pid M` line; "" when nothing sent it.
void ucrash_sender(const struct ucrash *r, char *out, int cap);

#endif
