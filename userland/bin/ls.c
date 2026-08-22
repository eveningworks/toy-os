// /bin/ls -- list a directory.
//
// A real ring-3 program over SYS_LISTDIR, taking ordinary C argc/argv.
// argv[0] is whatever path the shell invoked it as; argv[1..] are flags
// and at most one positional directory path.
//
// WITH NO ARGUMENT IT LISTS THE CURRENT DIRECTORY, which is what every
// ls does and what this one did not: it defaulted to `/`, so a bare
// `ls` in /docs listed the root. Nothing noticed because the kernel
// shell had a BUILTIN WRAPPER whose only remaining job was to resolve
// the cwd and pass it in -- one command with two halves in two rings,
// where the ring-3 half was wrong on its own and could not be run on
// its own. Fixing the default deleted the wrapper.
//
// It asks SYS_GETCWD rather than passing "." down, deliberately. The
// cwd is the KERNEL's and every path syscall already resolves against
// it, so "." would work for the listing itself -- but the path is also
// printed in -R headers and joined onto child names, and "./docs:" is
// not what a reader wants where "/docs:" was. Resolving once, up
// front, keeps every later use absolute.
//
// COLOUR IS AN ESCAPE SEQUENCE NOW, NOT A SYSCALL. This used to call
// sys_set_color(), which reaches around the byte stream and changes the
// console's state directly -- so `ls > out.txt` recoloured the console
// while its bytes went to the file, and `ls | cat` coloured whatever the
// console was printing instead. `ESC[36m` travels IN the stream and
// lands wherever the output lands, which is what every real terminal
// does and what makes `--color=never` a thing worth offering.
//
// `--color=auto` IS THE DEFAULT, and it is what a terminal being a real
// object bought. "Colour to a terminal, plain to a pipe" needs to know
// whether fd 1 IS one; that used to be unanswerable here and is now one
// call (sys_isatty(), over SYS_FSTAT's SYS_STAT_TTY). So `ls` on a
// terminal is coloured, `ls > out.txt` and `ls | cat` are not, and
// nobody has to remember a flag -- which is the whole reason real ls
// defaults to auto. `never` and `always` override it.
//
// WHAT IS NOT COLOURED, and why: executables. Real ls colours them from
// the mode bits, and this filesystem has none -- struct sys_dirent carries a
// name, a size, is_dir and a timestamp. Colouring /bin's contents by
// their DIRECTORY would be a rule about where a file sits rather than
// what it is, so directories are coloured and everything else is left
// alone until file permissions exist (docs/roadmap.md).
#include <stdint.h>
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include "lib/dirsort.h"

// The output flags, gathered so the recursion below can pass one thing.
struct opts {
    int long_form;  // -l
    int columns;    // -C -- multi-column; one per line otherwise
    int human;      // -h
    int recurse;    // -R
    int reverse;    // -r
    int sort;       // 'n' name (default), 't' time, 'S' size
    int color;
};

static void put(const char *s) { sys_write(1, s, strlen(s)); }

// ---- colour ----------------------------------------------------------
//
// Only two, deliberately: see this file's top comment on why an
// executable cannot be told apart from a plain file here.
#define C_DIR   "\033[1;36m" // bright cyan
#define C_RESET "\033[0m"

static void put_name(const struct opts *o, const struct sys_dirent *e) {
    if (o->color && e->is_dir) put(C_DIR);
    put(e->name);
    if (o->color && e->is_dir) put(C_RESET);
    // The trailing slash is NOT gated on colour: it is the one piece of
    // type information that survives being piped into another program,
    // which is exactly when colour has been turned off.
    if (e->is_dir) put("/");
}

// ---- sizes -----------------------------------------------------------

// 1.2K / 4.0M, one decimal, the coreutils `-h` shape. Integer only --
// this kernel has no floating point (-mno-sse), so the tenth is
// computed from the remainder rather than by dividing.
static void put_human(uint32_t n) {
    static const char unit[] = { 'B', 'K', 'M', 'G' };
    int u = 0;
    uint32_t whole = n, rem = 0;
    while (whole >= 1024 && u < 3) {
        rem = whole % 1024;
        whole /= 1024;
        u++;
    }
    char buf[16];
    if (u == 0) snprintf(buf, sizeof buf, "%u", whole);
    else if (whole >= 10) snprintf(buf, sizeof buf, "%u%c", whole, unit[u]);
    else snprintf(buf, sizeof buf, "%u.%u%c", whole, (rem * 10) / 1024, unit[u]);

    // Right-aligned in a fixed field, like the plain size column.
    for (int i = (int)strlen(buf); i < 6; i++) put(" ");
    put(buf);
}

static void put_size(const struct opts *o, uint32_t n) {
    if (o->human) { put_human(n); return; }
    char buf[16];
    snprintf(buf, sizeof buf, "%u", n);
    for (int i = (int)strlen(buf); i < 10; i++) put(" ");
    put(buf);
}

static void put_timestamp(const struct rtc_time *t) {
    char buf[32];
    snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u",
             t->year, t->month, t->day, t->hour, t->minute, t->second);
    put(buf);
}

// ---- ordering --------------------------------------------------------
//
// Sorting lives in userland/lib/dirsort.c, NOT here, because Notepad's
// file dialog has to produce the same order -- see that header. Default
// is BY NAME, which is new: this used to print whatever order fs_list()
// happened to walk the directory in, so the same directory could list
// differently on two machines.

// ---- one directory ---------------------------------------------------

// The entry buffer is STATIC and there is one of it, which is what
// forces -R below to be a queue rather than recursion: SYS_LISTDIR_MAX
// dirents is far past ring 3's 2 KiB frame budget (USERLAND_CFLAGS), so
// a recursive call could not have its own.
static struct sys_dirent g_entries[SYS_LISTDIR_MAX];

// -R's work list. Breadth-first, because a depth-first walk means
// recursion and recursion means a per-level buffer this program cannot
// afford. The bound is stated rather than assumed: a tree deeper or
// wider than this reports that it stopped.
#define QUEUE_MAX 64
// 64 is FS_PATH_MAX, the kernel's own limit -- a path longer than this
// cannot name a file here, so nothing is lost by bounding at it. Named
// rather than repeated: it is the queue row width, the snprintf bound
// and the cwd buffer, and those three must agree.
#define LS_PATH_MAX 64

static char g_queue[QUEUE_MAX][LS_PATH_MAX];
static int g_qhead, g_qtail, g_qdropped;

static void queue_push(const char *dir, const char *name) {
    if (g_qtail >= QUEUE_MAX) { g_qdropped++; return; }
    char *dst = g_queue[g_qtail];
    // "/" + name, avoiding the double slash at the root.
    if (dir[1] == '\0' && dir[0] == '/') snprintf(dst, LS_PATH_MAX, "/%s", name);
    else snprintf(dst, LS_PATH_MAX, "%s/%s", dir, name);
    g_qtail++;
}

// Lists one directory. Returns 0 on success, 1 if the path could not be
// read at all.
static int list_one(const struct opts *o, const char *path, int with_header) {
    int64_t count = sys_listdir(path, g_entries, SYS_LISTDIR_MAX);
    if (count < 0) {
        put("ls: cannot access '");
        put(path);
        put("': ");
        put(strerror(sys_errno()));
        put("\n");
        return 1;
    }

    if (with_header) { put("\n"); put(path); put(":\n"); }

    dirsort(g_entries, (int)count,
            o->sort == 'S' ? DIRSORT_SIZE : o->sort == 't' ? DIRSORT_TIME : DIRSORT_NAME,
            o->reverse);

    for (int i = 0; i < (int)count; i++) {
        struct sys_dirent *e = &g_entries[i];

        if (o->recurse && e->is_dir) queue_push(path, e->name);

        if (o->long_form) {
            put(e->is_dir ? "d " : "- ");
            if (e->is_dir) put(o->human ? "      " : "          ");
            else put_size(o, e->size);
            put(" ");
            put_timestamp(&e->modified);
            put("  ");
        }
        put_name(o, e);
        // ONE PER LINE BY DEFAULT, columns only when asked. Real ls
        // columnises to a TERMINAL and prints one per line when its
        // output is a pipe -- and with no isatty() here (see this file's
        // top comment) it cannot tell, so the default is the one that is
        // safe to parse. Making columns the default broke Notepad's
        // dialog test, which reads `ls /` a line at a time, the moment
        // it was tried.
        put(o->columns && !o->long_form ? "  " : "\n");
    }
    if (o->columns && !o->long_form && count > 0) put("\n");

    // A FULL ARRAY MEANS THERE MAY BE MORE. SYS_LISTDIR fills up to its
    // cap and says nothing about what it left, so this is the only place
    // that can notice -- and before it did, `ls` on a directory of 40
    // files listed 32 and stopped silently. See syscall_abi.h.
    if (count == SYS_LISTDIR_MAX) {
        put("ls: ");
        put(path);
        put(": listing truncated at ");
        char buf[16];
        snprintf(buf, sizeof buf, "%u", (unsigned)SYS_LISTDIR_MAX);
        put(buf);
        put(" entries\n");
        return 1;
    }
    return 0;
}

static void usage(void) {
    put("usage: ls [-1aCFhlRrSt] [--color=never|always|auto] [dir]\n"
        "  -l  long form      -C  multi-column     -h  human sizes\n"
        "  -t  newest first   -S  largest first    -r  reverse\n"
        "  -R  recurse        -a  accepted, no-op (no dotfile convention)\n");
}

int main(int argc, char **argv) {
    // The last field is `color`, and its default is AUTO -- resolved
    // here rather than carried as a third state, because everything
    // below only ever asks "colour or not".
    struct opts o = { 0, 0, 0, 0, 0, 'n', sys_isatty(1) };
    // The default, if no positional argument arrives. `/` is the
    // fallback for a caller with no scheduler slot: SYS_GETCWD needs a
    // process, and the legacy `run` loader is not one (see its ABI
    // comment), so a bare `ls` under `run` still lists something rather
    // than failing.
    char cwd[LS_PATH_MAX];
    const char *path = "/";
    if (sys_getcwd(cwd, sizeof cwd) > 0 && cwd[0]) path = cwd;
    int got_path = 0, bad = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-') {
            // The one long option. Compared in full rather than by
            // prefix, so a typo is refused instead of guessed at --
            // this file's own rule about parsers rejecting rather than
            // guessing (CLAUDE.md).
            if (strcmp(a, "--color=never") == 0) o.color = 0;
            else if (strcmp(a, "--color=always") == 0) o.color = 1;
            else if (strcmp(a, "--color=auto") == 0) o.color = sys_isatty(1);
            else if (strcmp(a, "--help") == 0) { usage(); sys_exit(0); }
            else {
                put("ls: unknown option: ");
                put(a);
                put("\n");
                bad = 1;
            }
        } else if (a[0] == '-' && a[1]) {
            for (int j = 1; a[j]; j++) {
                switch (a[j]) {
                case 'l': o.long_form = 1; break;
                case '1': o.columns = 0; break;
                case 'C': o.columns = 1; break;
                case 'h': o.human = 1; break;
                case 'R': o.recurse = 1; break;
                case 'r': o.reverse = 1; break;
                case 't': o.sort = 't'; break;
                case 'S': o.sort = 'S'; break;
                case 'a': break; // accepted, no-op -- no dotfile convention here
                case 'F': break; // accepted: the trailing '/' is unconditional
                default:
                    put("ls: unknown flag\n");
                    bad = 1;
                    break;
                }
            }
        } else if (!got_path) {
            path = a;
            got_path = 1;
        } else {
            put("ls: only one directory at a time\n");
            bad = 1;
        }
    }
    if (bad) { usage(); sys_exit(2); }

    int rc = list_one(&o, path, 0);

    // -R, iteratively. Each directory found is appended and processed in
    // turn, so the walk costs one static buffer however deep the tree is.
    while (o.recurse && g_qhead < g_qtail) {
        const char *next = g_queue[g_qhead++];
        if (list_one(&o, next, 1)) rc = 1;
    }
    if (g_qdropped) {
        put("ls: too many subdirectories -- some were not listed\n");
        rc = 1;
    }

    sys_exit(rc);
}
