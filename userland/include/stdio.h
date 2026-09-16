#ifndef ULIB_STDIO_H
#define ULIB_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h> // ssize_t, for getline()/getdelim()
#include <kfmt.h> // the toolkit's formatter; no name clash, unlike string.h's

// The C library's stream layer and formatted output, for ring 3 only.
// `#include <stdio.h>`. See <string.h>'s header comment for the shared
// rationale -- this is its other half, and the two were built together.
//
// THE CONVERSION SET IS kfmt's, NOT A FULL printf's -- but it is most
// of one now: %d %i %u %o %x %X %c %s %p %%, plus %f %e %g (%F %E %G),
// which are RING 3 ONLY and so are usable here. C's flags (- + space #
// 0), width (digits or `*`), precision (.N or .*, honoured by the
// integer and float conversions and ignored by %s) and the l/ll/z/h/hh
// length modifiers are all parsed. Missing: %n, %a and the wide-char
// conversions. An unrecognised conversion is emitted literally and
// consumes no argument, so a typo shows up in the output rather than
// desynchronising every argument after it. Read kfmt.h before assuming
// a conversion exists.
//
// WHAT A STREAM IS HERE, and the one place it differs from POSIX in a
// way a caller can see: **a FILE is a reader or a writer, never both.**
// The kernel's own fd has a single mode (SYS_O_WRITE or not), so there
// is no "r+"/"w+"/"a+" to implement over it. fopen() REFUSES those
// modes rather than quietly opening something weaker -- a parser that
// guesses is the failure this project's conventions exist to prevent.
//
// BUFFERING is the standard policy and it is the reason this layer
// exists at all: an unbuffered printf() is one syscall per call, which
// is worse than the sys_print()-shaped code it replaces. stderr is
// unbuffered, a stream on a terminal is line buffered, everything else
// is fully buffered. "Is it a terminal" is SYS_FSTAT's SYS_STAT_TTY
// flag, asked once per stream.
//
// THE TRAP THAT COMES WITH BUFFERING: output written but not yet
// flushed is LOST if the process leaves without going through exit().
// crt0 calls exit() when main() returns, and exit() flushes every
// stream -- but a program that calls sys_exit() directly (rt/sys.h)
// bypasses all of it, as does a crash. If you mix printf() with
// sys_write(1, ...) in one program the two orders will not agree
// either; pick one per stream.

typedef struct _FILE FILE;

extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

#define EOF (-1)

// One flush is one write syscall, so this trades memory for syscalls --
// and the memory is REAL: .bss is eagerly committed (elf_load() zeroes
// every page up to p_memsz), so 2 x BUFSIZ is resident in every process
// from spawn whether it prints or not, plus one more per fopen().
//
// 16384 is measured, on HARDWARE: writing 64-byte records to a TFS3
// disk it is 2.15x the throughput of 4096, and the sizes above it buy
// +34% and then +7% for each doubling of that resident cost.
// docs/libc-design.md has the curve and the two runs behind it.
//
// **DO NOT RE-TUNE THIS UNDER QEMU.** An emulated disk made the same
// sweep look flat above 4096 (+4%, against 12x on the real one), which
// is how it was first set too low.
//
// **FIXTURES IN stdio_test.c ARE SIZED FROM THIS.** A test that means
// "larger than the buffer" must say so in terms of BUFSIZ -- three were
// written as literals against an older value and silently stopped
// crossing a boundary when it moved.
#define BUFSIZ 16384

// Streams a program may have open at once, over and above the three
// standard ones. Bounded by the kernel's own FD_MAX (16 descriptors per
// address space), so a bigger number here could not be honoured anyway.
#define FOPEN_MAX 8

#define _IOFBF 0 // fully buffered
#define _IOLBF 1 // line buffered
#define _IONBF 2 // unbuffered

// The same three values SYS_LSEEK takes (abi/syscall_abi.h), named as C
// spells them.
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// mode is "r", "w" or "a", optionally with a 'b' that is accepted and
// ignored (there is no text/binary distinction here). Anything else,
// including every "+" mode, returns NULL -- see the header comment.
FILE *fopen(const char *path, const char *mode);
// Flushes, closes the descriptor and releases the slot. Returns 0, or
// EOF if the final flush failed -- which is the one place a buffered
// write reports a disk error, since the write() that failed happened
// long after the fputc() that queued it.
int   fclose(FILE *f);
// NULL flushes every open stream. Returns 0, or EOF on a write error.
int   fflush(FILE *f);
// Only mode changes are supported and only before any I/O on the
// stream; `buf` must be NULL and `size` is ignored. Returns 0, or -1.
// It exists so a program can turn the buffering OFF -- without it there
// is no escape hatch from a policy this layer chose.
int   setvbuf(FILE *f, char *buf, int mode, size_t size);
// setbuf() is setvbuf() with C's two fixed choices and no way to report
// failure, which is why setvbuf() is the one to reach for.
void  setbuf(FILE *f, char *buf);
int   fileno(FILE *f);

// A stream over an fd stdio did not open (a pipe, a socket, an inherited
// descriptor), and a reopen that KEEPS the caller's FILE * -- which is
// what redirecting stdout needs, since its address is a constant.
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);

// --- explicit locking -------------------------------------------------
//
// Every stdio function above already takes the stream's lock, so each is
// atomic against the others. These exist for what that does NOT cover: a
// SEQUENCE which must not be interleaved -- a prompt and the read of its
// answer, or a report that spans several calls.
//
// The lock is REENTRANT, so holding it across calls that take it again
// is safe and is the intended use.
void  flockfile(FILE *f);
void  funlockfile(FILE *f);
int   ftrylockfile(FILE *f);   // 0 if acquired, non-zero if it would block

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);

int   fgetc(FILE *f);
int   getc(FILE *f);
int   getchar(void);
// ONE byte of pushback, which is all C promises and all this
// implements. It is NOT a backward seek: a pipe and a terminal have no
// position, and implementing it as a seek would break on both.
int   ungetc(int c, FILE *f);
char *fgets(char *s, int size, FILE *f);

int   fputc(int c, FILE *f);
int   putc(int c, FILE *f);
int   putchar(int c);
int   fputs(const char *s, FILE *f);
// Appends a newline, as C requires and as fputs() does not.
int   puts(const char *s);

// UNBOUNDED -- it writes as much as the format produces and cannot be
// told how big `out` is. It exists because C requires it and ported code
// uses it; snprintf() is the one to reach for in code written here.
int   sprintf(char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int   vsprintf(char *out, const char *fmt, va_list ap);

// Reads formatted input from a STRING. Conversions: %d %i %u %o %x %c
// %s %f/%e/%g %n %%, with a width, `*` to suppress the assignment, and
// the h/hh/l/ll/z length modifiers. Returns the number of items
// assigned, or EOF if input ran out before the first one.
//
// scanf()/fscanf() read from a STREAM through the same scanner -- see
// userland/libc/scanf.c, where a string and a stream differ only in
// where the next character comes from. Note that fgets() plus sscanf()
// is usually the better combination on a terminal, because a failed
// fscanf leaves the offending input in the stream and the obvious retry
// loop spins on it forever.
int   sscanf(const char *s, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int   vsscanf(const char *s, const char *fmt, va_list ap);
int   scanf(const char *fmt, ...) __attribute__((format(scanf, 1, 2)));
int   fscanf(FILE *f, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int   vscanf(const char *fmt, va_list ap);
int   vfscanf(FILE *f, const char *fmt, va_list ap);

int   printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int   fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int   vprintf(const char *fmt, va_list ap);
int   vfprintf(FILE *f, const char *fmt, va_list ap);

// Seeking works only on a stream over a FILE -- a terminal, a pipe and
// a socket have no position, and these report that as -1 with errno
// ESPIPE rather than pretending. A pending write is flushed first and a
// read buffer is discarded, so the position means what the caller
// thinks it means.
int      fseek(FILE *f, long offset, int whence);
long     ftell(FILE *f);
void     rewind(FILE *f);

int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);

// --- formatting into a buffer ---------------------------------------
//
// These two are not part of the stream layer at all: they are kfmt's
// formatter under its C name, and they were here before FILE existed.

// **THEY RETURN int, WHICH IS WHAT C SAYS, and they used to return
// size_t.** The difference is not cosmetic: C specifies a NEGATIVE
// return for an encoding error, so `if (snprintf(...) < 0)` is the
// documented way to check one -- and against an unsigned return that
// comparison is always false, which the compiler folds away silently.
// It also made every `int n = snprintf(...)` a narrowing conversion.
// k_snprintf keeps its size_t signature: it is the kernel's API and has
// no failure value, so the conversion happens here, at the C boundary.
static inline int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    return (int)k_vsnprintf(out, cap, fmt, ap);
}

// An inline rather than the `#define snprintf k_snprintf` this used to
// be. The macro existed to keep GCC's format checking at the call site,
// which a wrapper would lose by re-packing the argument list -- but the
// format ATTRIBUTE gives the same checking, and a real function is what
// lets the return type be corrected above. Taking the address of
// snprintf now works too, which a macro never allowed.
__attribute__((format(printf, 3, 4)))
static inline int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = (int)k_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

// --- files, by name ---------------------------------------------------
//
// ISO C's two file-management functions. They ARE `sys_unlink()` and
// `sys_rename()` under another name, and this header used to list them
// as deliberately absent for exactly that reason -- a wrapper adding
// nothing. That was the wrong bar and is now reversed: `tolibc` aims to
// be COMPLETE, because its audience is code not yet written
// (docs/libc-design.md), and code not yet written calls `remove()`. The
// first real caller was Doom's savegame handling, which is precisely the
// "somebody else's program" case the argument is about.
//
// Both return 0, or -1 with errno set.
int   remove(const char *path);
int   rename(const char *oldpath, const char *newpath);

// perror() writes "<s>: <strerror(errno)>" to stderr, and the errno it
// reports is the one from BEFORE the call -- printing must not be able
// to change the thing being printed.
void  perror(const char *s);

// fpos_t is an OPAQUE position, which is the difference between this
// pair and fseek/ftell: C lets an implementation put more in it than a
// byte offset. Here it is the offset, and saying so costs nothing --
// but a caller must still treat it as a token to hand back.
typedef long fpos_t;
int   fgetpos(FILE *f, fpos_t *pos);
int   fsetpos(FILE *f, const fpos_t *pos);

// tmpnam() names a file in the volatile scratch directory, which is a
// SETTING (api/tmppath.h) rather than a spelled path. The name carries
// the pid and a counter, so neither two processes nor one process
// asking twice can collide.
#define L_tmpnam 64
char *tmpnam(char *s);

// getline()/getdelim() -- POSIX, not ISO C, and here because reading a
// file a line at a time is what most C actually does. The buffer is
// GROWN as needed: *lineptr may be NULL and *n 0 on the first call, and
// the caller frees it at the end. Returns the byte count (the delimiter
// included) or -1 at end of file.
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f);
ssize_t getline(char **lineptr, size_t *n, FILE *f);

// DELIBERATELY ABSENT, both for the same reason: an open file here has
// ONE mode, so `fopen` refuses `+` and no read-write stream can exist.
// freopen() has nothing to do under that model, and tmpfile() is
// specified to open in UPDATE mode -- a write-only version would look
// like it worked until the first read, which is the failure this
// library's `+` refusal exists to avoid. tmpnam() plus fopen() is the
// honest spelling, and it is what a caller wanting a scratch file
// should use.

#endif
