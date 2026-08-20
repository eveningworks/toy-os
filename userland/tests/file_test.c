// A freestanding userland test program that exercises the new fd-aware
// file I/O syscalls (SYS_OPEN/SYS_READ/SYS_CLOSE, plus the now-fd-aware
// SYS_WRITE -- see syscall_abi.h) against the in-memory filesystem
// (fs.c). Opens a file for writing (creating it), writes a fixed
// string, closes it, reopens the SAME file for reading, reads it back,
// and writes what it read to stdout (fd 1) so the round-trip is visible
// on screen -- then exits 0 if the read-back bytes matched what was
// written, or 1 if anything about the round-trip didn't check out
// (open/read/write failed, or the bytes differ). This is the first
// program in this project to touch a real named file from ring 3 --
// every earlier test only ever wrote to the console.
#include <stdint.h>
#include "rt/sys.h"















static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

#define READ_CAP 128

int main(void) {
    const char *path = "filetest.txt";
    const char *content = "hello from a real file, round-tripped through fs.c\n";
    uint64_t content_len = my_strlen(content);

    // Write phase: open for writing, creating it if it's not there yet
    // and truncating any leftover content from a previous run.
    int64_t wfd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (wfd < 0) {
        const char *msg = "filetest: open(write) failed\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }
    int64_t wrote = sys_write((int)wfd, content, content_len);
    if (wrote != (int64_t)content_len) {
        const char *msg = "filetest: write() wrote the wrong number of bytes\n";
        sys_write(1, msg, my_strlen(msg));
        sys_close((int)wfd);
        sys_exit(1);
    }
    sys_close((int)wfd);

    // Read phase: reopen the same file for reading (default: read-only,
    // no flags needed) and read it back.
    int64_t rfd = sys_open(path, 0);
    if (rfd < 0) {
        const char *msg = "filetest: open(read) failed\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }
    char buf[READ_CAP];
    int64_t got = sys_read((int)rfd, buf, READ_CAP);
    sys_close((int)rfd);
    if (got < 0) {
        const char *msg = "filetest: read() failed\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }

    {
        const char *prefix = "filetest: read back: ";
        sys_write(1, prefix, my_strlen(prefix));
    }
    sys_write(1, buf, (uint64_t)got);

    // Verify the round trip actually matched byte-for-byte.
    int ok = (got == (int64_t)content_len);
    for (int64_t i = 0; ok && i < got; i++) {
        if (buf[i] != content[i]) ok = 0;
    }

    // --- A BINARY ROUND TRIP, WITH ZERO BYTES IN THE MIDDLE ----------
    //
    // THE REGRESSION THIS EXISTS FOR: sys_write() to a file used to
    // call fs_write(), which takes a NUL-TERMINATED C STRING -- so a
    // write containing a zero byte stopped there, stored only the
    // prefix, AND RETURNED THE FULL LENGTH as though all of it had
    // landed. Every caller checked the return value; every one of them
    // was told it had succeeded. Text files were unaffected, which is
    // why it survived: nothing in this OS wrote binary through an fd
    // until /bin/mkfiles did.
    //
    // The check that matters is the SIZE READ BACK, not the write's
    // return value -- the old code returned the right number and stored
    // the wrong bytes, so believing the return value is precisely the
    // mistake.
    if (ok) {
        const char *bpath = "filetest.bin";
        char bin[8];
        bin[0] = 'A'; bin[1] = 0; bin[2] = 'B'; bin[3] = 0;
        bin[4] = 0;   bin[5] = 'C'; bin[6] = (char)0xFF; bin[7] = 'D';

        int64_t bfd = sys_open(bpath, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
        if (bfd < 0) { ok = 0; }
        else {
            int64_t bw = sys_write((int)bfd, bin, sizeof bin);
            sys_close((int)bfd);
            if (bw != (int64_t)sizeof bin) ok = 0;
        }
        if (ok) {
            int64_t rfd = sys_open(bpath, 0);
            if (rfd < 0) { ok = 0; }
            else {
                char back[16];
                int64_t bg = sys_read((int)rfd, back, sizeof back);
                sys_close((int)rfd);
                // Length first: the old bug produced 1 byte here (it
                // stopped at bin[1]), which is the difference this
                // check is really about.
                if (bg != (int64_t)sizeof bin) ok = 0;
                for (int64_t i = 0; ok && i < bg; i++) {
                    if (back[i] != bin[i]) ok = 0;
                }
            }
        }
        const char *bmsg = ok ? "filetest: binary round trip OK\n"
                              : "filetest: binary round trip MISMATCH (zero bytes truncated?)\n";
        sys_write(1, bmsg, my_strlen(bmsg));
    }

    if (ok) {
        const char *msg = "filetest: round trip OK\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(0);
    } else {
        const char *msg = "filetest: round trip MISMATCH\n";
        sys_write(1, msg, my_strlen(msg));
        sys_exit(1);
    }
}
