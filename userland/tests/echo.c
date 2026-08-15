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
#include "rt/sys.h"

// Own copy of kernel/include/api/keyboard.h's IS_PRINTABLE_KEY() -- this
// file is a freestanding ring-3 userland program built against no
// kernel headers at all (see syscall_abi.h being the only include
// above), so it can't share that macro directly. Keep the codepoint
// list in sync with keyboard.h's CHAR_*/IS_NORDIC_CHAR() if it ever
// changes. See docs/decisions.md's Nordic-keyboard entry.
#define ECHO_IS_NORDIC_CHAR(k) ((k) == 0xC4 || (k) == 0xD6 || (k) == 0xC5 || \
                                 (k) == 0xE4 || (k) == 0xF6 || (k) == 0xE5)
#define ECHO_IS_PRINTABLE_KEY(k) (((k) >= 32 && (k) < 127) || ECHO_IS_NORDIC_CHAR(k))













static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

#define LINE_CAP 256

int main(void) {
    const char *banner =
        "echo: type to see it echoed back by this ring-3 process itself\n"
        "(via SYS_READ_KEY + SYS_WRITE). Backspace works. Esc quits.\n\n";
    sys_write(1, banner, my_strlen(banner));

    char *line = (char *)sys_sbrk(LINE_CAP);
    if ((int64_t)(uintptr_t)line == -1) {
        const char *err = "echo: sbrk() failed -- no heap armed, or out of memory\n";
        sys_write(1, err, my_strlen(err));
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
