// A freestanding userland program that exercises two syscalls no
// earlier test used: SYS_READ_KEY (keyboard input straight into ring 3)
// and SYS_SBRK (a per-process heap). It reads one character at a time
// and echoes it straight back via SYS_WRITE -- so unlike
// write_test.c/write_bad_test.c, which just prove a syscall works and
// then exit, this one stays interactive until you press Esc.
//
// SYS_READ_KEY is non-blocking (see syscall_abi.h for why), so this
// spins, calling it again whenever it gets -1, until a key shows up.
// That's a busy-wait, not a real blocking read -- fine for a demo
// program with nothing else to do, but the honest thing to note.
//
// The line buffer it types into is allocated with sbrk() rather than
// living in .bss, purely so this program actually demonstrates SYS_SBRK
// doing something -- a fixed-size static array would work just as well
// functionally.
#include <stdint.h>
#include "syscall_abi.h"

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int64_t sys_write(const char *buf, uint64_t len) {
    return syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)buf, len);
}

static inline int64_t sys_read_key(void) {
    return syscall2(SYS_READ_KEY, 0, 0);
}

static inline void *sys_sbrk(int64_t inc) {
    return (void *)(uintptr_t)syscall2(SYS_SBRK, (uint64_t)inc, 0);
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { } // unreachable
}

static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

#define LINE_CAP 256

void _start(void) {
    const char *banner =
        "echo: type to see it echoed back by this ring-3 process itself\n"
        "(via SYS_READ_KEY + SYS_WRITE). Backspace works. Esc quits.\n\n";
    sys_write(banner, my_strlen(banner));

    char *line = (char *)sys_sbrk(LINE_CAP);
    if ((int64_t)(uintptr_t)line == -1) {
        const char *err = "echo: sbrk() failed -- no heap armed, or out of memory\n";
        sys_write(err, my_strlen(err));
        sys_exit(1);
    }
    uint64_t pos = 0;

    for (;;) {
        int64_t key;
        do {
            key = sys_read_key();
        } while (key == -1);

        if (key == 27) { // Esc
            const char *bye = "\n[echo: exiting]\n";
            sys_write(bye, my_strlen(bye));
            break;
        } else if (key == '\n') {
            sys_write("\n", 1);
            pos = 0; // start the next line fresh
        } else if (key == '\b') {
            if (pos > 0) {
                pos--;
                // A literal '\b' byte through SYS_WRITE is special-cased
                // by vga_putc() (see vga.c) to call vga_backspace(),
                // which both moves the cursor back AND blanks that cell
                // itself -- no separate space+backspace dance needed.
                sys_write("\b", 1);
            }
        } else if (key >= 32 && key < 127 && pos < LINE_CAP - 1) {
            line[pos++] = (char)key;
            char c = (char)key;
            sys_write(&c, 1);
        }
        // anything else (arrow keys, etc.) is silently ignored
    }

    sys_exit(0);
}
