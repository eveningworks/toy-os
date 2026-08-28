#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include "syscall_table.h"
#include "fs.h" // FS_PATH_MAX -- struct open_file's name

struct tty; // kernel/tty.h -- fd_tty() below, without dragging it in here

// Every syscall handler, plus the little that genuinely crosses between
// the files they live in. Included by syscall_table.c (which needs every
// handler's address) and by each handler file (which needs its own
// prototypes to match).
//
// The equivalent in Linux is include/linux/syscalls.h, and for the same
// reason: the table is built somewhere neither the handler nor its
// subsystem can see.

// A helper holding a KiB-sized bounce buffer or a message struct, kept
// out of its handler's frame so the handler pays for it only on the
// path that uses one. `noinline` is not decoration -- each has a single
// caller, so at -O2 GCC folds it straight back in otherwise.
//
// These take `regs` and write regs[14] themselves, which is the syscall
// return value; that is the convention they had as branches of one
// dispatcher, kept so the move stayed a move.
#define SYSCALL_HANDLER static __attribute__((noinline)) void

// --- the file-descriptor table (kernel/proc/syscall_fd.c) ------------
//
// A small global table of open files, sockets and pipe ends. Each entry
// records which process (BY CR3) it belongs to, so one process's fds
// can't be read, written or closed by a different one -- the same
// isolation trick the legacy heap and window state use.
//
// One shared namespace for every kind, as in real Unix, rather than a
// parallel table per kind: a new kind costs close and teardown nothing.
// Socket fds carry no real state yet (no domain/type distinction, no
// transport) -- the `socket` arm of the union below is deliberately
// empty; it exists so a socket slot has *some* member to be valid C,
// and as the obvious place to grow per-socket state once a NIC driver
// exists.
//
// ---- TWO LEVELS, which is the whole design ----
//
// A DESCRIPTION is what a stream IS -- a file, a pipe end, the console.
// A DESCRIPTOR is a number one address space uses to name a
// description. `dup2` copies the NAME, not the stream, so two
// descriptors share one description and the description dies with the
// last of them. That is POSIX's split, and it is what fds 0/1/2 need
// in order to be REDIRECTABLE at all.
//
// Before this, 0/1/2 were not table entries: they were numbers matched
// in an `if`, and "stdout is redirected" was a single `stdout_pipe`
// field on the process, set once at spawn. So there was nowhere for
// `dup2(pipe, 1)` to record itself, and `>` could not be expressed.
// Now they are ordinary descriptors that merely start out pointing at
// the console, and every routing decision is made on the description's
// KIND rather than on the number.
//
// ---- keyed by CR3, not by pid ----
//
// A descriptor table belongs to an ADDRESS SPACE. The legacy blocking
// loader (`run` at the physical shell) has no scheduler slot and
// therefore no pid, so keying this by pid would leave it with no fds
// at all -- the same trap `SYS_SBRK` documents. CR3 is the identifier
// every path has.
#define FD_DESC_MAX  32 // open-file DESCRIPTIONS, shared across dup()
#define FD_MAX       16 // DESCRIPTORS per address space (0..15)
#define FD_SPACE_MAX 24 // address spaces that may hold fds at once

// The three every process starts with. Not magic numbers any more --
// just the descriptors fd_space_open() pre-fills.
#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

enum fd_mode { FD_MODE_READ, FD_MODE_WRITE };

// CONSOLE and KLOG are descriptions like any other, which is what lets
// a descriptor be moved off them. They are kept apart because stdout
// and stderr genuinely differ here: stdout goes to the screen and may
// be redirected into a pipe, while stderr goes to the kernel log so a
// client with no terminal can still say something a test can read.
enum fd_kind {
    FD_KIND_FILE, FD_KIND_SOCKET, FD_KIND_PIPE_R, FD_KIND_PIPE_W,
    FD_KIND_CONSOLE, FD_KIND_KLOG,
    // The two ends of a pseudo-terminal (kernel/pty.h). Two kinds rather
    // than one with a flag, because every switch in syscall_fd.c
    // dispatches on the kind and the two ends do OPPOSITE things: a
    // write to the master is input, a write to the slave is output.
    FD_KIND_TTY_MASTER, FD_KIND_TTY_SLAVE
};

struct open_file {
    int refs; // 0 = free. dup() makes it 2; the last unref tears down.
    enum fd_kind kind;
    // SYS_SET_NONBLOCK: a read that would park returns -EAGAIN instead.
    // On the DESCRIPTION, so a dup2'd copy shares it -- which is what
    // Linux does with O_NONBLOCK and is what makes "set it once on the
    // fd you own" mean something.
    uint8_t nonblock;
    union {
        struct {
            char name[FS_PATH_MAX];
            enum fd_mode mode;
            // THE position, shared by reads and writes, as in POSIX.
            // It was a read-only cursor until SYS_LSEEK existed and
            // writes appended regardless; a position one half of the
            // interface ignores cannot be seeked. 64-bit because
            // fs_read_range()/fs_write_range() take a 64-bit offset --
            // the old uint32_t was a 4 GiB ceiling nothing announced.
            uint64_t pos;
            uint8_t append; // SYS_O_APPEND: every write goes to the end
        } file;
        struct {
            int unused_placeholder; // no real socket state yet -- see SYS_SOCKET's doc comment
        } socket;
        struct {
            int idx; // index into pipe.c's table
        } pipe;
        struct {
            int idx; // index into pty.c's table
        } pty;
    };
};

extern struct open_file fd_desc[FD_DESC_MAX];

// --- descriptions ---
// Allocates one with refs = 1. `aux_idx` is the pipe or pty index for
// the kinds that have one, and ignored for the rest.
int  fd_desc_alloc(enum fd_kind kind, int aux_idx);

// The TERMINAL an fd names, or NULL when it is not one. What
// SYS_TCSETPGRP, SYS_TCGETPGRP and the termios calls resolve first --
// and the one place that knows fd 0 on the console means tty0, so the
// four of them do not each have to.
struct tty *fd_tty(uint64_t pml4, int fd);
// Drops a reference, tearing the description down at zero -- which is
// where a pipe end is closed and a file forgotten. Every close path
// goes through here so there is one place that decides "really gone".
void fd_desc_unref(int di);

// --- descriptors ---
// Gives `pml4` a descriptor table with 0/1 on the console and 2 on the
// kernel log. Idempotent. Called for a process's own address space the
// first time anything asks about its fds.
int  fd_space_open(uint64_t pml4);
// The lowest free descriptor in `pml4` naming description `di`, which
// it takes a reference to. -1 when the table is full.
int  fd_install(uint64_t pml4, int di);
// The description behind one descriptor, or NULL if it is not open.
struct open_file *fd_get(uint64_t pml4, int fd);
// Its index, for callers that need to share it (dup, spawn).
int  fd_desc_index(uint64_t pml4, int fd);
int  fd_close(uint64_t pml4, int fd);
// POSIX's: `newfd` is closed first if open, and dup2(fd, fd) is a
// no-op rather than a close-then-reopen.
int  fd_dup2(uint64_t pml4, int oldfd, int newfd);
// Points one descriptor of `pml4` at description `di`, taking a
// reference and closing whatever was there. The kernel-side half of
// dup2, used by spawn to place a child's stdout before it runs.
int  fd_set_desc(uint64_t pml4, int fd, int di);
// Copies `parent`'s whole descriptor table into `child`, sharing every
// description. What a spawned process inherits, and the reason a shell
// can redirect a child without running code in it.
void fd_inherit(uint64_t child, uint64_t parent);

// --- what a dying process leaves behind ------------------------------
//
// Each of these drops the state its own file keeps for one address
// space, called from syscall_process_exit_cleanup(). Split per file
// because the state is per file: none of it is part of the address
// space, so vmm_destroy_address_space() would not touch any of it.
void fd_release_all(uint64_t pml4_phys);
void proc_syscall_release(uint64_t pml4_phys);
void win_syscall_release(uint64_t pml4_phys);

// --- the handlers ----------------------------------------------------
//
// Grouped by the file they live in. A new syscall is a handler here, a
// prototype in this list, and a row in kernel/proc/syscall_table.c.

// kernel/proc/syscall_fd.c -- the fd namespace and everything on it
int sys_write(struct syscall_ctx *c);
int sys_read(struct syscall_ctx *c);
int sys_close(struct syscall_ctx *c);
int sys_dup(struct syscall_ctx *c);
int sys_dup2(struct syscall_ctx *c);
int sys_socket(struct syscall_ctx *c);
int sys_send(struct syscall_ctx *c);
int sys_recv(struct syscall_ctx *c);
int sys_pipe(struct syscall_ctx *c);

// kernel/fs/fs_syscalls.c -- the path-keyed filesystem calls
int sys_open(struct syscall_ctx *c);
int sys_unlink(struct syscall_ctx *c);
int sys_listdir(struct syscall_ctx *c);
int sys_fs_generation(struct syscall_ctx *c);

// kernel/drivers/partition_syscall.c -- writing a partition table.
int sys_mkpart(struct syscall_ctx *c);
int sys_mount(struct syscall_ctx *c);
int sys_umount(struct syscall_ctx *c);
int sys_chdir(struct syscall_ctx *c);
int sys_getcwd(struct syscall_ctx *c);
int sys_mkdir(struct syscall_ctx *c);
int sys_rename(struct syscall_ctx *c);
int sys_truncate(struct syscall_ctx *c);
int sys_stat(struct syscall_ctx *c);
int sys_lseek(struct syscall_ctx *c);
int sys_fstat(struct syscall_ctx *c);
int sys_link(struct syscall_ctx *c);
int sys_sync(struct syscall_ctx *c);


// kernel/proc/proc_syscalls.c -- processes, the heap, and time
int sys_exit(struct syscall_ctx *c);
int sys_yield(struct syscall_ctx *c);
int sys_sbrk(struct syscall_ctx *c);
int sys_mmap(struct syscall_ctx *c);   // kernel/mm/mmap.c
int sys_munmap(struct syscall_ctx *c); // kernel/mm/mmap.c
int sys_spawn(struct syscall_ctx *c);
int sys_waitpid(struct syscall_ctx *c);
int sys_kill(struct syscall_ctx *c);
// Signals and process groups (kernel/proc/signal_syscalls.c). Beside the
// process syscalls rather than with the signal core, because these are
// the ring-3 SURFACE of it -- the core has kernel callers of its own
// (the keyboard's INTR key) and must not depend on the syscall layer.
int sys_setpgid(struct syscall_ctx *c);
int sys_getpgid(struct syscall_ctx *c);
int sys_sigaction(struct syscall_ctx *c);
int sys_sigreturn(struct syscall_ctx *c);
int sys_tcsetpgrp(struct syscall_ctx *c);
int sys_tcgetpgrp(struct syscall_ctx *c);

// --- terminals (kernel/tty/tty_syscalls.c) ---------------------------
int sys_openpty(struct syscall_ctx *c);
int sys_set_nonblock(struct syscall_ctx *c);
int sys_tcgetwinsz(struct syscall_ctx *c);
int sys_tcsetwinsz(struct syscall_ctx *c);
int sys_tcgetattr(struct syscall_ctx *c);
int sys_tcsetattr(struct syscall_ctx *c);
int sys_proc_info(struct syscall_ctx *c);
int sys_getpid(struct syscall_ctx *c);
int sys_gettid(struct syscall_ctx *c);
int sys_thread_create(struct syscall_ctx *c);
int sys_thread_exit(struct syscall_ctx *c);
int sys_thread_join(struct syscall_ctx *c);
int sys_thread_detach(struct syscall_ctx *c);
int sys_set_tls(struct syscall_ctx *c);
int sys_notify_ready(struct syscall_ctx *c);
int sys_ticks(struct syscall_ctx *c);
int sys_console_size(struct syscall_ctx *c);
int sys_monotonic_ns(struct syscall_ctx *c);
int sys_sleep(struct syscall_ctx *c);

// Registers the heap's demand-paging handler with vmm. Called once from
// kernel_main(); until it runs, a heap page fault is fatal exactly as
// any other unmapped access is.
void uheap_fault_init(void);

// kernel/proc/win_syscalls.c -- windows, events, and the raw keyboard
int sys_gui_init(struct syscall_ctx *c);
int sys_gui_poll_key(struct syscall_ctx *c);
int sys_read_key(struct syscall_ctx *c);
int sys_win_create(struct syscall_ctx *c);
int sys_win_present(struct syscall_ctx *c);
int sys_poll_event(struct syscall_ctx *c);
int sys_wait_event(struct syscall_ctx *c);
int sys_win_request(struct syscall_ctx *c);
int sys_win_debug(struct syscall_ctx *c);

// kernel/core/sys_syscalls.c -- the machine: hardware, settings, power
int sys_gettime(struct syscall_ctx *c);
int sys_pci_count(struct syscall_ctx *c);
int sys_pci_info(struct syscall_ctx *c);
int sys_cpu_info(struct syscall_ctx *c);
int sys_getrandom(struct syscall_ctx *c);
int sys_setting(struct syscall_ctx *c);
int sys_sysinfo(struct syscall_ctx *c);
int sys_query(struct syscall_ctx *c);
int sys_set_color(struct syscall_ctx *c);
int sys_poweroff(struct syscall_ctx *c);
int sys_crashtest(struct syscall_ctx *c);

#endif
