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
#include "tmppath.h"

static const char *p_path(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "stdio_test.txt");
    return p;
}
#define PATH p_path()
static const char *p_big(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "stdio_big.txt");
    return p;
}
#define BIG p_big()

#include "lib/utest.h"   // reports with sys_write(), not the stdio under test

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
    utest_begin("stdio_test", "FILE, buffering, printf and exit", 0);

    // STATIC, not a local: a 4 KiB frame trips USERLAND_CFLAGS's
    // -Wframe-larger-than=2048, which exists because a ring-3 stack has
    // one 4 KiB guard page and a frame that big can step over it.
    static char rb[4096];

    // --- fopen refuses what it cannot serve --------------------------
    utest_check(fopen(PATH, "r+") == 0, "fopen refuses \"r+\" rather than degrading");
    utest_check(fopen(PATH, "q") == 0, "and an unknown mode");
    utest_check(fopen("/definitely/not/here", "r") == 0, "and a missing file");

    // --- a write stream buffers --------------------------------------
    sys_unlink(PATH);
    FILE *f = fopen(PATH, "w");
    utest_check(f != 0, "fopen(\"w\") succeeds");
    utest_check(fileno(f) >= 3, "and hands out a real descriptor");

    fputs("hello", f);
    // THE LOAD-BEARING CHECK: a stdio with no buffering passes every
    // other check in this file and fails this one.
    utest_check(slurp(PATH, rb, sizeof rb) == 0, "nothing reached the file yet");
    utest_check(fflush(f) == 0, "fflush succeeds");
    utest_check(slurp(PATH, rb, sizeof rb) == 5 && strcmp(rb, "hello") == 0,
          "and after the flush the bytes are there");

    // --- printf, past every fixed buffer in the old design -----------
    for (int i = 0; i < 40; i++) fprintf(f, "%03d-", i); // 160 bytes
    fprintf(f, "\n");
    fflush(f);
    int n = slurp(PATH, rb, sizeof rb);
    utest_check(n == 5 + 40 * 4 + 1, "fprintf wrote every conversion");
    utest_check(strncmp(rb + 5, "000-001-002-", 12) == 0, "in order and correctly formatted");
    utest_check(fclose(f) == 0, "fclose succeeds");

    // A single printf past KFMT_LINE_MAX (256): the sink form has no
    // line limit, the scratch-buffer shape it replaced stopped at 256.
    // Crossing a BUFSIZ boundary is the ftell fixture's job below, which
    // derives its size rather than naming one.
    f = fopen(BIG, "w");
    utest_check(f != 0, "reopened for the long line");
    const char *s64 = "0123456789abcdef0123456789abcdef"
                      "0123456789abcdef0123456789abcdef";
    fprintf(f, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
            s64, s64, s64, s64, s64, s64, s64, s64, s64, s64,
            s64, s64, s64, s64, s64, s64, s64, s64, s64, s64);
    fclose(f);
    n = slurp(BIG, rb, sizeof rb);
    utest_check(n == 1280, "a single printf of 1280 bytes is not truncated");
    utest_check(n > 256, "which is past the formatter's old fixed buffer");
    utest_check(rb[0] == '0' && rb[1279] == 'f', "and both ends are intact");

    // --- setvbuf is the escape hatch ---------------------------------
    sys_unlink(PATH);
    f = fopen(PATH, "w");
    utest_check(setvbuf(f, 0, _IONBF, 0) == 0, "setvbuf(_IONBF) is accepted");
    utest_check(setvbuf(f, rb, _IOFBF, 64) == -1, "a caller-supplied buffer is refused");
    fputs("now", f);
    // The mirror of the buffering check above, and its control: the
    // same sequence that left the file empty must now reach it at once.
    utest_check(slurp(PATH, rb, sizeof rb) == 3, "an unbuffered stream writes through");
    fclose(f);

    // --- reading, ungetc, and fgets ----------------------------------
    sys_unlink(PATH);
    f = fopen(PATH, "w");
    fputs("one\ntwo\nthree", f);
    fclose(f);

    f = fopen(PATH, "r");
    utest_check(f != 0, "fopen(\"r\")");
    utest_check(fgetc(f) == 'o', "fgetc reads the first byte");
    utest_check(ungetc('X', f) == 'X', "ungetc accepts a byte");
    utest_check(fgetc(f) == 'X', "which comes back next");
    // EIGHT deep, not one. C guarantees a single byte and permits more,
    // and scanf needs more -- deciding that "0x" does not begin a
    // number means putting several characters back. This asserted the
    // one-byte limit until scanf arrived and the limit changed; it is
    // the DOCUMENTED depth that is being checked, not an accident.
    utest_check(ungetc('Y', f) == 'Y' && ungetc('Z', f) == 'Z',
          "a second pushback is accepted -- the stack is eight deep");
    utest_check(fgetc(f) == 'Z' && fgetc(f) == 'Y',
          "and they come back in reverse order, as a stack");
    for (int i = 0; i < 8; i++) ungetc('a' + i, f);
    utest_check(ungetc('!', f) == EOF, "a NINTH is refused");
    for (int i = 0; i < 8; i++) fgetc(f);   // drain them again

    char line[32];
    utest_check(fgets(line, sizeof line, f) != 0 && strcmp(line, "ne\n") == 0,
          "fgets stops after the newline");
    utest_check(fgets(line, sizeof line, f) != 0 && strcmp(line, "two\n") == 0,
          "and the next line is whole");
    utest_check(fgets(line, sizeof line, f) != 0 && strcmp(line, "three") == 0,
          "a last line with no newline still arrives");
    utest_check(fgets(line, sizeof line, f) == 0, "and then EOF");
    utest_check(feof(f), "feof says so");
    fclose(f);

    // --- position, across a buffer boundary --------------------------
    //
    // SIZED FROM BUFSIZ, not written as a literal: the point is that the
    // stream refills several times, and at a fixed 3000 that quietly
    // stopped being true the moment BUFSIZ passed it.
    const int fix_n = 3 * BUFSIZ;
    const int mid   = fix_n / 2;      // past at least one refill
    sys_unlink(BIG);
    f = fopen(BIG, "w");
    for (int i = 0; i < fix_n; i++) fputc('A' + (i % 26), f);
    utest_check(fclose(f) == 0, "wrote a 3*BUFSIZ fixture");

    f = fopen(BIG, "r");
    utest_check(ftell(f) == 0, "a fresh read stream is at 0");
    for (int i = 0; i < mid; i++) fgetc(f);
    // If ftell reported the fd's position it would say a whole number of
    // buffers here, never mid.
    utest_check(ftell(f) == mid, "ftell is the LOGICAL position, not the fd's");
    utest_check(fgetc(f) == 'A' + (mid % 26), "and the next byte is the right one");

    // p2 is p1 + 100, not + 99: the fgetc below the SEEK_SET consumes a
    // byte, so SEEK_CUR starts from p1 + 1.
    const int p1 = 2 * BUFSIZ, p2 = p1 + 100;
    utest_check(fseek(f, p1, SEEK_SET) == 0, "fseek SEEK_SET");
    utest_check(ftell(f) == p1, "reports the new position");
    utest_check(fgetc(f) == 'A' + (p1 % 26), "and reads that byte");
    utest_check(fseek(f, 99, SEEK_CUR) == 0 && ftell(f) == p2,
          "SEEK_CUR is relative to the LOGICAL position");
    utest_check(fgetc(f) == 'A' + (p2 % 26), "and reads 99 further on");
    utest_check(fseek(f, -1, SEEK_END) == 0 && fgetc(f) == 'A' + ((fix_n - 1) % 26),
          "SEEK_END reaches the last byte");
    rewind(f);
    utest_check(ftell(f) == 0 && fgetc(f) == 'A', "rewind goes back to the start");

    // fread's direct path: a block larger than the buffer, which is what
    // sends it straight out instead of through f->buf -- so it is sized
    // from BUFSIZ too. Static, not on the stack: ring-3 frames are
    // budgeted at 2048 bytes.
    rewind(f);
    static char big[2 * BUFSIZ];
    utest_check(fread(big, 1, sizeof big, f) == sizeof big, "fread past the buffer");
    utest_check(big[0] == 'A' && big[sizeof big - 1] == 'A' + ((sizeof big - 1) % 26),
          "with the right contents");
    fclose(f);

    // --- sprintf and sscanf ------------------------------------------
    //
    // Both landed only when a PORTED program asked for them
    // (docs/libc-design.md Stage 6), so they get direct checks here --
    // cJSON exercises them, but only through paths where a wrong answer
    // can still produce valid output.
    sprintf(rb, "%s=%d/%x", "n", -5, 255);
    utest_check(strcmp(rb, "n=-5/ff") == 0, "sprintf writes an unbounded result");

    int a = 0, b = 0;
    utest_check(sscanf("12 34", "%d %d", &a, &b) == 2 && a == 12 && b == 34,
          "sscanf reads two integers");
    utest_check(sscanf("x=7", "x=%d", &a) == 1 && a == 7, "and matches literal text");
    utest_check(sscanf("y=7", "x=%d", &a) == 0, "and STOPS when the literal does not match");
    a = 0;
    utest_check(sscanf("0x1f", "%i", &a) == 1 && a == 31,
          "%i honours a 0x prefix, unlike %d");
    utest_check(sscanf("0x1f", "%d", &a) == 1 && a == 0,
          "and %d stops at the 'x', reading just the 0");
    double d = 0;
    utest_check(sscanf("3.25rest", "%lg", &d) == 1 && d == 3.25,
          "%lg reads a double -- the exact call cJSON makes");
    float fl = 0;
    utest_check(sscanf("1.5", "%f", &fl) == 1 && fl == 1.5f,
          "%f is a FLOAT and %lf a double, the one modifier that changes the type");
    char word[16] = {0};
    utest_check(sscanf("  hello world", "%s", word) == 1 && strcmp(word, "hello") == 0,
          "%s skips leading space and stops at the next");
    utest_check(sscanf("abcdef", "%3s", word) == 1 && strcmp(word, "abc") == 0,
          "a width bounds it");
    char two[3] = {0};
    utest_check(sscanf("ab", "%2c", two) == 1 && two[0] == 'a' && two[1] == 'b',
          "%c reads a block and does NOT skip whitespace");
    a = b = 0;
    utest_check(sscanf("5 6", "%*d %d", &a) == 1 && a == 6,
          "* suppresses an assignment without consuming an argument");
    int pos = 0;
    utest_check(sscanf("42abc", "%d%n", &a, &pos) == 1 && pos == 2,
          "%n reports the position and does not count as an assignment");
    utest_check(sscanf("", "%d", &a) == EOF, "empty input before the first assignment is EOF");
    utest_check(sscanf("q", "%d", &a) == 0, "but unmatched input is 0, not EOF");

    // --- an unseekable stream says so --------------------------------
    utest_check(fseek(stdout, 0, SEEK_SET) == -1 && sys_errno() == ESPIPE,
          "seeking a terminal stream is refused");

    sys_unlink(PATH);
    sys_unlink(BIG);

    int code = utest_end();

    // Registered A then B, so LIFO runs B first and the console must
    // show "atexit:BA". Neither handler ends its output with a newline,
    // so the text can only appear at all if exit() flushed.
    atexit(say_A);
    atexit(say_B);
    printf("atexit:");
    return code;
}
