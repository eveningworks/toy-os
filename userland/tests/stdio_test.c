// The C library's stream layer: FILE, buffering, the printf family,
// and exit() flushing on the way out (docs/libc-design.md, Stage 2).
//
// THE CHECKS THAT ARE HARD TO FAKE, and why each is shaped the way it
// is:
//
//  - **Buffering is proved by what is NOT on disk yet.** Writing to a
//    fully buffered stream and then reading the file through an
//    INDEPENDENT fd must find it still empty. "The bytes arrived
//    eventually" is satisfied by a stdio with no buffering at all, so
//    it measures nothing; the empty read is what only a buffered
//    implementation can pass.
//  - **printf is checked past the formatter's old fixed buffer.**
//    KFMT_LINE_MAX is 256 and BUFSIZ is 1024, so a line longer than
//    both is what distinguishes the sink form from the
//    format-into-a-scratch-buffer shape it replaced.
//  - **ftell is checked ACROSS a buffer boundary.** A read stream's
//    logical position is the fd's minus what is sitting unread in the
//    buffer; an implementation that forgets that is correct for the
//    first BUFSIZ bytes of every file and wrong after, which is
//    invisible in any small test.
//  - **atexit and the exit-time flush are proved from the CONSOLE.**
//    The handlers print without a newline, so the text can only appear
//    if exit() flushed an unterminated line -- and the ORDER proves
//    LIFO. Both are properties of what happens after main() returns,
//    which no assertion inside main() can reach.
//
// Prints one line per check and exits with the number of failures.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "syscall_abi.h"

#define PATH "/tmp/stdio_test.txt"
#define BIG  "/tmp/stdio_big.txt"

static int g_fail;

static void put(const char *s) { sys_write(1, s, strlen(s)); }

static void check(int ok, const char *what) {
    put(ok ? "  ok   " : "  FAIL ");
    put(what);
    put("\n");
    if (!ok) g_fail++;
}

// Reads a file through a RAW fd, never through the stream being tested.
static int slurp(const char *path, char *out, int cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    int n = 0;
    while (n < cap - 1) {
        int64_t r = sys_read(fd, out + n, (uint64_t)(cap - 1 - n));
        if (r <= 0) break;
        n += (int)r;
    }
    sys_close(fd);
    out[n] = '\0';
    return n;
}

static void say_A(void) { fputs("A", stdout); }
static void say_B(void) { fputs("B", stdout); }

int main(void) {
    put("stdio_test: FILE, buffering, printf and exit\n");

    // STATIC, not a local: a 4 KiB frame trips USERLAND_CFLAGS's
    // -Wframe-larger-than=2048, which exists because a ring-3 stack has
    // one 4 KiB guard page and a frame that big can step over it.
    static char rb[4096];

    // --- fopen refuses what it cannot serve --------------------------
    check(fopen(PATH, "r+") == 0, "fopen refuses \"r+\" rather than degrading");
    check(fopen(PATH, "q") == 0, "and an unknown mode");
    check(fopen("/definitely/not/here", "r") == 0, "and a missing file");

    // --- a write stream buffers --------------------------------------
    sys_unlink(PATH);
    FILE *f = fopen(PATH, "w");
    check(f != 0, "fopen(\"w\") succeeds");
    check(fileno(f) >= 3, "and hands out a real descriptor");

    fputs("hello", f);
    // THE LOAD-BEARING CHECK: a stdio with no buffering passes every
    // other check in this file and fails this one.
    check(slurp(PATH, rb, sizeof rb) == 0, "nothing reached the file yet");
    check(fflush(f) == 0, "fflush succeeds");
    check(slurp(PATH, rb, sizeof rb) == 5 && strcmp(rb, "hello") == 0,
          "and after the flush the bytes are there");

    // --- printf, past every fixed buffer in the old design -----------
    for (int i = 0; i < 40; i++) fprintf(f, "%03d-", i); // 160 bytes
    fprintf(f, "\n");
    fflush(f);
    int n = slurp(PATH, rb, sizeof rb);
    check(n == 5 + 40 * 4 + 1, "fprintf wrote every conversion");
    check(strncmp(rb + 5, "000-001-002-", 12) == 0, "in order and correctly formatted");
    check(fclose(f) == 0, "fclose succeeds");

    // A single printf longer than KFMT_LINE_MAX (256) AND than BUFSIZ
    // (1024): the sink form has no line limit, the scratch-buffer shape
    // it replaced would have stopped at 256.
    f = fopen(BIG, "w");
    check(f != 0, "reopened for the long line");
    const char *s64 = "0123456789abcdef0123456789abcdef"
                      "0123456789abcdef0123456789abcdef";
    fprintf(f, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
            s64, s64, s64, s64, s64, s64, s64, s64, s64, s64,
            s64, s64, s64, s64, s64, s64, s64, s64, s64, s64);
    fclose(f);
    n = slurp(BIG, rb, sizeof rb);
    check(n == 1280, "a single printf of 1280 bytes is not truncated");
    check(n > 256, "which is past the formatter's old fixed buffer");
    check(rb[0] == '0' && rb[1279] == 'f', "and both ends are intact");

    // --- setvbuf is the escape hatch ---------------------------------
    sys_unlink(PATH);
    f = fopen(PATH, "w");
    check(setvbuf(f, 0, _IONBF, 0) == 0, "setvbuf(_IONBF) is accepted");
    check(setvbuf(f, rb, _IOFBF, 64) == -1, "a caller-supplied buffer is refused");
    fputs("now", f);
    // The mirror of the buffering check above, and its control: the
    // same sequence that left the file empty must now reach it at once.
    check(slurp(PATH, rb, sizeof rb) == 3, "an unbuffered stream writes through");
    fclose(f);

    // --- reading, ungetc, and fgets ----------------------------------
    sys_unlink(PATH);
    f = fopen(PATH, "w");
    fputs("one\ntwo\nthree", f);
    fclose(f);

    f = fopen(PATH, "r");
    check(f != 0, "fopen(\"r\")");
    check(fgetc(f) == 'o', "fgetc reads the first byte");
    check(ungetc('X', f) == 'X', "ungetc accepts a byte");
    check(fgetc(f) == 'X', "which comes back next");
    check(ungetc('Y', f) == 'Y' && ungetc('Z', f) == EOF,
          "and a SECOND pushback is refused, as C allows");
    check(fgetc(f) == 'Y', "the one pushback still works");

    char line[32];
    check(fgets(line, sizeof line, f) != 0 && strcmp(line, "ne\n") == 0,
          "fgets stops after the newline");
    check(fgets(line, sizeof line, f) != 0 && strcmp(line, "two\n") == 0,
          "and the next line is whole");
    check(fgets(line, sizeof line, f) != 0 && strcmp(line, "three") == 0,
          "a last line with no newline still arrives");
    check(fgets(line, sizeof line, f) == 0, "and then EOF");
    check(feof(f), "feof says so");
    fclose(f);

    // --- position, across a buffer boundary --------------------------
    //
    // 3000 bytes: more than BUFSIZ (1024), so the stream refills twice
    // and ftell has to account for what is still sitting unread.
    sys_unlink(BIG);
    f = fopen(BIG, "w");
    for (int i = 0; i < 3000; i++) fputc('A' + (i % 26), f);
    check(fclose(f) == 0, "wrote a 3000-byte fixture");

    f = fopen(BIG, "r");
    check(ftell(f) == 0, "a fresh read stream is at 0");
    for (int i = 0; i < 1500; i++) fgetc(f);
    // If ftell reported the fd's position it would say 2048 here.
    check(ftell(f) == 1500, "ftell is the LOGICAL position, not the fd's");
    check(fgetc(f) == 'A' + (1500 % 26), "and the next byte is the right one");

    check(fseek(f, 2000, SEEK_SET) == 0, "fseek SEEK_SET");
    check(ftell(f) == 2000, "reports the new position");
    check(fgetc(f) == 'A' + (2000 % 26), "and reads byte 2000");
    check(fseek(f, 99, SEEK_CUR) == 0 && ftell(f) == 2100,
          "SEEK_CUR is relative to the LOGICAL position");
    check(fgetc(f) == 'A' + (2100 % 26), "and reads byte 2100");
    check(fseek(f, -1, SEEK_END) == 0 && fgetc(f) == 'A' + (2999 % 26),
          "SEEK_END reaches the last byte");
    rewind(f);
    check(ftell(f) == 0 && fgetc(f) == 'A', "rewind goes back to the start");

    // fread's direct path: a block larger than the buffer.
    rewind(f);
    static char big[2048];
    check(fread(big, 1, sizeof big, f) == sizeof big, "fread of 2048 bytes");
    check(big[0] == 'A' && big[2047] == 'A' + (2047 % 26), "with the right contents");
    fclose(f);

    // --- an unseekable stream says so --------------------------------
    check(fseek(stdout, 0, SEEK_SET) == -1 && sys_errno() == ESPIPE,
          "seeking a terminal stream is refused");

    sys_unlink(PATH);
    sys_unlink(BIG);

    if (g_fail) printf("stdio_test: %d FAILURES\n", g_fail);
    else        printf("stdio_test: all checks passed\n");

    // Registered A then B, so LIFO runs B first and the console must
    // show "atexit:BA". Neither handler ends its output with a newline,
    // so the text can only appear at all if exit() flushed.
    atexit(say_A);
    atexit(say_B);
    printf("atexit:");
    return g_fail;
}
