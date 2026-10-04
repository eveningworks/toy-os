// A freestanding userland program that reads the console one byte at a
// time through fd 0 -- a BLOCKING read, so it holds no CPU while it
// waits -- and echoes each byte back through SYS_WRITE, staying
// interactive until Esc. Its line buffer comes from sbrk() rather than
// .bss purely so the program demonstrates SYS_SBRK doing something.
#include <stdint.h>
#include "rt/sys.h"

// Own copy of kernel/include/api/keyboard.h's IS_PRINTABLE_KEY() -- this
// file is a freestanding ring-3 userland program built against no
// kernel headers at all (see syscall_abi.h being the only include
// above), so it can't share that macro directly: printable ASCII or
// Latin-1 0xA0-0xFF. See docs/decisions/drivers.md's Nordic-keyboard entry.
#define ECHO_IS_PRINTABLE_KEY(k) (((k) >= 32 && (k) < 127) || ((k) >= 0xA0 && (k) <= 0xFF))













static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

#define LINE_CAP 256

int main(void) {
    const char *banner =
        "echo: type to see it echoed back by this ring-3 process itself\n"
        "(via a blocking read of fd 0 + SYS_WRITE). Backspace works. Esc quits.\n\n";
    sys_write(1, banner, my_strlen(banner));

    char *line = (char *)sys_sbrk(LINE_CAP);
    if ((int64_t)(uintptr_t)line == -1) {
        const char *err = "echo: sbrk() failed -- no heap armed, or out of memory\n";
        sys_write(1, err, my_strlen(err));
        sys_exit(1);
    }
    uint64_t pos = 0;

    for (;;) {
        unsigned char byte;
        if (sys_read(0, &byte, 1) != 1) break; // the console has no EOF; anything else is a failure
        int64_t key = byte;

        if (key == 27) { // Esc
            const char *bye = "\n[echo: exiting]\n";
            sys_write(1, bye, my_strlen(bye));
            break;
        } else if (key == '\n') {
            sys_write(1, "\n", 1);
            pos = 0; // start the next line fresh
        } else if (key == '\b') {
            if (pos > 0) {
                pos--;
                // A literal '\b' byte through SYS_WRITE is special-cased
                // by vga_putc() (see vga.c) to call vga_backspace(),
                // which both moves the cursor back AND blanks that cell
                // itself -- no separate space+backspace dance needed.
                sys_write(1, "\b", 1);
            }
        } else if (ECHO_IS_PRINTABLE_KEY(key) && pos < LINE_CAP - 1) {
            line[pos++] = (char)key;
            char c = (char)key;
            sys_write(1, &c, 1);
        }
        // anything else (arrow keys, etc.) is silently ignored
    }

    sys_exit(0);
}
