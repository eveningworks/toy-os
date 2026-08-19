// Reads fd 0 until EOF and writes it to fd 1.
//
// A FIXTURE, not a feature: `<` redirection needs a program that reads
// standard input, and until now nothing here did. tosh's `cat` takes a
// filename and opens it itself, so it could never demonstrate that fd 0
// had been pointed somewhere else.
//
// Deliberately not made a tosh builtin. `cat` with no argument reading
// stdin is correct Unix behaviour, but in the GUI Terminal fd 0 is the
// physical console, which that window does not own -- so a bare `cat`
// there would block the app on a keyboard it cannot see. A separate
// program is the honest shape until there is a real TTY per terminal.
#include <stdint.h>
#include "rt/sys.h"

int main(void) {
    char buf[256];
    for (;;) {
        int64_t n = sys_read(0, buf, sizeof buf);
        // 0 is EOF on a pipe or a file. A CONSOLE never returns 0 (see
        // SYS_READ's ABI note), so running this without `<` waits for
        // input rather than exiting -- which is what `cat` does too.
        if (n <= 0) break;
        sys_write(1, buf, (uint64_t)n);
    }
    return 0;
}
