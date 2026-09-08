// lswin -- what the KERNEL thinks each client window is.
//
// **THE POINT IS THE PAIR.** `guictl windows` reports the COMPOSITOR's
// window list; this reports win_server.c's. A window-protocol bug is
// very often the two DISAGREEING rather than either being wrong alone,
// and until this existed only one of them could be read -- so a
// disagreement had to be inferred from behaviour, which is how three
// wrong hypotheses got written in one afternoon.
//
// PER BUFFER, because a buffer carries its own size (abi/win_proto.h): a
// resize rebuilds one of the two and leaves the other showing the last
// finished frame at the old size, so `w x h` differing from a buffer's
// own size is NORMAL mid-resize and is exactly what you want to see.
//
// `g` IS THE GENERATION -- which OBJECT is behind that buffer's name.
// The kernel does not hold a window's pixels any more, so this and the
// size are all it knows; the generation climbing on a drag is a resize
// working, and it climbing on a buffer the compositor never re-opens is
// the disagreement this pair exists to show.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "lib/cmd.h"

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { cmd_usage("lswin"); return 1; }

    printf("  PID  WIN     W    H  FRONT   BUF0 (w x h, gen)"
           "   BUF1 (w x h, gen)\n");
    struct query_window r;
    int n = 0;
    QUERY_FOREACH(QUERY_WINDOWS, r, i) {
        char b0[40], b1[40];
        if (r.buf_w[0])
            snprintf(b0, sizeof b0, "%dx%d g%u", r.buf_w[0], r.buf_h[0], r.buf_gen[0]);
        else
            snprintf(b0, sizeof b0, "-");
        if (r.buf_w[1])
            snprintf(b1, sizeof b1, "%dx%d g%u", r.buf_w[1], r.buf_h[1], r.buf_gen[1]);
        else
            snprintf(b1, sizeof b1, "-");

        printf("%5d %4u %5d %4d %6u   %-20s   %-20s\n",
               r.pid, r.id, r.w, r.h, r.front, b0, b1);
        n++;
    }
    if (!n) printf("(no client windows)\n");
    return 0;
}
