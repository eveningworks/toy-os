// tosh -- the toy-os shell (t + OS + h), running in RING 3.
//
// The kernel has its own shell (apps/shell*.c) with dozens of builtins
// that reach straight into the filesystem, the drivers and the test
// harness. This is deliberately not that, and not a front-end to it
// either: it is an ordinary ring-3 program that does what a shell does
// using only syscalls -- a handful of builtins over the file API, and
// SYS_SPAWN for everything else.
//
// That is the whole point of the exercise. `run foo` in the kernel
// Terminal calls scheduler_spawn() because Terminal IS kernel code;
// here, `foo` is spawned by a process with no more privilege than the
// program it starts, and its output arrives through a pipe like any
// other data.
//
// Structured as a library rather than a program: userland/terminal.c
// links it and feeds it a line at a time, because a GUI terminal owns
// its own event loop and cannot sit in a read() loop of its own. A
// standalone `tosh` binary would be a thin main() over the same calls.
#include "lib/tosh.h"
#include "rt/sys.h"

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int seq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

static void scopy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Joins the shell's cwd and a possibly-relative path. Kept here rather
// than in the caller because every builtin needs it and getting "/" vs
// "/a" vs "a" subtly different per command is exactly how a shell ends
// up with paths that mean different things in different places -- the
// bug kernel/lib/kpath.c exists to have fixed once, on that side.
static void resolve(struct tosh *sh, const char *path, char *out, int cap) {
    if (!path || !path[0]) { scopy(out, sh->cwd, cap); return; }
    if (path[0] == '/') { scopy(out, path, cap); return; }

    int i = 0;
    for (; sh->cwd[i] && i < cap - 2; i++) out[i] = sh->cwd[i];
    if (i > 0 && out[i - 1] != '/') out[i++] = '/';
    for (int j = 0; path[j] && i < cap - 1; j++) out[i++] = path[j];
    out[i] = '\0';
}

static void up_one(char *dir) {
    int n = slen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    while (n > 1 && dir[n - 1] != '/') n--;
    if (n < 1) n = 1;
    dir[n] = '\0';
    if (n > 1 && dir[n - 1] == '/') dir[n - 1] = '\0';
    if (!dir[0]) scopy(dir, "/", TOSH_PATH_MAX);
}

void tosh_init(struct tosh *sh, tosh_out_fn out, void *ctx) {
    scopy(sh->cwd, "/", TOSH_PATH_MAX);
    sh->out = out;
    sh->ctx = ctx;
    sh->last_status = 0;
}

static void emit(struct tosh *sh, const char *s) {
    if (sh->out) sh->out(sh->ctx, s, slen(s));
}

static void emit_int(struct tosh *sh, int v) {
    char b[16];
    int i = 0;
    if (v < 0) { b[i++] = '-'; v = -v; }
    char d[12];
    int n = 0;
    if (v == 0) d[n++] = '0';
    while (v > 0) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) b[i++] = d[--n];
    b[i] = '\0';
    emit(sh, b);
}

// --- builtins ---------------------------------------------------------

static void bi_ls(struct tosh *sh, const char *arg) {
    char path[TOSH_PATH_MAX];
    resolve(sh, arg, path, TOSH_PATH_MAX);

    struct dirent ents[32];
    int n = sys_listdir(path, ents, 32);
    if (n < 0) { emit(sh, "ls: cannot read "); emit(sh, path); emit(sh, "\n"); return; }
    for (int i = 0; i < n; i++) {
        emit(sh, ents[i].name);
        if (ents[i].is_dir) emit(sh, "/");
        emit(sh, "\n");
    }
}

static void bi_cat(struct tosh *sh, const char *arg) {
    if (!arg || !arg[0]) { emit(sh, "cat: needs a filename\n"); return; }
    char path[TOSH_PATH_MAX];
    resolve(sh, arg, path, TOSH_PATH_MAX);

    int fd = sys_open(path, 0);
    if (fd < 0) { emit(sh, "cat: cannot open "); emit(sh, path); emit(sh, "\n"); return; }
    char buf[256];
    for (;;) {
        int64_t n = sys_read(fd, buf, sizeof buf - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        emit(sh, buf);
    }
    sys_close(fd);
}

static void bi_cd(struct tosh *sh, const char *arg) {
    if (!arg || !arg[0]) { scopy(sh->cwd, "/", TOSH_PATH_MAX); return; }
    if (seq(arg, "..")) { up_one(sh->cwd); return; }

    char path[TOSH_PATH_MAX];
    resolve(sh, arg, path, TOSH_PATH_MAX);
    // Verified with a listdir rather than assumed: `cd` onto a file (or
    // onto nothing) silently "succeeding" leaves every later relative
    // path wrong, with nothing pointing at the cd as the cause.
    struct dirent probe[1];
    if (sys_listdir(path, probe, 1) < 0) {
        emit(sh, "cd: no such directory: ");
        emit(sh, path);
        emit(sh, "\n");
        return;
    }
    scopy(sh->cwd, path, TOSH_PATH_MAX);
}

// --- external programs -------------------------------------------------

// Runs `path` with `args`, streaming its output through a pipe into
// this shell's sink. THIS is the part that could not exist before
// SYS_SPAWN/SYS_PIPE: a ring-3 program starting another and reading
// what it prints.
static int run_external(struct tosh *sh, const char *path, const char *args) {
    int fds[2];
    if (sys_pipe(fds) != 1) { emit(sh, "tosh: out of pipes\n"); return -1; }

    int pid = sys_spawn(path, args, fds[1]);
    if (pid < 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        return -1;
    }

    // Close OUR write end. The child holds its own copy, so this does
    // not end the stream -- but leaving it open would mean the read
    // below never sees EOF even after the child exits, because a live
    // writer (us) would still exist. The classic pipe deadlock.
    sys_close(fds[1]);

    char buf[256];
    for (;;) {
        int64_t n = sys_read(fds[0], buf, sizeof buf - 1);
        if (n <= 0) break; // 0 = EOF; the read BLOCKS rather than spinning
        buf[n] = '\0';
        emit(sh, buf);
    }
    sys_close(fds[0]);

    int code = -1;
    sys_waitpid(pid, &code);
    return code;
}

// PATH lookup, in the same order and spirit as the kernel shell's
// (apps/shell_path.c): /bin, then /usr/bin, then /tests. A name
// containing '/' is a path and is used as given.
static const char *const PATH_DIRS[] = { "/bin", "/usr/bin", "/tests" };
#define PATH_DIR_COUNT (int)(sizeof(PATH_DIRS) / sizeof(PATH_DIRS[0]))

static int find_program(const char *name, char *out, int cap) {
    for (const char *p = name; *p; p++) {
        if (*p == '/') { scopy(out, name, cap); return 1; }
    }
    for (int i = 0; i < PATH_DIR_COUNT; i++) {
        int n = 0;
        for (const char *d = PATH_DIRS[i]; *d && n < cap - 2; d++) out[n++] = *d;
        out[n++] = '/';
        for (const char *c = name; *c && n < cap - 1; c++) out[n++] = *c;
        out[n] = '\0';
        // Probing by opening is the only test available: there is no
        // stat syscall yet. A directory would open too, but PATH
        // entries holding a directory named like a command is not a
        // case worth carrying code for.
        int fd = sys_open(out, 0);
        if (fd >= 0) { sys_close(fd); return 1; }
    }
    return 0;
}

int tosh_run_line(struct tosh *sh, const char *line) {
    // Split into command and the rest. Everything after the first space
    // is handed to the program verbatim -- there is no quoting or
    // globbing here, and pretending otherwise would be worse than not
    // having it.
    char cmd[TOSH_PATH_MAX];
    int i = 0;
    while (line[i] == ' ') i++;
    int c = 0;
    while (line[i] && line[i] != ' ' && c < TOSH_PATH_MAX - 1) cmd[c++] = line[i++];
    cmd[c] = '\0';
    while (line[i] == ' ') i++;
    const char *args = line[i] ? line + i : 0;

    if (!cmd[0]) return 0;

    if (seq(cmd, "ls"))   { bi_ls(sh, args);  return 0; }
    if (seq(cmd, "cat"))  { bi_cat(sh, args); return 0; }
    if (seq(cmd, "cd"))   { bi_cd(sh, args);  return 0; }
    if (seq(cmd, "pwd"))  { emit(sh, sh->cwd); emit(sh, "\n"); return 0; }
    if (seq(cmd, "echo")) { if (args) emit(sh, args); emit(sh, "\n"); return 0; }
    if (seq(cmd, "help")) {
        emit(sh, "tosh -- the toy-os shell, running in ring 3.\n"
                 "builtins: ls cat cd pwd echo help\n"
                 "anything else is spawned from /bin, /usr/bin or /tests\n");
        return 0;
    }

    char path[TOSH_PATH_MAX];
    if (!find_program(cmd, path, TOSH_PATH_MAX)) {
        emit(sh, cmd);
        emit(sh, ": not found\n");
        sh->last_status = -1;
        return -1;
    }

    int code = run_external(sh, path, args);
    sh->last_status = code;
    if (code != 0) {
        emit(sh, "[exit ");
        emit_int(sh, code);
        emit(sh, "]\n");
    }
    return code;
}
