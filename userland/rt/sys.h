#ifndef USERLAND_SYS_H
#define USERLAND_SYS_H

#include <stdint.h>
#include <stddef.h>
#include "syscall_abi.h"
#include "errno.h"   // sys_errno()'s values
#include "proc_info.h" // struct proc_info -- sys_proc_info() below
#include "pci.h"     // struct pci_device, for sys_pci_info()
#include "cpuinfo.h" // struct cpu_info, for sys_cpu_info()
#include "setting_abi.h"
#include "query_abi.h"
#include "tty_abi.h"  // struct tty_termios -- the terminal calls below
#include "signal_abi.h" // SIG*, SIG_DFL/SIG_IGN -- sys_kill() takes a signal
#include "crash_abi.h" // struct setting_msg, struct sys_info

// libsys -- typed wrappers for every syscall a ring-3 program can make.
//
// WHY THIS EXISTS: before it, every single userland program carried its
// own copy of
//
//     static inline int64_t syscall2(uint64_t n, uint64_t a, uint64_t b) {
//         int64_t r; __asm__ volatile ("int $0x80" : ...); return r;
//     }
//
// -- the same eight lines, duplicated more than twenty times, with each
// copy free to get the clobber list or the argument registers subtly
// wrong. The syscall ABI is a contract with the kernel; it should be
// written down once.
//
// These are thin ON PURPOSE. Each one is the raw syscall with a name
// and types, no buffering, no errno, no retry logic. A real libc layer
// (Milestone 24) belongs on top of this, not inside it -- keeping the
// two apart is what makes it obvious which calls actually enter the
// kernel.
//
// The return convention is the kernel's own, documented per-call in
// abi/syscall_abi.h: mostly "0 or positive on success, -1 on failure",
// with the exceptions called out there. What the wrappers DO add is the
// error reason -- the kernel returns -ERRNO (abi/errno.h) and each
// wrapper with the -1 contract turns that back into -1 plus a code
// readable through sys_errno(). Call sites are unchanged by that; a
// caller that wants to know WHY simply has somewhere to ask now.

// --- errors ----------------------------------------------------------

// The reason the last failed syscall gave, as an errno value from
// abi/errno.h -- 0 if nothing has failed yet.
//
// READ IT ONLY AFTER A CALL REPORTED FAILURE. It is not cleared on
// success (POSIX's rule), so a stale value from an earlier failure is
// still sitting here after a hundred successful calls.
//
// WHICH CALLS SET IT: every wrapper below whose failure value is -1.
// The ones that do NOT are the ones where -1 is not a failure --
// sys_read_key() and sys_gui_poll_key() return -1 for "no key waiting"
// -- and the handful whose failure value is 0 rather than -1
// (sys_unlink, sys_kill, sys_gettime, sys_proc_info, sys_win_create),
// which cannot carry a negative code without a caller-visible flip and
// so still cannot say why. sys_sbrk() is the exception in the other
// direction: it keeps returning (void *)-1 and sets this to ENOMEM.
int sys_errno(void);

// The message for a code -- what a program prints when it has to tell a
// person. `sys_strerror(sys_errno())` is the whole idiom. An unrecognised
// code comes back as "unknown error <n>" rather than a shrug, in a static
// buffer the next call overwrites (POSIX permits exactly that).
//
// This is what userland/lib/string.h's strerror() calls, so there is one
// table rather than a libc copy that can drift from it.
const char *sys_strerror(int e);

// --- the raw escape hatch --------------------------------------------

// The syscall instruction itself, with a number and three arguments.
//
// Every typed wrapper below is one line on top of this. It stays public
// for the diagnostic binaries in /tests, which exist precisely to poke
// the raw interface -- write_bad_test.c hands the kernel a deliberately
// invalid pointer, newsyscalls_test.c walks syscall numbers -- and for
// which a typed wrapper would be an obstacle rather than a convenience.
//
// ORDINARY PROGRAMS SHOULD NOT CALL THIS. If a real program needs a
// syscall that has no wrapper here, the fix is to add the wrapper.
int64_t sys_call(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

// --- process ---------------------------------------------------------

void sys_exit(int code) __attribute__((noreturn));

// Cooperatively give up the rest of this timeslice.
void sys_yield(void);

// --- console and files -----------------------------------------------

// fd 1/2 are the console; fd >= 3 come from sys_open().
int64_t sys_write(int fd, const void *buf, size_t len);
int64_t sys_read(int fd, void *buf, size_t len);
int     sys_open(const char *path, int flags); // SYS_O_* flags
int     sys_close(int fd);

// Descriptor plumbing. fds 0/1/2 are ordinary descriptors that merely
// start out on the console and the kernel log, so they redirect like
// any other -- see SYS_DUP2 in abi/syscall_abi.h for the shell dance
// this enables without a fork().
int     sys_dup(int fd);            // lowest free fd naming the same stream
int     sys_dup2(int oldfd, int newfd); // newfd names it too; returns newfd
int     sys_unlink(const char *path);
int     sys_listdir(const char *path, struct sys_dirent *out, int max);

// THE CURRENT DIRECTORY IS THE KERNEL'S, and every path above resolves
// against it -- so a relative path means the same thing here as at any
// shell, and a spawned child starts where its parent was standing. It
// begins at "/" and is inherited across sys_spawn().
//
// This is what makes a /bin program a real command rather than something
// only usable with absolute paths: `mkdir docs` run from /tmp creates
// /tmp/docs because the KERNEL joined it, not because some shell rewrote
// the argument first.
int     sys_chdir(const char *path);
// Fills `buf` with the cwd and returns its length, or -1 (errno ERANGE)
// if it would not fit -- never a truncated path, which names a different
// directory rather than being a shorter answer.
int     sys_getcwd(char *buf, unsigned long cap);

int     sys_mkdir(const char *path);
int     sys_rename(const char *oldpath, const char *newpath);
int     sys_truncate(const char *path, unsigned long long size);
int     sys_stat(const char *path, struct sys_stat *out);

// The fd's position, moved. `whence` is SYS_SEEK_SET/CUR/END. Returns
// the NEW position, or -1 with errno ESPIPE on a console, a pipe or a
// socket -- which is what a buffered stdio turns into fseek()'s failure
// on an unseekable stream rather than pretending it worked.
//
// Seeking PAST the end is legal and is not an error: a following write
// zero-fills the gap and a read there returns 0. There is no sys_tell()
// -- sys_lseek(fd, 0, SYS_SEEK_CUR) is it, as in every Unix.
long long sys_lseek(int fd, long long offset, int whence);

// stat() for an OPEN fd, filling the same struct. What it adds over the
// path-keyed one is `flags`: SYS_STAT_TTY (choose line buffering) and
// SYS_STAT_SEEKABLE (fseek will work). For a console, a pipe or a
// socket the size and timestamps are zero, honestly -- there is no
// length for a pipe to have.
int     sys_fstat(int fd, struct sys_stat *out);

// The caller's own pid, or -1 for a caller with no scheduler slot (the
// legacy `run` loader). It exists because SYS_PROC_INFO is indexed by
// table SLOT, so a process had no way to find its own row -- which is
// what clock() needs to read its own cpu_ns.
int     sys_getpid(void);

// --- the environment --------------------------------------------------

// This process's environment, as a NULL-terminated array of "KEY=VALUE"
// strings. Set by crt0 from the initial stack; NULL only if crt0 was
// bypassed.
//
// It lives here rather than in tolibc because libsys owns the whole
// startup vector -- crt0 IS this layer, and argc/argv/envp arrive
// together. tolibc's <stdlib.h> getenv()/setenv() are the C API over
// this same pointer.
extern char **environ;
// A second NAME for an existing file. -1 with errno EPERM when the
// mounted filesystem's format has no link counts, which is a property
// of the volume rather than of these two paths.
int     sys_link(const char *existing, const char *newpath);
// Flushes the disk write-back cache. Returns the number of SECTORS
// written (0 is a real answer -- nothing was pending), or -1 with errno
// EIO if some could NOT be written, which is the one disk answer a
// caller must not read as success: that data is in RAM only.
int     sys_sync(void);

// Convenience over sys_write(): writes a NUL-terminated string to
// stdout. The one wrapper here that is not a bare syscall, because
// "print this string" is what nearly every caller actually wants and
// hand-rolling a strlen at each call site is noise.
int64_t sys_print(const char *s);

// The same, to STDERR -- which the kernel routes to the kernel log
// (serial console + `dmesg`), never into a parent's stdout pipe.
//
// Use this for anything diagnostic. Two reasons it is not just
// sys_print(): a program whose stdout has been redirected would
// otherwise corrupt the parent's data with its own chatter, and a GUI
// client has no terminal attached at all, so sys_print() from one goes
// to whatever sink the console happens to have. Diagnostics on stderr
// are readable either way.
int64_t sys_eprint(const char *s);

// --- input and time --------------------------------------------------

// Non-blocking: returns the next queued key, or -1 if none. See
// syscall_abi.h for why there is no blocking variant of THIS call --
// use sys_wait_event() instead for anything new.
int sys_read_key(void);

// The console's size in text cells. Returns rows, and stores columns
// through `cols` when it is non-NULL. Never fails.
//
// Ask rather than assume: the console is font-derived here, and
// `font_size` is a runtime setting, so a baked 80x25 is wrong on any
// machine whose font was changed.
int sys_console_size(int *cols);

int sys_gettime(struct rtc_time *out);

// --- memory ----------------------------------------------------------

// Grows the heap by `increment` bytes and returns the OLD break (a
// pointer to that many fresh zeroed bytes), or (void *)-1 on failure.
void *sys_sbrk(int64_t increment);

// --- sockets ---------------------------------------------------------
//
// There is no NIC driver or protocol stack yet, so send/recv always
// fail -- deliberately, see syscall_abi.h. The fd namespace and the ABI
// are real, which is the point of them existing this early.

int sys_socket(int domain, int type);
int64_t sys_send(int fd, const void *buf, size_t len);
int64_t sys_recv(int fd, void *buf, size_t len);

// --- machine info ----------------------------------------------------

int sys_pci_count(void);
int sys_pci_info(int index, struct pci_device *out);
int sys_cpu_info(struct cpu_info *out);

// Fills `buf` with `n` random bytes from the kernel's entropy source.
// Returns n, or -1 for an invalid pointer or an n over
// SYS_GETRANDOM_MAX (4096). Never returns a short count -- it either
// fills the whole buffer or fails, since nothing in the kernel blocks
// waiting for entropy.
//
// The bytes are as good as the machine allows and no better: on a CPU
// without RDSEED/RDRAND they come from timing jitter, which is weak
// under emulation. See kernel/include/api/krandom.h.
int sys_getrandom(void *buf, unsigned long n);

// Reports on process-table SLOT `index` (0 .. SYS_PROC_MAX-1), not on a
// pid -- so a caller can walk the table without knowing which pids
// exist. An empty slot is a SUCCESSFUL call reporting pid 0: skip it,
// do not stop. Returns 1 on success, 0 for a bad index or pointer.
int sys_proc_info(int index, struct proc_info *out);

// The settings and config-file registries (abi/setting_abi.h). ONE call
// with an op field, not one per operation -- see SYS_SETTING's comment.
// Returns 0, or -1 for a bad op or index. Note SETTING_OP_SET reports
// its own three-way outcome in `msg->result`, which a caller must read:
// SETTING_UNSAVED means the change is live but will NOT survive a
// reboot, and reporting that as success is the exact lie this ABI is
// shaped to prevent.
//
// The convenience wrappers below cover the two common cases; anything
// else (enumeration, choices, reload) builds a message directly.
int sys_setting(struct setting_msg *msg);

// Whole-machine memory and disk figures. CPU identity is
// sys_cpu_info(), the PCI count sys_pci_count() and uptime
// sys_monotonic_ns() -- none are duplicated here.
int sys_sysinfo(struct sys_info *out);

// ---- facts (SYS_QUERY) ----------------------------------------------
//
// A FACT is live kernel state, computed on every read and never stored
// -- as opposed to a SETTING, which is persisted and writable. See
// docs/settings-and-queries.md's "The vocabulary".
//
// Four thin wrappers over one syscall. Class 0 (QUERY_PROVIDERS) is the
// registry describing itself, so a program needs to know exactly one
// number to discover every other class -- and a purpose-built command
// that already knows its class skips discovery and asks directly.
int sys_query_record(unsigned cls, unsigned index, void *out, unsigned len);
int sys_query_field_count(unsigned cls);
// Fills `name` (at least QUERY_FIELD_PATH_MAX bytes) and `*out_type`.
int sys_query_field_info(unsigned cls, unsigned index, char *name, unsigned *out_type);
// `qualified` is "<provider>.<field>", e.g. "mem.frame_free". Fails with
// errno ENOTSUP when the class is a LIST and so has no single value --
// which is a different answer from ENOENT ("no such fact").
int sys_query_field_get(const char *qualified, unsigned long long *out_value,
                        unsigned *out_type);

// Terminates `pid` immediately, reporting `exit_code`. Returns 1 if it
// was killed, 0 if there is no such process.
//
// The FORCE path, and unprivileged: any process may kill any other (see
// abi/syscall_abi.h, which explains why there is no permission check
// and what would have to change for there to be one). The polite path
// is the window close handshake, which an app may refuse.
int sys_kill(int pid, int sig);

// --- signals and process groups (abi/signal_abi.h) --------------------
//
// sys_kill() above takes a SIGNAL now, not an exit code -- SIGKILL is
// what its old behaviour is called. A signalled process reports
// SIGNAL_EXIT_BASE + the signal as its exit code, so a Ctrl-C'd program
// exits 130 and a SIGTERM'd one 143, exactly as a Unix shell prints
// them. A NEGATIVE pid names a process GROUP.

// Put `pid` (0 = me) in group `pgid` (0 = the same value as `pid`, i.e.
// lead a new group). Returns 0, or -1 with errno.
//
// You usually do not need this: a child inherits its spawner's group,
// and sys_spawn_group() below is what a shell wants for a pipeline.
// This is for a process naming its OWN group, which nothing else can do.
int sys_setpgid(int pid, int pgid);

// `pid`'s group (0 = me), or -1 with errno ESRCH.
int sys_getpgid(int pid);

// Set this process's disposition for `sig` to SIG_DFL or SIG_IGN.
// Returns the PREVIOUS disposition, or -1 with errno.
//
// NOT `sigaction`: there is no handler, and passing a function pointer
// is refused with EINVAL rather than accepted and never called. SIGKILL
// and SIGQUIT cannot be ignored (EPERM), so there is always something
// that works.
//
// A SHELL SHOULD IGNORE SIGINT. Its own group is in front of the console
// whenever no job is running, and without a handler to redraw a prompt
// there is nothing else it could usefully do with one.
int sys_sigaction(int sig, int disp);

// Put `pgid` in the FOREGROUND of the physical console -- what Ctrl-C
// interrupts. Returns 0, or -1 with errno: EPERM unless this process
// owns the console (it has read fd 0), ENODEV if nobody does, ESRCH for
// a group with no live member.
//
// The shell's half of job control: put the job's group in front, wait
// for it, then put your own back.
//
// `fd` NAMES THE TERMINAL, and it matters which: a shell in a window
// must move ITS terminal's foreground group, not the physical console's
// -- doing the latter would aim the keyboard's Ctrl-C at its child.
// Pass the fd the shell reads its input from, which is 0.
int sys_tcsetpgrp(int fd, int pgid);

// That terminal's foreground group, -1 with errno ENODEV when nobody
// owns it, or ENOTTY when `fd` is not a terminal.
int sys_tcgetpgrp(int fd);

// A new pseudo-terminal: `*master_fd` is the end that BEHAVES like a
// terminal (write to type at it, read what it prints), `*slave_fd` the
// end a program uses as its stdin/stdout. Returns 0, or -1 with errno.
//
// BOTH ENDS AT ONCE AND NO PATH -- there are no device nodes here, so
// this is BSD's openpty(3) rather than opening /dev/ptmx. Hand the slave
// to a child by dup2()ing it onto 0/1/2 before spawning; the child is
// then on a terminal, and Ctrl-C means what it means everywhere else.
int sys_openpty(int *master_fd, int *slave_fd);

// A terminal's behaviour. See abi/tty_abi.h -- two lflags and three
// control characters, not POSIX's four words and 32.
int sys_tcgetattr(int fd, struct tty_termios *tio);
int sys_tcsetattr(int fd, const struct tty_termios *tio);

// Is this fd a TERMINAL? What `--color=auto` and every "am I
// interactive?" decision is made of.
//
// Over SYS_FSTAT's existing SYS_STAT_TTY flag rather than a syscall of
// its own: the kernel already answers "what KIND of thing is this fd",
// and a second call to ask one bit of the same question would be a
// second thing to keep true. The console and both ends of a pty say
// yes; a file, a pipe and a socket say no.
int sys_isatty(int fd);

// How big the terminal is, in CHARACTER CELLS -- what every full-screen
// program asks for first (ioctl(TIOCGWINSZ) elsewhere). The set half is
// for a terminal EMULATOR, which is the only thing that knows how big
// its window is in cells; the physical console derives its own and
// ignores it.
int sys_tcgetwinsz(int fd, struct tty_winsize *ws);
int sys_tcsetwinsz(int fd, const struct tty_winsize *ws);

// A read that would BLOCK returns -1 with errno EAGAIN instead. What a
// program with its own event loop needs when it also has to drain a
// child -- there is no poll() here, so it drains on a tick.
//
// On the DESCRIPTION, so a dup2'd copy shares it: set it on a pty master
// you own, never on a slave you are about to hand to a child.
int sys_set_nonblock(int fd, int on);

// ICANON and ECHO off, ISIG on -- what every shell here wants, since
// they all edit for themselves and none of them wants the kernel
// echoing on top. One call rather than four lines in three shells.
int sys_tty_raw(int fd);

// Monotonic timer TICKS since boot, at whatever rate the timer runs.
// Coarse -- 10ms steps today -- and fine for pacing something, but not
// for measuring: use sys_monotonic_ns() for an interval. Not wall-clock
// either; see sys_gettime() for that.
unsigned long sys_ticks(void);

// Monotonic NANOSECONDS since boot, from the kernel's best clocksource.
// The denominator for a CPU percentage: the numerator is a delta of
// proc_info's cpu_ns and this is a delta of the SAME clock.
//
// The resolution is not promised -- on a CPU with no invariant TSC this
// advances in 10ms steps and two reads inside one tick return the same
// value, so measure across a long enough interval rather than assuming
// nanosecond precision is really there.
unsigned long long sys_monotonic_ns(void);

// The filesystem's GENERATION counter -- bumped on every mutation, by
// anyone. Compare it against the last value you saw to answer "has
// anything changed?" without listing a directory: that is one integer
// compare per frame against real disk I/O, which is why the desktop's
// live `.desktop` reload can afford to ask every frame.
//
// It says something changed, never WHAT -- re-read whatever you cache
// when it moves. Only ever increases, and is non-zero once a filesystem
// is mounted, so 0 is safe as "not sampled yet".
unsigned long long sys_fs_generation(void);

// Powers the machine off (`reboot` = 0) or restarts it (1). DOES NOT
// RETURN on success, so a caller that continues past it should treat
// that as a failure. The disk cache is flushed first either way.
int sys_poweroff(int reboot);

// The kernel's DELIBERATE fault table -- see abi/crash_abi.h. `msg->op`
// picks enumerate or trigger.
//
// CRASH_OP_TRIGGER MAY NOT RETURN: on success the machine has panicked.
// It returns -1 when the kernel is not armed (CRASH_F_ARMED clear in
// the reply), which is the default -- `faultinject` on the GRUB command
// line is what opens the hole.
int sys_crashtest(struct crash_msg *msg);

// Sets the console colours this process writes in. Both are
// `enum vga_color` values; out-of-range is refused, not clamped.
int sys_set_color(int fg, int bg);

// --- the older, modal GUI syscalls -----------------------------------
//
// Superseded by the windowing protocol below for anything new -- these
// map the whole framebuffer (or one kernel-composited buffer) to a
// single process and take over the screen. Kept because
// userland/gui_test.c and win_test.c are what prove those paths still
// work. See syscall_abi.h.

int sys_gui_init(struct gui_info *out);
int sys_gui_poll_key(void);
int sys_win_create(struct win_request *req);
int sys_win_present(void);

// --- processes and pipes ----------------------------------------------

// Creates a pipe. fds[0] is the read end, fds[1] the write end.
// Returns 1, or -1.
int sys_pipe(int fds[2]);

// Runs `path` as a new process. `args` is whitespace-separated
// (NULL for none). `stdout_fd` is a pipe WRITE end from sys_pipe() to
// capture the child's output, or -1 to let it write to the console.
// Returns the child's pid, or -1.
//
// This plus sys_read() on the pipe's read end is how one program runs
// another and reads what it printed -- the thing a terminal does.
int sys_spawn(const char *path, const char *args, int stdout_fd);

// The same, with the child's environment given EXPLICITLY -- pass NULL
// for an empty one. sys_spawn() above is this with `environ`, which is
// exactly the execv()/execve() split: the KERNEL inherits nothing, and
// a library function passing your environment for you is what
// inheritance actually is (abi/syscall_abi.h).
//
// Returns -1 with errno E2BIG if the environment does not fit
// SYS_ENV_MAX -- refused rather than truncated, because a child missing
// half its variables is worse than one that failed to start.
int sys_spawn_env(const char *path, const char *args, int stdout_fd, char **env);

// The same, plus the process GROUP the child starts in -- 0 to inherit
// the caller's, which is what sys_spawn_env() passes.
//
// A SHELL RUNNING A PIPELINE WANTS THIS RATHER THAN setpgid() AFTER THE
// FACT. The child is already running when spawn returns, so a Ctrl-C
// arriving in between would signal the wrong group -- and unlike POSIX,
// where both sides of a fork() call setpgid() to close that window,
// there is no second side here to do it from. Pass the first stage's pid
// as every later stage's `pgid` and the whole pipeline is one group.
int sys_spawn_group(const char *path, const char *args, int stdout_fd,
                     char **env, int pgid);

// BLOCKS until `pid` exits, then reaps it. Writes the exit code to
// `*out_code` if non-NULL. Returns the pid, or -1.
//
// Wraps the kernel's "0 means woken, ask again" retry contract, like
// sys_wait_event() -- the loop lives here rather than in every caller.
int sys_waitpid(int pid, int *out_code);

// Asks whether a child has finished, WITHOUT waiting. Returns the pid
// (reaped, exit code written), SYS_RETRY if it is still running, or -1
// for a pid that is not this caller's live child.
//
// For a caller that must not block on any one child -- a compositor
// checking everything it launched once a frame is the reason this
// exists. sys_waitpid()'s own retry loop is the wrong shape there: it
// hides the "still running" answer, which is exactly the answer such a
// caller wants.
int sys_waitpid_nohang(int pid, int *out_code);

// Waits as sys_waitpid() does, but ALSO returns when the child is
// stopped by SIGSTOP/SIGTSTP. Returns the pid either way; the code is
// SIGNAL_STOP_BASE + the signal for a stop, and SIGNAL_IS_STOP() is the
// test (abi/signal_abi.h).
//
// **A STOP RESULT MEANS THE CHILD IS STILL ALIVE AND UNREAPED.** It
// still holds its slot and its memory, and it will report nothing more
// until it is continued and stopped again -- so a caller that wants to
// keep waiting must resume it first, and one that walks away must
// remember it. This is what /bin/tosh's job table exists for.
int sys_waitpid_untraced(int pid, int *out_code);

// Sleeps for `ms` milliseconds, then returns 0. Returns -1 if the
// caller has no scheduler slot to park in.
//
// Never returns EARLY, and never returns LATE by more than one timer
// tick -- the kernel wakes sleepers from the tick, so that is the
// resolution available. For an idle loop with nothing to wait on; if
// there IS something to wait on (a child, a pipe, an event), block on
// that instead and the process consumes nothing at all.
int sys_sleep_ms(int ms);

// --- windowing -------------------------------------------------------

// One typed message in, one out. See abi/win_proto.h.
int sys_win_request(struct win_request_msg *req);

// TWP's diagnostic channel -- the `gui` commands the test tools drive
// the desktop with. Only the registered compositor may use it; see
// SYS_WIN_DEBUG and WIN_REQ_DEBUG_TAKE.
int sys_win_debug(struct win_debug_msg *msg);

// Non-blocking. 1 if an event was written, 0 if the queue is empty.
int sys_poll_event(struct win_event *out);

// BLOCKS until an event is available, and returns with `*out` filled.
//
// This wraps the kernel's documented "0 means woken, ask again" retry
// contract so callers don't each have to remember it -- the loop is
// here, once, instead of in every client. It does not spin: each pass
// that finds nothing parks the process again in the kernel, using no
// timeslices at all. Returns 1 on success, or -1 if the kernel refused
// (a caller with no event queue -- see syscall_abi.h).
int sys_wait_event(struct win_event *out);

#endif
