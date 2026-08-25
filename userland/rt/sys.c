// libsys -- see sys.h for why this exists.
//
// The syscall ABI, written down exactly once. Everything below is a
// thin typed shell around one of these three.
#include "rt/sys.h"

// `int $0x80` with the SysV-ish register convention the kernel's
// dispatcher uses: number in RAX, arguments in RDI/RSI/RDX, result in
// RAX (see kernel/proc/syscall.c).
//
// "memory" in the clobber list is not decoration: several of these hand
// the kernel a pointer it writes THROUGH (sys_read, sys_listdir,
// sys_wait_event), and without it the compiler is free to keep a stale
// copy of that memory in a register across the call. Getting this wrong
// in one of the twenty hand-copied versions of this stub would have
// produced a bug visible only under optimisation.
static inline int64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}

static inline int64_t syscall2(uint64_t num, uint64_t a1, uint64_t a2) {
    return syscall3(num, a1, a2, 0);
}

static inline int64_t syscall1(uint64_t num, uint64_t a1) {
    return syscall3(num, a1, 0, 0);
}

static inline int64_t syscall0(uint64_t num) {
    return syscall3(num, 0, 0, 0);
}

// --- errors ----------------------------------------------------------
//
// THE KERNEL RETURNS -ERRNO; THIS IS WHERE IT BECOMES -1 PLUS A REASON.
// A handler that refuses returns the negated error number (abi/errno.h),
// and every wrapper below that has the "-1 on failure" contract runs its
// result through err() -- which records the code and hands the caller
// back the -1 it has always had. So no existing call site changes
// behaviour, and one that wants the reason asks sys_errno().
//
// A GLOBAL, and the caveat that comes with it: this is exactly the
// variable that needs thread-local storage once a process can have two
// threads in a syscall at once (docs/roadmap.md lists TLS and threads).
// It is correct today because there is only ever one thread per process,
// and it is here rather than in the kernel because the RETURNED code is
// the ABI -- see abi/errno.h. When TLS lands, this declaration moves and
// nothing else does.
//
// NOT CLEARED ON SUCCESS, as POSIX specifies: a caller reads it only
// after a call has told it something failed. Clearing it would cost
// every successful syscall a store for the benefit of nobody.
static int g_errno;

int sys_errno(void) { return g_errno; }

// What <errno.h>'s `errno` macro expands through, and the reason it is
// a FUNCTION rather than the variable itself: `errno` has to be an
// lvalue a program can assign to, and exporting a bare global would
// commit this file to a name every ported program also uses. musl does
// exactly this, for the same reason plus a per-thread one that does not
// apply here yet -- with no threads there is one errno, and TLS is not
// a prerequisite for spelling it correctly.
int *__errno_location(void) { return &g_errno; }

// Is this return value an error code rather than a result?
//
// The range test is the whole contract, and it is why ERRNO_MAX is small
// (abi/errno.h): a legitimate result must never land inside it. The one
// call that makes this non-obvious is sbrk(), which returns a POINTER --
// a ring-3 heap address is nowhere near the top of the address space, so
// it cannot be mistaken for one of these. SYS_RETRY sits one past the
// top of the range on purpose and is NOT an error: it means the process
// was woken and must ask again, which the loops below handle.
static int is_err(int64_t r) {
    return r < 0 && r >= -(int64_t)ERRNO_MAX;
}

// Record the reason and give the caller the -1 its contract promises.
static int64_t err(int64_t r) {
    if (is_err(r)) { g_errno = (int)-r; return -1; }
    return r;
}

// The name for a code, for a program that has to tell a person.
//
// A TABLE, not a switch, because the set is data and the compiler puts
// it in .rodata where --gc-sections drops it from any binary that never
// asks (CLAUDE.md's note on how each ELF gets only the members it
// references). The strings are the sentence a user reads, not the POSIX
// macro name: "no such file or directory" beats "ENOENT" at a prompt,
// and the macro name is one grep away for anyone who wants it.
//
// An unknown code is reported AS a number rather than as "unknown
// error", so a value this table has not caught up with is still
// diagnosable from the message alone.
static const struct { int code; const char *msg; } g_errmsg[] = {
    { EPERM,  "operation not permitted" },
    { ENOENT, "no such file or directory" },
    { ESRCH,  "no such process" },
    { EIO,    "input/output error" },
    { EBADF,  "bad file descriptor" },
    { ECHILD, "no child processes" },
    { ENOMEM, "out of memory" },
    { EFAULT, "bad address" },
    { EEXIST, "file exists" },
    { ENODEV, "no such device" },
    { EINVAL, "invalid argument" },
    { ESPIPE, "illegal seek" },
    { E2BIG,  "argument list too long" },
    { ENFILE, "too many open files in system" },
    { EMFILE, "too many open files" },
    { ENOTTY, "not a terminal" },
    { ENOSPC, "no space left" },
    { ENOTDIR, "not a directory" },
    { EISDIR, "is a directory" },
    { ERANGE, "out of range" },
    { ENAMETOOLONG, "path too long" },
    { ENOTSUP, "not a single value" },
    { ENOSYS, "not implemented" },
};

const char *sys_strerror(int e) {
    for (unsigned i = 0; i < sizeof g_errmsg / sizeof g_errmsg[0]; i++)
        if (g_errmsg[i].code == e) return g_errmsg[i].msg;
    if (e == 0) return "no error";

    // Static, so the caller can hold it -- and overwritten by the next
    // call, which is exactly what POSIX allows strerror() to do.
    static char unknown[24];
    static const char pfx[] = "unknown error ";
    unsigned n = 0;
    while (pfx[n]) { unknown[n] = pfx[n]; n++; }
    if (e < 0) { unknown[n++] = '-'; e = -e; }
    char digits[12];
    int d = 0;
    do { digits[d++] = (char)('0' + e % 10); e /= 10; } while (e);
    while (d > 0 && n < sizeof unknown - 1) unknown[n++] = digits[--d];
    unknown[n] = '\0';
    return unknown;
}

int64_t sys_call(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    return syscall3(num, a1, a2, a3);
}

// --- process ---------------------------------------------------------

void sys_exit(int code) {
    syscall1(SYS_EXIT, (uint64_t)(int64_t)code);
    // The kernel does not return from this. The infinite loop is here
    // so the compiler can see the noreturn promise is kept even if it
    // somehow did.
    for (;;) { }
}

void sys_yield(void) { syscall0(SYS_YIELD); }

// --- console and files -----------------------------------------------

int64_t sys_write(int fd, const void *buf, size_t len) {
    // TWO LOOPS, AND THEY ARE FOR DIFFERENT THINGS.
    //
    // The inner one retries SYS_RETRY, which a write to a FULL PIPE
    // returns when the process is woken. Re-sending the whole chunk is
    // correct because a pipe write is all-or-nothing (api/pipe.h): a
    // parked write took none of the bytes, so a retry cannot duplicate
    // them. Without it the caller would see -4095 and treat it as a
    // count.
    //
    // **THE OUTER ONE IS THE ONE THAT WAS MISSING, AND IT LOST DATA
    // SILENTLY.** The kernel caps a single write at SYS_WRITE_MAX
    // (1024 bytes, abi/syscall_abi.h) -- an artefact of the bounce
    // buffer it copies through, not a promise to the caller. A program
    // handing over more than that got 1024 bytes written, a return
    // value saying so, and no indication that the remainder had gone
    // anywhere. Every caller that ignored the count -- which is most of
    // them, because a write to a terminal "cannot fail" -- silently
    // truncated its output at 1 KB.
    //
    // `less` is how it surfaced: a screenful of ~1.5 KB came out as
    // seventeen lines cut mid-word, with the status line (which sits at
    // the END of the frame it builds) never written at all. It looked
    // like a pager bug, and it was in this function.
    //
    // Asking for 2000 bytes means 2000 bytes. A caller that genuinely
    // wants partial-write semantics -- a non-blocking socket, which
    // this system does not have -- would need a different call, and
    // this project's rule against silently truncating a value (see
    // kfmt.h) points the same way.
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len) {
        int64_t r;
        do {
            r = syscall3(SYS_WRITE, (uint64_t)fd,
                          (uint64_t)(uintptr_t)(p + done),
                          (uint64_t)(len - done));
        } while (r == SYS_RETRY);
        // An error, or a kernel that accepted nothing: report it rather
        // than spinning. A caller that has already had some bytes
        // written gets the COUNT, which is what a partial success means
        // and what every read/write pair here does.
        if (r < 0) return done ? (int64_t)done : err(r);
        if (r == 0) break;
        done += (size_t)r;
    }
    return (int64_t)done;
}

int64_t sys_read(int fd, void *buf, size_t len) {
    int64_t r;
    // Loops on SYS_RETRY, which a blocking read (a pipe with no data
    // yet) returns when the process is woken. 0 is NOT the retry
    // signal here -- it is a real end-of-file, which is exactly why
    // SYS_RETRY has its own value. See abi/syscall_abi.h.
    do {
        r = syscall3(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)len);
    } while (r == SYS_RETRY);
    return err(r);
}

int sys_open(const char *path, int flags) {
    return (int)err(syscall2(SYS_OPEN, (uint64_t)(uintptr_t)path, (uint64_t)(int64_t)flags));
}

int sys_close(int fd) { return (int)err(syscall1(SYS_CLOSE, (uint64_t)fd)); }
int sys_dup(int fd) { return (int)err(syscall1(SYS_DUP, (uint64_t)fd)); }
int sys_dup2(int oldfd, int newfd) {
    return (int)err(syscall2(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd));
}

int sys_unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (uint64_t)(uintptr_t)path);
}

int sys_listdir(const char *path, struct sys_dirent *out, int max) {
    return (int)err(syscall3(SYS_LISTDIR, (uint64_t)(uintptr_t)path,
                              (uint64_t)(uintptr_t)out, (uint64_t)(int64_t)max));
}

int sys_chdir(const char *path) {
    return (int)err(syscall1(SYS_CHDIR, (uint64_t)(uintptr_t)path));
}

int sys_getcwd(char *buf, unsigned long cap) {
    return (int)err(syscall2(SYS_GETCWD, (uint64_t)(uintptr_t)buf, (uint64_t)cap));
}

int sys_mkdir(const char *path) {
    return (int)err(syscall1(SYS_MKDIR, (uint64_t)(uintptr_t)path));
}

int sys_rename(const char *oldpath, const char *newpath) {
    return (int)err(syscall2(SYS_RENAME, (uint64_t)(uintptr_t)oldpath,
                              (uint64_t)(uintptr_t)newpath));
}

int sys_truncate(const char *path, unsigned long long size) {
    return (int)err(syscall2(SYS_TRUNCATE, (uint64_t)(uintptr_t)path, (uint64_t)size));
}

int sys_stat(const char *path, struct sys_stat *out) {
    return (int)err(syscall2(SYS_STAT, (uint64_t)(uintptr_t)path,
                              (uint64_t)(uintptr_t)out));
}

long long sys_lseek(int fd, long long offset, int whence) {
    return (long long)err(syscall3(SYS_LSEEK, (uint64_t)fd,
                                    (uint64_t)offset, (uint64_t)whence));
}

int sys_getpid(void) { return (int)syscall0(SYS_GETPID); }
int sys_notify_ready(void) { return (int)err(syscall0(SYS_NOTIFY_READY)); }

// --- the environment --------------------------------------------------

// THE PROCESS'S ENVIRONMENT, set by crt0 from the initial stack.
//
// It lives in libsys rather than in tolibc because libsys already owns
// the whole startup vector: crt0 is this layer, and argc/argv/envp
// arrive together. Putting it here is also what lets sys_spawn() below
// pass it automatically, so every existing caller -- tosh, init, the
// WM -- inherits without being changed.
//
// tolibc's <stdlib.h> getenv/setenv are the C API over this same
// pointer. There is one environment per process and this is it.
char **environ;

// Flattens `environ` into the blob SYS_SPAWN takes: "K=V\0K=V\0\0".
//
// A STATIC buffer, not malloc: libsys sits BELOW the C library and must
// not depend on an allocator that tolibc provides -- a program can link
// libsys alone. The cost is that spawn is not re-entrant, which is
// already true of it for other reasons.
//
// Returns 0 if the environment does not fit, and sys_spawn() then
// refuses rather than passing a truncated one: a child missing half its
// variables is worse than a child that failed to start.
static char g_envblob[SYS_ENV_MAX];

static int flatten_env(char **env) {
    size_t n = 0;
    if (env) {
        for (int i = 0; env[i]; i++) {
            const char *e = env[i];
            size_t len = 0;
            while (e[len]) len++;
            if (n + len + 2 > sizeof g_envblob) return 0;  // + its NUL + the final one
            for (size_t k = 0; k < len; k++) g_envblob[n++] = e[k];
            g_envblob[n++] = '\0';
        }
    }
    if (n + 1 > sizeof g_envblob) return 0;
    g_envblob[n++] = '\0';   // the empty string that ends the run
    return 1;
}

int sys_fstat(int fd, struct sys_stat *out) {
    return (int)err(syscall2(SYS_FSTAT, (uint64_t)fd, (uint64_t)(uintptr_t)out));
}

int sys_link(const char *existing, const char *newpath) {
    return (int)err(syscall2(SYS_LINK, (uint64_t)(uintptr_t)existing,
                              (uint64_t)(uintptr_t)newpath));
}

int sys_sync(void) {
    return (int)err(syscall0(SYS_SYNC));
}

// The message is zeroed before each call rather than partly filled: it
// carries out-fields the kernel writes, and a stale `value` from a
// previous call reading as this call's answer is the kind of bug that
// only shows up once two calls happen in the wrong order.
static void query_msg_init(struct query_msg *m, unsigned op, unsigned cls,
                           unsigned index) {
    for (unsigned i = 0; i < sizeof *m; i++) ((char *)m)[i] = 0;
    m->op = op;
    m->cls = cls;
    m->index = index;
}

int sys_query_record(unsigned cls, unsigned index, void *out, unsigned len) {
    struct query_msg m;
    query_msg_init(&m, QUERY_OP_RECORD, cls, index);
    m.buf = (uint64_t)(uintptr_t)out;
    m.len = len;
    if (err(syscall1(SYS_QUERY, (uint64_t)(uintptr_t)&m)) < 0) return -1;
    return (int)m.returned;
}

int sys_query_field_count(unsigned cls) {
    struct query_msg m;
    query_msg_init(&m, QUERY_OP_FIELD_COUNT, cls, 0);
    if (err(syscall1(SYS_QUERY, (uint64_t)(uintptr_t)&m)) < 0) return -1;
    return (int)m.returned;
}

int sys_query_field_info(unsigned cls, unsigned index, char *name, unsigned *out_type) {
    struct query_msg m;
    query_msg_init(&m, QUERY_OP_FIELD_INFO, cls, index);
    if (err(syscall1(SYS_QUERY, (uint64_t)(uintptr_t)&m)) < 0) return -1;
    for (unsigned i = 0; i < sizeof m.name; i++) name[i] = m.name[i];
    if (out_type) *out_type = m.type;
    return 0;
}

int sys_query_field_get(const char *qualified, unsigned long long *out_value,
                        unsigned *out_type) {
    struct query_msg m;
    query_msg_init(&m, QUERY_OP_FIELD_GET, 0, 0);
    unsigned i = 0;
    for (; qualified && qualified[i] && i < sizeof m.name - 1; i++)
        m.name[i] = qualified[i];
    m.name[i] = 0;
    if (err(syscall1(SYS_QUERY, (uint64_t)(uintptr_t)&m)) < 0) return -1;
    if (out_value) *out_value = m.value;
    if (out_type) *out_type = m.type;
    return 0;
}

int64_t sys_print(const char *s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return sys_write(1, s, n);
}

int64_t sys_eprint(const char *s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return sys_write(2, s, n);
}

// --- input and time --------------------------------------------------

int sys_read_key(void) { return (int)syscall0(SYS_READ_KEY); }

int sys_gettime(struct rtc_time *out) {
    return (int)syscall1(SYS_GETTIME, (uint64_t)(uintptr_t)out);
}

// --- memory ----------------------------------------------------------

void *sys_sbrk(int64_t increment) {
    // KEEPS RETURNING (void *)-1 ON FAILURE, unlike every wrapper above.
    // That value is this call's contract -- POSIX sbrk()'s, and what
    // heap_os.c, ugfx.c and the WM already test against -- so err()'s -1
    // would be right by accident and wrong in type. The kernel refuses
    // with a plain -1 here for the same reason (proc_syscalls.c), so
    // there is no code to record; ENOMEM is the only thing it can mean.
    int64_t r = syscall1(SYS_SBRK, (uint64_t)increment);
    if (r == -1) g_errno = ENOMEM;
    return (void *)(uintptr_t)r;
}

// --- windowing -------------------------------------------------------

int sys_win_request(struct win_request_msg *req) {
    return (int)syscall1(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)req);
}

int sys_poll_event(struct win_event *out) {
    return (int)err(syscall1(SYS_POLL_EVENT, (uint64_t)(uintptr_t)out));
}

int sys_wait_event(struct win_event *out) {
    int64_t r;
    // The kernel's documented retry contract, honoured here once rather
    // than in every client: 0 means "you were woken, ask again", not
    // "no event". See syscall_abi.h for why the kernel cannot hand the
    // event over at wake time. Each pass that finds nothing parks the
    // process again, so this consumes no CPU while waiting.
    do {
        r = syscall1(SYS_WAIT_EVENT, (uint64_t)(uintptr_t)out);
    } while (r == 0);
    return (int)err(r);
}

// --- sockets ---------------------------------------------------------

int sys_socket(int domain, int type) {
    return (int)err(syscall2(SYS_SOCKET, (uint64_t)(int64_t)domain, (uint64_t)(int64_t)type));
}

int64_t sys_send(int fd, const void *buf, size_t len) {
    return err(syscall3(SYS_SEND, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)len));
}

int64_t sys_recv(int fd, void *buf, size_t len) {
    return err(syscall3(SYS_RECV, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)len));
}

// --- machine info ----------------------------------------------------

int sys_pci_count(void) { return (int)syscall0(SYS_PCI_COUNT); }

int sys_pci_info(int index, struct pci_device *out) {
    return (int)err(syscall2(SYS_PCI_INFO, (uint64_t)(int64_t)index, (uint64_t)(uintptr_t)out));
}

int sys_cpu_info(struct cpu_info *out) {
    return (int)err(syscall1(SYS_CPU_INFO, (uint64_t)(uintptr_t)out));
}

int sys_getrandom(void *buf, unsigned long n) {
    return (int)err(syscall2(SYS_GETRANDOM, (uint64_t)(uintptr_t)buf, (uint64_t)n));
}

int sys_proc_info(int index, struct proc_info *out) {
    return (int)syscall2(SYS_PROC_INFO, (uint64_t)(int64_t)index,
                          (uint64_t)(uintptr_t)out);
}

int sys_setting(struct setting_msg *msg) {
    return (int)err(syscall1(SYS_SETTING, (uint64_t)(uintptr_t)msg));
}

int sys_sysinfo(struct sys_info *out) {
    return (int)err(syscall1(SYS_SYSINFO, (uint64_t)(uintptr_t)out));
}

unsigned long sys_ticks(void) {
    return (unsigned long)syscall0(SYS_TICKS);
}

unsigned long long sys_monotonic_ns(void) {
    return (unsigned long long)syscall0(SYS_MONOTONIC_NS);
}

int sys_crashtest(struct crash_msg *msg) {
    return (int)err(syscall1(SYS_CRASHTEST, (uint64_t)(uintptr_t)msg));
}

unsigned long long sys_fs_generation(void) {
    return (unsigned long long)syscall0(SYS_FS_GENERATION);
}

int sys_mkpart(const struct mkpart_request *req) {
    return (int)err(syscall1(SYS_MKPART, (uint64_t)(uintptr_t)req));
}

int sys_win_debug(struct win_debug_msg *msg) {
    return (int)err(syscall1(SYS_WIN_DEBUG, (uint64_t)(uintptr_t)msg));
}

int sys_poweroff(int reboot) {
    return (int)err(syscall1(SYS_POWEROFF, (uint64_t)reboot));
}

int sys_kill(int pid, int exit_code) {
    return (int)syscall2(SYS_KILL, (uint64_t)(int64_t)pid,
                          (uint64_t)(int64_t)exit_code);
}

int sys_set_color(int fg, int bg) {
    return (int)err(syscall2(SYS_SET_COLOR, (uint64_t)(int64_t)fg, (uint64_t)(int64_t)bg));
}

// --- the older, modal GUI syscalls -----------------------------------

int sys_gui_init(struct gui_info *out) {
    return (int)syscall1(SYS_GUI_INIT, (uint64_t)(uintptr_t)out);
}

int sys_gui_poll_key(void) { return (int)syscall0(SYS_GUI_POLL_KEY); }

int sys_win_create(struct win_request *req) {
    return (int)syscall1(SYS_WIN_CREATE, (uint64_t)(uintptr_t)req);
}

int sys_win_present(void) { return (int)err(syscall0(SYS_WIN_PRESENT)); }

// --- processes and pipes ----------------------------------------------

int sys_pipe(int fds[2]) {
    return (int)err(syscall1(SYS_PIPE, (uint64_t)(uintptr_t)fds));
}

int sys_spawn(const char *path, const char *args, int stdout_fd) {
    // Passes `environ`, which is what makes a child inherit. This is
    // execv() to sys_spawn_env()'s execve() -- the kernel inherits
    // nothing, the library does it for you (abi/syscall_abi.h).
    return sys_spawn_env(path, args, stdout_fd, environ);
}

int sys_spawn_env(const char *path, const char *args, int stdout_fd, char **env) {
    // 0 = inherit the caller's group, which is what every caller that
    // does not care about job control wants.
    return sys_spawn_group(path, args, stdout_fd, env, 0);
}

int sys_spawn_group(const char *path, const char *args, int stdout_fd,
                     char **env, int pgid) {
    return sys_spawn_flags(path, args, stdout_fd, env, pgid, 0);
}

int sys_spawn_flags(const char *path, const char *args, int stdout_fd,
                     char **env, int pgid, unsigned flags) {
    if (!flatten_env(env)) { g_errno = E2BIG; return -1; }
    struct spawn_msg msg;
    msg.path = path;
    msg.args = args;
    // An EMPTY environment is still an environment: the blob is the
    // single terminating NUL, and the child gets envp[0] == NULL rather
    // than no envp at all. Passing NULL here would be "no environment",
    // which is what a kernel-side spawner means and not what a program
    // with an empty one means.
    msg.env = g_envblob;
    msg.stdout_fd = stdout_fd;
    msg.pgid = pgid;
    msg.flags = flags;
    return (int)err(syscall1(SYS_SPAWN, (uint64_t)(uintptr_t)&msg));
}


// `pid` may be -1 for "any child of mine" (SYS_WAITPID's ABI comment).
// Note the -1 RETURN then means "no children at all", which is
// permanent -- looping on it waits for something that cannot happen.
int sys_waitpid(int pid, int *out_code) {
    int64_t r;
    // Same retry contract as sys_wait_event(): a 0 return means the
    // process was woken and should ask again, not that the child
    // exited. Each pass that finds it still running parks again, so
    // this consumes no CPU while waiting.
    //
    // **AND -EINTR IS RETRIED HERE RATHER THAN RETURNED**, which became
    // load-bearing the moment the kernel started sending SIGCHLD: a
    // parent with a SIGCHLD handler and no SA_RESTART is woken by its
    // OWN child's death, and returning -1 from the call that was
    // waiting for exactly that would be absurd. The caller asked to
    // wait for a specific child and nothing about a signal changes
    // that, so the wait continues -- which is what an EINTR loop around
    // waitpid() does in every Unix program that installs one.
    //
    // A caller that genuinely wants to be interrupted has
    // sys_waitpid_nohang(); this one blocks until it has an answer.
    do {
        r = syscall3(SYS_WAITPID, (uint64_t)(int64_t)pid,
                     (uint64_t)(uintptr_t)out_code, 0);
    } while (r == SYS_RETRY || r == -EINTR);
    return (int)err(r);
}

// waitpid() that also returns when a child STOPS -- POSIX's WUNTRACED.
//
// A SEPARATE ENTRY POINT rather than a flag on sys_waitpid(), because
// the two have different contracts and the difference is easy to miss:
// this one can return with the child STILL ALIVE and unreaped, and a
// caller that treats every result as an exit would drop a suspended job
// on the floor. SIGNAL_IS_STOP() on the code is how a caller tells.
int sys_waitpid_untraced(int pid, int *out_code) {
    int64_t r;
    // -EINTR retried for the reason sys_waitpid() states: a shell
    // waiting on its foreground job is the process a SIGCHLD is aimed
    // at, and it must not have the wait fail underneath it.
    do {
        r = syscall3(SYS_WAITPID, (uint64_t)(int64_t)pid,
                     (uint64_t)(uintptr_t)out_code, SYS_WUNTRACED);
    } while (r == SYS_RETRY || r == -EINTR);
    return (int)err(r);
}

// Asks whether a child has exited OR stopped, without waiting.
//
// The combination `jobs` needs: a shell about to print a prompt has to
// notice a background job that finished AND one that suspended itself
// (a background reader stopped by SIGTTIN), and must not block on
// either -- there may be nothing to report at all.
int sys_waitpid_check(int pid, int *out_code) {
    return (int)err(syscall3(SYS_WAITPID, (uint64_t)(int64_t)pid,
                             (uint64_t)(uintptr_t)out_code,
                             SYS_WNOHANG | SYS_WUNTRACED));
}

int sys_setpgid(int pid, int pgid) {
    return (int)err(syscall2(SYS_SETPGID, (uint64_t)(int64_t)pid,
                             (uint64_t)(int64_t)pgid));
}

int sys_getpgid(int pid) {
    return (int)err(syscall1(SYS_GETPGID, (uint64_t)(int64_t)pid));
}

int sys_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old) {
    return (int)err(syscall3(SYS_SIGACTION, (uint64_t)(int64_t)sig,
                             (uint64_t)(uintptr_t)act, (uint64_t)(uintptr_t)old));
}

sighandler_t sys_signal(int sig, sighandler_t h) {
    struct k_sigaction act = {
        .handler = (uint64_t)(uintptr_t)h,
        // ONLY WITH A REAL HANDLER. SIG_DFL and SIG_IGN never return
        // anywhere, so a restorer beside one is a field the kernel would
        // have to decide to ignore -- and it does, but saying so twice
        // is how the two copies drift.
        .restorer = SIG_IS_HANDLER(h) ? (uint64_t)(uintptr_t)__sigrestore : 0,
        .flags    = SIG_IS_HANDLER(h) ? SA_RESTART : 0,
    };
    struct k_sigaction old;
    if (sys_sigaction(sig, &act, &old) < 0) return SIG_ERR;
    return (sighandler_t)(uintptr_t)old.handler;
}

int sys_tcsetpgrp(int fd, int pgid) {
    return (int)err(syscall2(SYS_TCSETPGRP, (uint64_t)(int64_t)fd,
                             (uint64_t)(int64_t)pgid));
}

int sys_tcgetpgrp(int fd) {
    return (int)err(syscall1(SYS_TCGETPGRP, (uint64_t)(int64_t)fd));
}

int sys_openpty(int *master_fd, int *slave_fd) {
    struct openpty_msg msg = {0};
    int r = (int)err(syscall1(SYS_OPENPTY, (uint64_t)(uintptr_t)&msg));
    if (r < 0) return r;
    if (master_fd) *master_fd = msg.master_fd;
    if (slave_fd)  *slave_fd  = msg.slave_fd;
    return 0;
}

int sys_tcgetattr(int fd, struct tty_termios *tio) {
    return (int)err(syscall2(SYS_TCGETATTR, (uint64_t)(int64_t)fd,
                             (uint64_t)(uintptr_t)tio));
}

int sys_tcsetattr(int fd, const struct tty_termios *tio) {
    return (int)err(syscall2(SYS_TCSETATTR, (uint64_t)(int64_t)fd,
                             (uint64_t)(uintptr_t)tio));
}

// Raw mode, in one call, because every shell here wants exactly this and
// three copies of the same four lines is how they drift. ICANON and ECHO
// off, ISIG left ON: a shell still wants Ctrl-C to interrupt its jobs --
// it is the LINE EDITING it does for itself (kernel/lib/klineedit.c),
// not the signalling.
//
// Returns 0, or -1 with errno ENOTTY when fd is not a terminal -- which
// a caller may legitimately ignore, since a shell reading a pipe has
// nothing to configure.
int sys_set_nonblock(int fd, int on) {
    return (int)err(syscall2(SYS_SET_NONBLOCK, (uint64_t)(int64_t)fd,
                             (uint64_t)(int64_t)on));
}

int sys_isatty(int fd) {
    struct sys_stat st;
    if (sys_fstat(fd, &st) < 0) return 0;
    return (st.flags & SYS_STAT_TTY) ? 1 : 0;
}

int sys_tcgetwinsz(int fd, struct tty_winsize *ws) {
    return (int)err(syscall2(SYS_TCGETWINSZ, (uint64_t)(int64_t)fd,
                             (uint64_t)(uintptr_t)ws));
}

int sys_tcsetwinsz(int fd, const struct tty_winsize *ws) {
    return (int)err(syscall2(SYS_TCSETWINSZ, (uint64_t)(int64_t)fd,
                             (uint64_t)(uintptr_t)ws));
}

int sys_tty_raw(int fd) {
    struct tty_termios tio;
    if (sys_tcgetattr(fd, &tio) < 0) return -1;
    tio.lflag &= ~(uint32_t)(TTY_ICANON | TTY_ECHO);
    tio.lflag |= TTY_ISIG;
    return sys_tcsetattr(fd, &tio);
}

int sys_sleep_ms(int ms) {
    return (int)err(syscall1(SYS_SLEEP, (uint64_t)(int64_t)ms));
}

int sys_waitpid_nohang(int pid, int *out_code) {
    // NO retry loop, deliberately: SYS_RETRY is the answer here ("still
    // running"), not a signal to ask again. Looping on it is exactly the
    // mistake that turned this call into a block.
    // SYS_RETRY passes through untouched -- err() ignores it, which is
    // what keeps "still running" distinct from a failure here.
    return (int)err(syscall3(SYS_WAITPID, (uint64_t)(int64_t)pid,
                             (uint64_t)(uintptr_t)out_code, SYS_WNOHANG));
}

int sys_console_size(int *cols) {
    // Rows in the low 32 bits, columns in the high 32 -- two small
    // numbers in one return value, so there is no user-memory copy to
    // validate. See SYS_CONSOLE_SIZE in abi/syscall_abi.h.
    uint64_t packed = (uint64_t)sys_call(SYS_CONSOLE_SIZE, 0, 0, 0);
    if (cols) *cols = (int)(packed >> 32);
    return (int)(packed & 0xFFFFFFFFu);
}
