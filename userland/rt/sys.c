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
    return syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)len);
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
    return r;
}

int sys_open(const char *path, int flags) {
    return (int)syscall2(SYS_OPEN, (uint64_t)(uintptr_t)path, (uint64_t)(int64_t)flags);
}

int sys_close(int fd) { return (int)syscall1(SYS_CLOSE, (uint64_t)fd); }

int sys_unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (uint64_t)(uintptr_t)path);
}

int sys_listdir(const char *path, struct dirent *out, int max) {
    return (int)syscall3(SYS_LISTDIR, (uint64_t)(uintptr_t)path,
                          (uint64_t)(uintptr_t)out, (uint64_t)(int64_t)max);
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
    return (void *)(uintptr_t)syscall1(SYS_SBRK, (uint64_t)increment);
}

// --- windowing -------------------------------------------------------

int sys_win_request(struct win_request_msg *req) {
    return (int)syscall1(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)req);
}

int sys_poll_event(struct win_event *out) {
    return (int)syscall1(SYS_POLL_EVENT, (uint64_t)(uintptr_t)out);
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
    return (int)r;
}

// --- sockets ---------------------------------------------------------

int sys_socket(int domain, int type) {
    return (int)syscall2(SYS_SOCKET, (uint64_t)(int64_t)domain, (uint64_t)(int64_t)type);
}

int64_t sys_send(int fd, const void *buf, size_t len) {
    return syscall3(SYS_SEND, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)len);
}

int64_t sys_recv(int fd, void *buf, size_t len) {
    return syscall3(SYS_RECV, (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)len);
}

// --- machine info ----------------------------------------------------

int sys_pci_count(void) { return (int)syscall0(SYS_PCI_COUNT); }

int sys_pci_info(int index, struct pci_device *out) {
    return (int)syscall2(SYS_PCI_INFO, (uint64_t)(int64_t)index, (uint64_t)(uintptr_t)out);
}

int sys_cpu_info(struct cpu_info *out) {
    return (int)syscall1(SYS_CPU_INFO, (uint64_t)(uintptr_t)out);
}

int sys_getrandom(void *buf, unsigned long n) {
    return (int)syscall2(SYS_GETRANDOM, (uint64_t)(uintptr_t)buf, (uint64_t)n);
}

int sys_proc_info(int index, struct proc_info *out) {
    return (int)syscall2(SYS_PROC_INFO, (uint64_t)(int64_t)index,
                          (uint64_t)(uintptr_t)out);
}

int sys_setting(struct setting_msg *msg) {
    return (int)syscall1(SYS_SETTING, (uint64_t)(uintptr_t)msg);
}

int sys_sysinfo(struct sys_info *out) {
    return (int)syscall1(SYS_SYSINFO, (uint64_t)(uintptr_t)out);
}

unsigned long sys_ticks(void) {
    return (unsigned long)syscall0(SYS_TICKS);
}

unsigned long long sys_monotonic_ns(void) {
    return (unsigned long long)syscall0(SYS_MONOTONIC_NS);
}

int sys_crashtest(struct crash_msg *msg) {
    return (int)syscall1(SYS_CRASHTEST, (uint64_t)(uintptr_t)msg);
}

unsigned long long sys_fs_generation(void) {
    return (unsigned long long)syscall0(SYS_FS_GENERATION);
}

int sys_win_debug(struct win_debug_msg *msg) {
    return (int)syscall1(SYS_WIN_DEBUG, (uint64_t)(uintptr_t)msg);
}

int sys_poweroff(int reboot) {
    return (int)syscall1(SYS_POWEROFF, (uint64_t)reboot);
}

int sys_kill(int pid, int exit_code) {
    return (int)syscall2(SYS_KILL, (uint64_t)(int64_t)pid,
                          (uint64_t)(int64_t)exit_code);
}

int sys_set_color(int fg, int bg) {
    return (int)syscall2(SYS_SET_COLOR, (uint64_t)(int64_t)fg, (uint64_t)(int64_t)bg);
}

// --- the older, modal GUI syscalls -----------------------------------

int sys_gui_init(struct gui_info *out) {
    return (int)syscall1(SYS_GUI_INIT, (uint64_t)(uintptr_t)out);
}

int sys_gui_poll_key(void) { return (int)syscall0(SYS_GUI_POLL_KEY); }

int sys_win_create(struct win_request *req) {
    return (int)syscall1(SYS_WIN_CREATE, (uint64_t)(uintptr_t)req);
}

int sys_win_present(void) { return (int)syscall0(SYS_WIN_PRESENT); }

// --- processes and pipes ----------------------------------------------

int sys_pipe(int fds[2]) {
    return (int)syscall1(SYS_PIPE, (uint64_t)(uintptr_t)fds);
}

int sys_spawn(const char *path, const char *args, int stdout_fd) {
    return (int)syscall3(SYS_SPAWN, (uint64_t)(uintptr_t)path,
                          (uint64_t)(uintptr_t)args, (uint64_t)(int64_t)stdout_fd);
}

int sys_waitpid(int pid, int *out_code) {
    int64_t r;
    // Same retry contract as sys_wait_event(): a 0 return means the
    // process was woken and should ask again, not that the child
    // exited. Each pass that finds it still running parks again, so
    // this consumes no CPU while waiting.
    do {
        r = syscall2(SYS_WAITPID, (uint64_t)(int64_t)pid, (uint64_t)(uintptr_t)out_code);
    } while (r == SYS_RETRY);
    return (int)r;
}
