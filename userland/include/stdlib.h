#ifndef USERLAND_LIB_STDLIB_H
#define USERLAND_LIB_STDLIB_H

// malloc/free for ring 3, under the C names.
//
// NOT a second allocator: this is the kernel's own
// kernel/lib/heap_core.c compiled a second time into libuapp.a, with
// SYS_SBRK behind it instead of the physical frame allocator (see
// api/heap_os.h). So a ring-3 malloc and the kernel's kmalloc cannot
// disagree about splitting, coalescing or double frees -- the same rule
// that already makes ring-3 `strlen` and `k_strlen` one implementation.
//
// It also means `heap debug on`'s machinery is here: red-zones each
// side of the payload and poison on free, catching an overflow or a
// use-after-free at the free() that follows it.
//
// **What it does NOT do is return memory to the kernel.** sbrk cannot
// move down, so free() returns a block to this process's free list and
// the process's footprint never shrinks. See userland/lib/heap_os.c.
//
// Deliberately absent: `aligned_alloc`, since nothing needs an
// alignment stronger than the allocator's 16.
#include <stddef.h>
#include <heap.h> // the toolkit allocator this header renames

// --- numbers from text -----------------------------------------------
//
// NOT wrappers over knum.h's k_parse_* family, and the reason is the
// contract rather than the arithmetic: k_parse_u64() answers "did the
// WHOLE string parse", which is what a config file wants, while
// strtol() must stop at the first byte it cannot use and hand that
// position back. A parser built on the first cannot be built out of the
// second without re-scanning, and vice versa. Both are correct; they
// answer different questions.
//
// `base` is 0 or 2..36. Base 0 means C's own literal rules: leading
// "0x" is hex, a leading "0" is octal, anything else decimal.
//
// On overflow the result SATURATES (LONG_MAX/LONG_MIN, ULONG_MAX) and
// errno is set to ERANGE, which is what C requires -- and is why a
// caller that cares must clear errno first: a successful call does not
// clear it.
//
// `endptr`, when not NULL, receives the first unconsumed byte. If no
// conversion was possible it receives `nptr` itself, so a caller can
// tell "0" from "not a number" -- both return 0.
long           strtol(const char *nptr, char **endptr, int base);
unsigned long  strtoul(const char *nptr, char **endptr, int base);
// The `long long` family. Same contract, and on this LP64 ABI the same
// range -- they exist because C requires them, which is this library's
// bar (see userland/libc/README.md), not because they can do more.
long long          strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
// atoi()/atol() are strtol() with the errors thrown away, which is
// exactly what C says they are. They cannot report anything, so reach
// for strtol() in new code.
int            atoi(const char *s);
long           atol(const char *s);
long long      atoll(const char *s);

// Floating point from text. THE SAME ACCURACY CAVEAT as printf's %f
// (userland/libc/printf_float.c): digits are accumulated by
// multiply-and-add rather than by exact arithmetic over the mantissa,
// so the last place is not guaranteed and strtod(printf("%.17g")) is
// not a round trip. "inf" and "nan" are accepted, as C requires.
double         strtod(const char *nptr, char **endptr);
double         atof(const char *s);

// --- arithmetic -------------------------------------------------------

int   abs(int v);
long  labs(long v);
long long llabs(long long v);

// C's integer divisions. The point of them is that quotient and
// remainder come back TOGETHER: `/` and `%` are two operations the
// compiler usually fuses anyway, and C guarantees the pair is
// consistent for negative operands, which is where an open-coded
// version goes wrong.
typedef struct { int quot, rem; }             div_t;
typedef struct { long quot, rem; }            ldiv_t;
typedef struct { long long quot, rem; }       lldiv_t;
div_t   div(int num, int den);
ldiv_t  ldiv(long num, long den);
lldiv_t lldiv(long long num, long long den);

// --- sorting and searching --------------------------------------------
//
// **qsort IS NOT STABLE**, and this matters here because both existing
// sorts in this tree deliberately are: uui_table keeps a previous
// column's order within ties (the behaviour every desktop table has)
// and dirsort tie-breaks by name. Neither can be replaced by this, and
// neither should be -- qsort is here because a C library has one, not
// because anything in toy-os was waiting for it.
//
// The comparator returns <0, 0 or >0, as C's does.
void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

// --- pseudo-random ----------------------------------------------------
//
// **NOT the kernel's entropy source, and not a substitute for it.**
// This is C's rand(): a deterministic sequence from a seed, for
// shuffling and sampling. Anything that must be unguessable asks
// sys_getrandom() (rt/sys.h), which is krandom.h's real generator and
// reports its own quality.
//
// The generator is the one C99 itself prints as an example, so a
// sequence is reproducible from a seed across any implementation that
// copies the standard's -- which is what makes a seeded test repeatable.
#define RAND_MAX 32767
int   rand(void);
void  srand(unsigned seed);

// --- the environment --------------------------------------------------
//
// `environ` is libsys's (rt/sys.h) -- crt0 sets it from the initial
// stack, because argc, argv and envp arrive together and crt0 is that
// layer. These are the C API over it.
//
// **A CHILD INHERITS BECAUSE THIS LIBRARY PASSES IT**, not because the
// kernel keeps one: sys_spawn() hands `environ` to SYS_SPAWN, and
// sys_spawn_env() is the form that takes one explicitly. That is
// execv() and execve(), and toy-os copies the split deliberately.
//
// getenv() returns a pointer INTO the environment -- valid until the
// next setenv()/unsetenv() touching that name, as C specifies. Copy it
// if you need it to outlive that.
char *getenv(const char *name);
// Copies both strings. `overwrite` of 0 leaves an existing value alone
// and still reports success. Returns 0, or -1 with errno EINVAL (a NULL
// or empty name, or one containing '=') or ENOMEM.
int   setenv(const char *name, const char *value, int overwrite);
// Removing something absent is NOT an error, as C says.
int   unsetenv(const char *name);
// **STORES THE CALLER'S POINTER, with no copy** -- C's real contract,
// and the reason setenv() exists. The string must outlive the call, so
// a stack buffer here leaves the environment pointing at a dead frame.
int   putenv(char *entry);
int   clearenv(void);

// --- leaving ---------------------------------------------------------
//
// exit() runs the atexit handlers in reverse order of registration and
// then FLUSHES EVERY STREAM. crt0 calls it when main() returns, which
// is what makes a buffered printf() reliable; a program that calls
// sys_exit() (rt/sys.h) directly bypasses both.
void exit(int code) __attribute__((noreturn));
// Returns 0, or -1 if the handler is NULL or the table (32 entries, C's
// minimum) is full.
int  atexit(void (*fn)(void));

// _Exit() leaves WITHOUT running atexit handlers and without flushing.
// It is what a child that has decided not to be its parent any more
// calls, and what a handler calls to stop the rest of them running.
void _Exit(int code) __attribute__((noreturn));

// quick_exit()/at_quick_exit() are C11's SECOND, separate list. The
// separation is the feature: a quick-exit handler is for the things
// that must happen even when the ordinary teardown is being skipped,
// so registering on one list never runs the other's.
void quick_exit(int code) __attribute__((noreturn));
int  at_quick_exit(void (*fn)(void));
// Exits with 134 (128 + SIGABRT) WITHOUT flushing the buffered streams:
// abort() means the state is not to be trusted, and committing a
// half-written file is worse than losing it. stderr is unbuffered, so
// anything already reported is already out.
void abort(void) __attribute__((noreturn));

// Runs `command` through /bin/tosh -c and waits for it. Returns the
// child's status in <sys/wait.h>'s encoding, or -1 if the shell could
// not be run. A NULL command asks whether a command processor exists at
// all and returns non-zero if one does -- POSIX's own contract, and a
// real check here rather than a constant.
//
// It does NOT block SIGCHLD or ignore SIGINT/SIGQUIT for the duration
// the way POSIX asks, because this libc has no sigprocmask(); see
// userland/libc/system.c for what that costs a caller.
int system(const char *command);

// The same contract as kmalloc(): NULL for a zero-sized or unsatisfiable
// request, 16-byte aligned otherwise.
static inline void *malloc(size_t n) { return kmalloc(n); }

// Zeroed. calloc()'s two arguments are multiplied here WITHOUT an
// overflow check on purpose being a thing to notice: n * size can wrap,
// and a wrapped product allocates a small block for a huge request.
// Checked below rather than trusted.
static inline void *calloc(size_t n, size_t size) {
    if (n && size && n > (size_t)-1 / size) return 0; // would wrap
    return kzalloc(n * size);
}

static inline void free(void *p) { kfree(p); }

// Grows or shrinks a block, COPYING every time it moves -- there is no
// grow-in-place path, because the shared allocator has no call for
// "extend this block if the next one is free" and adding one to satisfy
// a single caller is not the bar this project holds. realloc(NULL, n)
// is malloc(n) and realloc(p, 0) frees and returns NULL, as C says.
//
// It cannot make the process smaller either way: free() returns a block
// to this process's own list and sbrk never moves down.
void *realloc(void *p, size_t n);


// **THE COMPILER'S, NOT A FUNCTION.** alloca() cannot be a library call:
// it allocates in the CALLER's frame, which only the compiler can do.
// __builtin_alloca is how every libc spells it.
//
// Here rather than in a separate <alloca.h>, which is a glibc invention
// -- BSD and macOS put it in this header too. **RING-3 CODE HAS A FRAME
// BUDGET** (docs/conventions/build.md) and alloca spends it at RUNTIME,
// where -Wframe-larger-than cannot see it, so a loop that allocas is a
// stack overflow the build will not catch.
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif

// Creates a unique file from a template ending in six `X`s, replacing
// them in place, and returns it OPEN -- which is what makes it safe
// where mktemp() is not: the name cannot be taken between the choice
// and the use. The caller owns the file, including removing it.
int mkstemp(char *template);

#endif
