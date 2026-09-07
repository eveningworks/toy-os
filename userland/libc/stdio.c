// The C library's stream layer: FILE, buffering, and the printf family.
//
// See <stdio.h> for the contract. This file holds the three things that
// have real design in them.
//
// **A FILE IS A READER OR A WRITER, NEVER BOTH.** That is not a
// simplification taken for convenience -- the kernel's own open file
// has a single mode (SYS_O_WRITE or not), so there is no read-write
// descriptor to build "r+" on. It removes the whole of a real libc's
// hardest corner (the flush-and-reposition dance when a stream switches
// direction) and the removal is honest rather than hidden: fopen()
// refuses the modes it cannot serve.
//
// **BUFFERING POLICY IS THE STANDARD ONE, AND IT IS WHY THIS EXISTS.**
// stderr unbuffered, a terminal line buffered, everything else fully
// buffered. Without it printf() is one syscall per call, which is worse
// than the sys_print()-shaped code it replaces -- so a stdio that
// skipped buffering would make the system slower and not be worth
// having. "Is it a terminal" is one SYS_FSTAT per stream, asked lazily
// on first use rather than at fopen(), so a stream nobody touches costs
// nothing.
//
// **PRINTF GOES THROUGH kfmt's SINK FORM, NOT A SCRATCH BUFFER.**
// k_vcbprintf() hands out bytes as it produces them (api/kfmt.h), so
// there is exactly one formatter in the tree and a printf() has no
// maximum line length. The obvious alternative -- format into a fixed
// buffer, then write it -- is what vga_printf() does, and it caps
// everything a program can print at a number this file would have had
// to invent.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include <errno.h>
#include "tmppath.h"   // TMP_DIR_DEFAULT, and enum tmp_kind
#include "kpath.h"     // k_path_join -- refuses rather than truncating
#include "setting_abi.h"

// --- the stream ------------------------------------------------------

#define F_READ    0x01
#define F_WRITE   0x02
#define F_EOF     0x04
#define F_ERR     0x08
#define F_INUSE   0x10
#define F_OWNBUF  0x20 // the buffer came from malloc and fclose frees it
#define F_MODESET 0x40 // the buffering policy has been decided

// How many bytes of pushback a stream holds. Eight is comfortably more
// than any conversion needs and costs eight bytes per FILE.
#define UNGET_MAX 8

struct _FILE {
    int fd;
    unsigned char *buf;
    size_t bufsz;
    size_t pos;   // reading: next byte to hand out. writing: bytes pending.
    size_t end;   // reading: bytes valid in buf. unused when writing.
    // PUSHBACK, DEEPER THAN C REQUIRES. The standard guarantees one
    // byte and permits more. scanf needs more than one: deciding that
    // "0x" is not the start of a number, or that "1e" has no exponent,
    // means putting several characters back -- and a scanf built on a
    // one-byte pushback has to buffer its own input instead, which is
    // a second buffer in front of this one.
    unsigned char ungetbuf[UNGET_MAX];
    int ungetn;   // 0 = nothing pushed back; the stack grows upward
    short mode;   // _IOFBF / _IOLBF / _IONBF
    short flags;
};

// The three standard streams get STATIC buffers, so a program that
// never opens a file makes no allocation at all and printf() works
// before (and after) the heap does. Everything fopen() returns takes
// its buffer from malloc.
static unsigned char g_inbuf[BUFSIZ];
static unsigned char g_outbuf[BUFSIZ];

static FILE g_std[3] = {
    { 0, g_inbuf,  BUFSIZ, 0, 0, {0}, 0, _IOFBF, F_READ  | F_INUSE },
    { 1, g_outbuf, BUFSIZ, 0, 0, {0}, 0, _IOFBF, F_WRITE | F_INUSE },
    // stderr is unbuffered and says so up front: F_MODESET keeps the
    // policy below from asking fstat and deciding otherwise. A
    // diagnostic that is still sitting in a buffer when the process
    // dies is a diagnostic that did not happen.
    { 2, 0,        0,      0, 0, {0}, 0, _IONBF, F_WRITE | F_INUSE | F_MODESET },
};

FILE *const stdin  = &g_std[0];
FILE *const stdout = &g_std[1];
FILE *const stderr = &g_std[2];

static FILE g_files[FOPEN_MAX];

// --- policy ----------------------------------------------------------

// Decides line-vs-full buffering the first time a stream is used. One
// syscall per stream, and only for streams that are actually touched.
static void decide_buffering(FILE *f) {
    if (f->flags & F_MODESET) return;
    f->flags |= F_MODESET;
    struct sys_stat st;
    if (sys_fstat(f->fd, &st) == 0 && (st.flags & SYS_STAT_TTY))
        f->mode = _IOLBF;
    else
        f->mode = _IOFBF;
}

// A stream with no buffer is unbuffered, whatever its mode says --
// which is how stderr and a failed malloc reach the same code path
// instead of the second one being a special case nobody tested.
static int unbuffered(FILE *f) { return f->mode == _IONBF || !f->buf; }

// --- writing ---------------------------------------------------------

// Writes exactly n bytes, looping. SYS_WRITE silently CAPS a write at
// SYS_WRITE_MAX and returns the capped count, so a single call is not
// enough even for a buffer this file sized to match -- and a caller
// that assumed otherwise would drop the tail of every long write
// without any error to notice.
static int write_all(FILE *f, const unsigned char *p, size_t n) {
    while (n) {
        int64_t w = sys_write(f->fd, p, n);
        if (w <= 0) { f->flags |= F_ERR; return -1; }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int flush_write(FILE *f) {
    if (!(f->flags & F_WRITE) || !f->pos) return 0;
    size_t n = f->pos;
    f->pos = 0; // cleared FIRST: a failed write must not be retried by
                // the next flush, which would emit it twice
    return write_all(f, f->buf, n);
}

// INPUT ON A TERMINAL FLUSHES stdout FIRST, which is what makes a
// prompt with no '\n' appear before the reader blocks on the answer.
// C11 7.21.3p3 lists this among the moments a line-buffered stream
// transmits, and both glibc (_IO_new_file_underflow) and MSVC do it;
// without it `printf("Give me a number: "); scanf(...)` reads from a
// blank screen and prints the prompt afterwards, at exit.
//
// stdout ALONE, not every line-buffered stream. glibc has the general
// form (_IO_flush_all_linebuffered) and ships this fast path instead,
// because the case it serves is a prompt and prompts go to stdout; the
// general form costs a walk of the whole stream table on every refill.
// Gated on the INPUT stream being line-buffered or unbuffered -- i.e. a
// terminal -- so reading a file does not flush anything, and cheap when
// it does fire, since flush_write() returns at once on an empty buffer.
static void flush_stdout_for_read(FILE *f) {
    if (f->mode == _IOLBF || unbuffered(f)) flush_write(&g_std[1]);
}

int fflush(FILE *f) {
    if (f) return flush_write(f) < 0 ? EOF : 0;
    // NULL means every stream, and one failure must not stop the rest
    // from being flushed -- the point of flushing everything is that
    // the process is going away.
    int rc = 0;
    for (int i = 0; i < 3; i++) if (flush_write(&g_std[i]) < 0) rc = EOF;
    for (int i = 0; i < FOPEN_MAX; i++)
        if ((g_files[i].flags & F_INUSE) && flush_write(&g_files[i]) < 0) rc = EOF;
    return rc;
}

int fputc(int c, FILE *f) {
    if (!f || !(f->flags & F_WRITE)) return EOF;
    decide_buffering(f);
    unsigned char ch = (unsigned char)c;
    if (unbuffered(f)) return write_all(f, &ch, 1) < 0 ? EOF : (int)ch;
    f->buf[f->pos++] = ch;
    if (f->pos == f->bufsz || (f->mode == _IOLBF && ch == '\n'))
        if (flush_write(f) < 0) return EOF;
    return (int)ch;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !(f->flags & F_WRITE) || !size || !nmemb) return 0;
    decide_buffering(f);
    size_t total = size * nmemb;
    const unsigned char *p = (const unsigned char *)ptr;

    // A block at least as big as the buffer goes STRAIGHT out: copying
    // it in to copy it out again is pure cost, and it is what makes
    // writing a large file O(bytes) rather than O(bytes) plus a
    // memcpy of the same size.
    if (unbuffered(f) || total >= f->bufsz) {
        if (flush_write(f) < 0) return 0;
        if (write_all(f, p, total) < 0) return 0;
        return nmemb;
    }
    if (f->pos + total > f->bufsz && flush_write(f) < 0) return 0;
    k_memcpy(f->buf + f->pos, p, total);
    f->pos += total;
    // Line buffering has to look INSIDE the block: a caller writing a
    // whole line with fwrite() expects it out at the newline just as
    // much as one writing it with putchar().
    if (f->mode == _IOLBF) {
        for (size_t i = 0; i < total; i++)
            if (p[i] == '\n') { if (flush_write(f) < 0) return 0; break; }
    }
    return nmemb;
}

int fputs(const char *s, FILE *f) {
    size_t n = k_strlen(s);
    return fwrite(s, 1, n, f) == n || n == 0 ? 0 : EOF;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}

// --- reading ---------------------------------------------------------

static int refill(FILE *f) {
    if (!(f->flags & F_READ)) return -1;
    decide_buffering(f);
    flush_stdout_for_read(f);
    f->pos = f->end = 0;
    if (!f->buf) return -1;
    int64_t n = sys_read(f->fd, f->buf, f->bufsz);
    if (n < 0) { f->flags |= F_ERR; return -1; }
    if (n == 0) { f->flags |= F_EOF; return -1; }
    f->end = (size_t)n;
    return 0;
}

int fgetc(FILE *f) {
    if (!f || !(f->flags & F_READ)) return EOF;
    if (f->ungetn > 0) return (int)f->ungetbuf[--f->ungetn];
    decide_buffering(f);
    if (unbuffered(f)) {
        // Here rather than at the top: a byte already in the buffer is
        // not a read, and flushing per character would be a syscall's
        // worth of compare for every getchar() in a loop.
        flush_stdout_for_read(f);
        unsigned char ch;
        int64_t n = sys_read(f->fd, &ch, 1);
        if (n < 0) { f->flags |= F_ERR; return EOF; }
        if (n == 0) { f->flags |= F_EOF; return EOF; }
        return (int)ch;
    }
    if (f->pos == f->end && refill(f) < 0) return EOF;
    return (int)f->buf[f->pos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    // ONE byte, and never a backward seek: a pipe and a terminal have
    // no position to seek, and this is the call a parser uses to look
    // one character ahead on exactly those.
    if (!f || c == EOF || f->ungetn >= UNGET_MAX) return EOF;
    f->ungetbuf[f->ungetn++] = (unsigned char)c;
    f->flags &= (short)~F_EOF; // pushing back un-ends the stream
    return c;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !(f->flags & F_READ) || !size || !nmemb) return 0;
    unsigned char *p = (unsigned char *)ptr;
    size_t want = size * nmemb, got = 0;
    while (got < want) {
        int c = fgetc(f);
        if (c == EOF) break;
        p[got++] = (unsigned char)c;
    }
    return got / size;
}

char *fgets(char *s, int size, FILE *f) {
    if (!s || size <= 0 || !f) return 0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0; // EOF or error with nothing read
    s[i] = '\0';
    return s;
}

// --- formatted output ------------------------------------------------

// The sink kfmt hands bytes to. It is deliberately the ordinary write
// path rather than a fast one: everything printf() emits goes through
// the same buffering and the same line-flush rule as everything else,
// so `printf("a"); putchar('b');` cannot come out in the wrong order.
static void stream_sink(void *ctx, const char *s, size_t n) {
    FILE *f = (FILE *)ctx;
    for (size_t i = 0; i < n; i++) fputc((unsigned char)s[i], f);
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f || !(f->flags & F_WRITE)) return -1;
    size_t n = k_vcbprintf(stream_sink, f, fmt, ap);
    return (f->flags & F_ERR) ? -1 : (int)n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

// --- sprintf ----------------------------------------------------------
//
// UNBOUNDED, which is why <stdio.h> called it deliberately absent until
// a ported program needed it. The reversal is the same one <string.h>
// made for strncpy: while this library served only toy-os's code,
// refusing a footgun cost nothing; for a port-capable library, omitting
// a function C requires does not produce a helpful message, it produces
// a link error in somebody else's source. So it exists with the warning
// attached, and snprintf() stays the one to reach for here.
//
// Built on the SINK form, which is what makes it correct rather than
// merely present: there is no intermediate buffer whose size would cap
// the output, so it writes exactly what the format produces.
static void raw_sink(void *ctx, const char *s, size_t n) {
    char **pp = (char **)ctx;
    for (size_t i = 0; i < n; i++) *(*pp)++ = s[i];
}

int vsprintf(char *out, const char *fmt, va_list ap) {
    char *p = out;
    size_t n = k_vcbprintf(raw_sink, &p, fmt, ap);
    *p = '\0';
    return (int)n;
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

// --- position --------------------------------------------------------

int fseek(FILE *f, long offset, int whence) {
    if (!f) return -1;
    if (f->flags & F_WRITE) {
        if (flush_write(f) < 0) return -1;
    } else {
        // SEEK_CUR must be relative to where the CALLER thinks it is,
        // which is not where the fd is: the buffer holds bytes already
        // read from the fd and not yet handed out, and a pushback byte
        // sits one further back still. Getting this wrong is invisible
        // until a file is read through a buffer boundary.
        if (whence == SEEK_CUR) {
            long behind = (long)(f->end - f->pos) + f->ungetn;
            offset -= behind;
        }
        f->pos = f->end = 0;
    }
    f->ungetn = 0;
    if (sys_lseek(f->fd, offset, whence) < 0) return -1;
    f->flags &= (short)~F_EOF;
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    if (f->flags & F_WRITE) {
        if (flush_write(f) < 0) return -1;
        return (long)sys_lseek(f->fd, 0, SEEK_CUR);
    }
    long at = (long)sys_lseek(f->fd, 0, SEEK_CUR);
    if (at < 0) return -1;
    return at - (long)(f->end - f->pos) - f->ungetn;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); if (f) f->flags &= (short)~F_ERR; }

// --- open and close --------------------------------------------------

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    (void)size;
    // Only what can be honoured: a mode, and only before the stream has
    // been used. Refusing a caller-supplied buffer rather than ignoring
    // it, because silently not using the memory somebody handed over is
    // how a caller ends up believing a guarantee it does not have.
    if (!f || buf) return -1;
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) return -1;
    if (f->pos || f->end) return -1;
    f->mode = (short)mode;
    f->flags |= F_MODESET;
    return 0;
}

int fileno(FILE *f) { return f ? f->fd : -1; }

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return 0;

    int flags, want_read;
    switch (mode[0]) {
    case 'r': flags = 0;                                     want_read = 1; break;
    case 'w': flags = SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC;  want_read = 0; break;
    case 'a': flags = SYS_O_WRITE | SYS_O_CREAT | SYS_O_APPEND; want_read = 0; break;
    default: return 0;
    }
    // 'b' is accepted and means nothing (there is no text mode here);
    // '+' is REFUSED, because the kernel's open file has one mode and a
    // read-write stream cannot be built over it. Returning NULL is the
    // honest answer -- opening read-only instead would look like it
    // worked until the first write.
    for (const char *p = mode + 1; *p; p++)
        if (*p != 'b') return 0;

    FILE *f = 0;
    for (int i = 0; i < FOPEN_MAX; i++)
        if (!(g_files[i].flags & F_INUSE)) { f = &g_files[i]; break; }
    if (!f) return 0;

    int fd = sys_open(path, flags);
    if (fd < 0) return 0;

    unsigned char *buf = (unsigned char *)malloc(BUFSIZ);
    // A stream with no buffer still WORKS -- unbuffered() sends it down
    // the one-byte path -- so a failed allocation costs speed rather
    // than the open. That is why there is no error return here.
    f->fd = fd;
    f->buf = buf;
    f->bufsz = buf ? BUFSIZ : 0;
    f->pos = f->end = 0;
    f->ungetn = 0;
    f->mode = buf ? _IOFBF : _IONBF;
    f->flags = (short)(F_INUSE | (want_read ? F_READ : F_WRITE) |
                       (buf ? F_OWNBUF : F_MODESET));
    return f;
}

int fclose(FILE *f) {
    if (!f || !(f->flags & F_INUSE)) return EOF;
    int rc = flush_write(f) < 0 ? EOF : 0;
    if (sys_close(f->fd) < 0) rc = EOF;
    if (f->flags & F_OWNBUF) free(f->buf);
    // A standard stream can be closed like any other; it just does not
    // return to a pool, and must not be left looking reusable.
    if (f >= g_files && f < g_files + FOPEN_MAX) {
        f->flags = 0;
        f->buf = 0;
    } else {
        f->flags &= (short)~(F_READ | F_WRITE);
    }
    return rc;
}

int feof(FILE *f) { return f && (f->flags & F_EOF) ? 1 : 0; }
int ferror(FILE *f) { return f && (f->flags & F_ERR) ? 1 : 0; }
void clearerr(FILE *f) { if (f) f->flags &= (short)~(F_EOF | F_ERR); }

// --- the rest of C's stdio surface -------------------------------------

// ERRNO IS READ FIRST. Everything below this line can change it --
// fputs, the write, strerror's own lookup -- so capturing it before any
// of that runs is the difference between reporting the caller's failure
// and reporting the report.
void perror(const char *s) {
    int e = errno;
    if (s && *s) { fputs(s, stderr); fputs(": ", stderr); }
    fputs(strerror(e), stderr);
    fputc('\n', stderr);
}

// C's two-choice front end to setvbuf(), with no way to report failure.
void setbuf(FILE *f, char *buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

// fpos_t is the byte offset here, so these are ftell/fseek with the
// position passed by pointer. Kept as their own functions rather than
// macros because C allows the type to carry more than an offset, and a
// caller written against that should keep compiling if it ever does.
int fgetpos(FILE *f, fpos_t *pos) {
    if (!pos) return -1;
    long off = ftell(f);
    if (off < 0) return -1;
    *pos = (fpos_t)off;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos) {
    if (!pos) return -1;
    return fseek(f, (long)*pos, SEEK_SET) == 0 ? 0 : -1;
}

// --- temporary files ---------------------------------------------------
//
// THE DIRECTORY IS A SETTING, NEVER A SPELLED PATH (api/tmppath.h): a
// literal "/tmp" keeps working on a default machine and silently
// ignores `storage.tmpdir` on any other. TMP_VOLATILE, because a
// temporary file is exactly what nobody wants surviving a reboot.
//
// The name carries the pid, so two processes cannot collide, and a
// counter, so one process asking twice cannot either.
// THE SETTING IS READ HERE RATHER THAN THROUGH tmppath(), and that is
// a layering constraint, not a preference: tmppath()'s ring-3 half
// (userland/lib/utmppath.c) lives in libuapp, the C library is linked
// AFTER it, and libc calls nothing in Toykit. A syscall it may make
// directly, so it makes the same one usetting_get() would.
//
// Cached for the reason utmppath.c caches: otherwise every call is a
// round trip, and a program whose scratch directory changed halfway
// through would leave half its files somewhere nobody looks.
static const char *tmpdir(void) {
    static char dir[64];
    if (dir[0]) return dir;

    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, "storage.tmpdir", sizeof m.name);
    // Anything not absolute is refused rather than used: a relative
    // scratch directory would resolve against whatever the process's
    // cwd happens to be. A missing registry is not an error -- the
    // compiled default is the directory the layout pass created.
    if (sys_setting(&m) == 0 && m.value[0] == '/')
        strlcpy(dir, m.value, sizeof dir);
    else
        strlcpy(dir, TMP_DIR_DEFAULT, sizeof dir);
    return dir;
}

char *tmpnam(char *s) {
    static char own[L_tmpnam];
    static unsigned seq;
    char name[32];
    char *out = s ? s : own;

    // The pid keeps two processes apart, the counter keeps one process
    // asking twice apart.
    snprintf(name, sizeof name, "tmp%d-%u", sys_getpid(), seq++);
    if (!k_path_join(tmpdir(), name, out, L_tmpnam)) { out[0] = '\0'; return 0; }
    return out;
}

// --- getline / getdelim ------------------------------------------------

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f) {
    if (!lineptr || !n || !f) { errno = EINVAL; return -1; }

    size_t len = 0;
    for (;;) {
        // GROW BEFORE THE STORE, and keep room for the NUL: a caller
        // may hand in a NULL buffer with *n = 0, which is the documented
        // way to start.
        if (len + 2 > *n) {
            size_t want = *n ? *n * 2 : 128;
            char *bigger = realloc(*lineptr, want);
            if (!bigger) { errno = ENOMEM; return -1; }
            *lineptr = bigger;
            *n = want;
        }
        int c = fgetc(f);
        if (c == EOF) {
            // END OF FILE WITH BYTES IN HAND IS NOT A FAILURE -- a last
            // line with no newline is still a line. Only an immediately
            // empty read is -1.
            if (len == 0) return -1;
            break;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim) break;
    }
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *f) {
    return getdelim(lineptr, n, '\n', f);
}
