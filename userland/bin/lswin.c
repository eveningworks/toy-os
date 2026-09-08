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
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "query_abi.h"
#include "lib/cmd.h"

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { cmd_usage("lswin"); return 1; }

    printf("  PID  WIN     W    H  FRONT   BUF0 (w x h, pages, shm)"
           "   BUF1 (w x h, pages, shm)  COMP\n");

    struct query_window r;
    int n = 0;
    QUERY_FOREACH(QUERY_WINDOWS, r, i) {
        char b0[40], b1[40];
        if (r.buf_pages[0])
            snprintf(b0, sizeof b0, "%dx%d %up #%d",
                     r.buf_w[0], r.buf_h[0], r.buf_pages[0], r.buf_shm[0]);
        else
            snprintf(b0, sizeof b0, "-");
        if (r.buf_pages[1])
            snprintf(b1, sizeof b1, "%dx%d %up #%d",
                     r.buf_w[1], r.buf_h[1], r.buf_pages[1], r.buf_shm[1]);
        else
            snprintf(b1, sizeof b1, "-");

        // A RETIRED window is one whose client is gone while the
        // compositor still holds the slot -- worth showing rather than
        // hiding, since a slot that never gets released is exactly the
        // shape of a leak.
        printf("%5d %4u %5d %4d %6u   %-24s   %-24s  %s%s\n",
               r.pid, r.id, r.w, r.h, r.front, b0, b1,
               r.comp_mapped ? "mapped" : "-",
               r.comp_retired ? " retired" : "");
        n++;
    }
    if (!n) printf("(no client windows)\n");
    return 0;
}
