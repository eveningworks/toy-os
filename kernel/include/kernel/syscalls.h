#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include "syscall_table.h"
#include "fs.h" // FS_PATH_MAX -- struct open_file's name
#include "scheduler.h"

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
// What a description is, and what it does, comes from its `ops` (struct
// fd_ops below); whatever state that kind keeps per description lives in
// its arm of the union, read only by its own ops.
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
// the console, and every routing decision is made through the
// description's OPS rather than on the number.
//
// ---- keyed by CR3, not by pid ----
//
// A descriptor table belongs to an ADDRESS SPACE. The legacy blocking
// loader (`run` at the physical shell) has no scheduler slot and
// therefore no pid, so keying this by pid would leave it with no fds
// at all -- the same trap `SYS_SBRK` documents. CR3 is the identifier
// every path has.
// THESE ARE CEILINGS, NOT TABLE SIZES. They were 32 / 16 / 24 fixed
// arrays, and on a 1080p desktop that bound long before memory did:
// each client window costs the compositor descriptions and each app is
// another address space, so the desktop stopped being able to open
// ANYTHING at about ten windows -- with the CPU idle, RAM free and 22
// of 64 process slots used, which is what made it so hard to read.
//
// Neither Linux nor NT has a small fixed table. Linux hangs a
// dynamically grown `struct fdtable` off the process and allocates
// `struct file` from a slab, bounding them with RLIMIT_NOFILE per
// process and fs.file-max globally -- the latter computed from RAM at
// boot. NT grows a sparse per-process handle table and bounds it with
// pool quota. Both grow on demand and keep a high ceiling; neither
// fixes a count at compile time.
//
// So: descriptions are allocated one at a time and the table of them
// grows; a descriptor table is allocated per live address space. What
// is left here is the ceiling each one refuses at, which exists for
// the reason fs.file-max does -- one runaway process must not be able
// to spend the kernel heap.
#define FD_DESC_MAX  1024 // open-file DESCRIPTIONS, system-wide ceiling
#define FD_MAX       256  // DESCRIPTORS per address space (0..255)

// The fd-space table grows to the process limit plus a few: an address
// space that can hold fds belongs to a process, plus the legacy `run`
// loader, which has no scheduler slot and is exactly why these are keyed
// by CR3 (syscall_fd.c, spaces_grow()).

// The three every process starts with. Not magic numbers any more --
// just the descriptors fd_space_open() pre-fills.
#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

enum fd_mode { FD_MODE_READ, FD_MODE_WRITE };

struct open_file;
struct syscall_ctx;
struct sys_stat;

// WHAT A DESCRIPTION DOES, as a table -- Linux's `struct file_operations`,
// NT's driver dispatch table. Each kind of stream defines one beside the
// subsystem that owns it (a pipe's in pipe.c, a socket's in
// kernel/net/net_syscalls.c) and the descriptor layer only dispatches, so
// a new kind of stream is a new table and nothing here changes.
//
// **read AND write RETURN 1 WHEN THEY PARKED THE CALLER**, and then the
// syscall has no result yet -- the wake writes RAX (NT's STATUS_PENDING).
// Otherwise they return 0 with the result already in c->regs[14]. Each
// op keeps its own check-and-park dance; the table does not add one.
//
// A NULL slot is an answer, not an omission: no read or write is EBADF
// (the wrong end of a pipe, a log that has no reader), no seek is
// ESPIPE, and no tty() means isatty() is false.
struct fd_ops {
    const char *name;       // what a log line calls this kind
    int  (*read)(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len);
    int  (*write)(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len);
    // fd_desc_alloc()'s `aux` -- the index into the owner's own table --
    // stored wherever this kind keeps it. NULL for a kind that has none.
    void (*open)(struct open_file *f, int aux);
    void (*release)(struct open_file *f); // the LAST reference is gone
    // SYS_LSEEK: the new position, or a negative errno.
    int64_t (*seek)(struct open_file *f, int64_t off, uint64_t whence);
    void (*stat)(struct open_file *f, struct sys_stat *out); // size, if it has one
    struct tty *(*tty)(struct open_file *f); // the terminal, or NULL if hung up
    unsigned flags;
};

// A WRITE TO IT IS INPUT, not output -- a pty master's is typed at the
// terminal. What stops a child's stderr being pointed at one.
#define FD_OPS_WRITE_IS_INPUT 0x1

// Every kind there is, each defined by its owner.
extern const struct fd_ops file_fd_ops;        // kernel/fs/fs_syscalls.c
extern const struct fd_ops pipe_read_fd_ops;   // kernel/proc/pipe.c
extern const struct fd_ops pipe_write_fd_ops;  // kernel/proc/pipe.c
extern const struct fd_ops shm_fd_ops;         // kernel/mm/shm.c
extern const struct fd_ops socket_fd_ops;      // kernel/net/net_syscalls.c
extern const struct fd_ops console_fd_ops;     // kernel/tty/tty_fd.c
extern const struct fd_ops pty_master_fd_ops;  // kernel/tty/tty_fd.c
extern const struct fd_ops pty_slave_fd_ops;   // kernel/tty/tty_fd.c
extern const struct fd_ops tty_fd_ops;         // kernel/tty/tty_fd.c
extern const struct fd_ops klog_fd_ops;        // kernel/core/log_fd.c
extern const struct fd_ops applog_fd_ops;      // kernel/core/log_fd.c

// A kernel buffer for one transfer: asks for `*len` and halves down to a
// 1 KiB floor on a fragmented heap, reporting what it got -- the caller
// then moves less, which every read and write op already handles as a
// short transfer. kfree() it. NULL only below the floor.
void *fd_bounce_alloc(uint64_t *len);

struct open_file {
    int refs; // 0 = free. dup() makes it 2; the last unref tears down.
    const struct fd_ops *ops; // NULL only while free
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
            int idx; // index into kernel/net/socket.c's table
        } socket;
        struct {
            int idx; // index into pipe.c's table
        } pipe;
        struct {
            int idx; // index into pty.c's table
        } pty;
        struct {
            int idx; // index into shm.c's object table
        } shm;
        struct {
            int idx;      // tty_at() index
            unsigned gen; // tty_generation() when opened
        } tty;
    };
};

// ONE DESCRIPTION, BY INDEX. Allocated individually, so a description
// never MOVES -- which is what lets a caller hold the pointer across
// an unrelated fd_desc_alloc(). A grown array would not: the mmap
// region list learned that the expensive way (docs/decisions.md).
// NULL for an index that is out of range or not allocated.
struct open_file *fd_desc_at(int i);

// How many description slots exist right now -- the bound for a walk
// over all of them (mount.c has the only one). Grows; never shrinks.
int fd_desc_count(void);

// --- descriptions ---
// Allocates one with refs = 1. `aux_idx` goes to ops->open -- the pipe,
// pty, shm or tty index for the kinds that have one.
int  fd_desc_alloc(const struct fd_ops *ops, int aux_idx);

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
// Gives `pml4` a descriptor table with 0/1/2 on the console -- or on the
// kernel-context terminal, while one is set. Idempotent. Called for a process's own address space the
// first time anything asks about its fds.
int  fd_space_open(uint64_t pml4);
// THE KERNEL CONTEXT'S TERMINAL: what a table fd_space_open() builds
// names on 0/1/2 while it is set, instead of the console. The serial
// debug console sets it for the length of one command, so the program
// that command runs -- and only it -- is handed the serial terminal.
// NULL restores the console.
void fd_set_kernel_tty(struct tty *t);
struct tty *fd_kernel_tty(void);
int  fd_dup_from(uint64_t pml4, int oldfd, int min);
// -1 queries, 0 clears, 1 sets; returns the flag as it was before.
int  fd_cloexec(uint64_t pml4, int fd, int op);
void fd_close_on_exec(uint64_t pml4);
// The lowest free descriptor in `pml4` naming description `di`. -1 when
// the table is full.
//
// IT TAKES NO REFERENCE. fd_desc_alloc() hands back the one reference
// the description starts with and this only points a descriptor at it,
// so a caller unrefs on the FAILURE path and never on success -- an
// unref after a successful install tears the description down under a
// live descriptor, and the fd then reads as EBADF.
int  fd_install(uint64_t pml4, int di);
// The description behind one descriptor, or NULL if it is not open.
struct open_file *fd_get(uint64_t pml4, int fd);
uint32_t fd_peer_ip(uint64_t pml4);

// kernel/lib/remote_log.c
int sys_remote_log(struct syscall_ctx *c);
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
// A fork's table: the parent's WHOLE table, every description shared.
// Unix's rule, and safe here only because a fork has a child side that
// can close what it must not keep (docs/fork-design.md).
void fd_clone(uint64_t child, uint64_t parent);
// An exec's table: the same descriptors, now keyed by the new address
// space. Nothing is opened or closed.
void fd_rekey(uint64_t old_pml4, uint64_t new_pml4);

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

// kernel/net/net_syscalls.c -- sockets, and configuring the stack
int sys_socket(struct syscall_ctx *c);
int sys_send(struct syscall_ctx *c);
int sys_recv(struct syscall_ctx *c);
int sys_sendto(struct syscall_ctx *c);
int sys_recvfrom(struct syscall_ctx *c);
int sys_bind(struct syscall_ctx *c);
int sys_connect(struct syscall_ctx *c);
int sys_listen(struct syscall_ctx *c);
int sys_accept(struct syscall_ctx *c);
int sys_net_config(struct syscall_ctx *c);
int sys_net_rename(struct syscall_ctx *c);
int sys_net_link(struct syscall_ctx *c);
int sys_input_inject(struct syscall_ctx *c);
int sys_net_resolved(struct syscall_ctx *c);
int sys_net_arp_probe(struct syscall_ctx *c);

// kernel/proc/pipe.c
int sys_pipe(struct syscall_ctx *c);

// kernel/fs/fs_syscalls.c -- the path-keyed filesystem calls
int sys_open(struct syscall_ctx *c);
int sys_unlink(struct syscall_ctx *c);
int sys_listdir(struct syscall_ctx *c);
int sys_listdir_at(struct syscall_ctx *c);
int sys_fs_generation(struct syscall_ctx *c);
int sys_fs_generation_of(struct syscall_ctx *c);

// kernel/drivers/partition_syscall.c -- writing a partition table.
int sys_mkpart(struct syscall_ctx *c);
int sys_install_boot(struct syscall_ctx *c);
int sys_mkfs(struct syscall_ctx *c);
int sys_mount(struct syscall_ctx *c);
int sys_umount(struct syscall_ctx *c);
int sys_fs_check(struct syscall_ctx *c);
int sys_chdir(struct syscall_ctx *c);
int sys_getcwd(struct syscall_ctx *c);

// Copies a path argument out of user memory and resolves it against the
// CALLER'S CWD into `out` (FS_PATH_MAX). Returns 0, or the negative
// errno to hand back. Defined in kernel/fs/fs_syscalls.c.
//
// Every syscall that takes a path must use this, and that includes the
// ones outside the filesystem: spawn and exec take one, and copying it
// raw is what made `./prog` fail from the directory holding it.
int resolve_user_path(uint64_t pml4, uint64_t uaddr, char *out);
int sys_mkdir(struct syscall_ctx *c);
int sys_rename(struct syscall_ctx *c);
int sys_rename2(struct syscall_ctx *c);
int sys_truncate(struct syscall_ctx *c);
int sys_stat(struct syscall_ctx *c);
int sys_lseek(struct syscall_ctx *c);
int sys_fstat(struct syscall_ctx *c);
int sys_link(struct syscall_ctx *c);
int sys_sync(struct syscall_ctx *c);
int sys_fsync(struct syscall_ctx *c);


// kernel/proc/proc_syscalls.c -- processes, the heap, and time
int sys_exit(struct syscall_ctx *c);
int sys_yield(struct syscall_ctx *c);
int sys_sbrk(struct syscall_ctx *c);
int sys_mmap(struct syscall_ctx *c);   // kernel/mm/mmap.c
int sys_munmap(struct syscall_ctx *c); // kernel/mm/mmap.c
int sys_dev_map_bar(struct syscall_ctx *c); // kernel/mm/mmap.c
int sys_dev_claim(struct syscall_ctx *c);   // kernel/drivers/dev_claim.c
int sys_dev_release(struct syscall_ctx *c); // beside it
int sys_dev_dma_alloc(struct syscall_ctx *c); // kernel/mm/mmap.c
int sys_dev_irq_enable(struct syscall_ctx *c); // kernel/drivers/dev_claim.c
int sys_dev_irq_ack(struct syscall_ctx *c);
int sys_dev_io(struct syscall_ctx *c);
int sys_usb_claim(struct syscall_ctx *c);
int sys_usb_release(struct syscall_ctx *c);
int sys_usb_control(struct syscall_ctx *c);
int sys_usb_isoch_open(struct syscall_ctx *c);
int sys_usb_isoch_post(struct syscall_ctx *c);
int sys_usb_isoch_status(struct syscall_ctx *c);    // beside it
int sys_snd_register(struct syscall_ctx *c);  // kernel/drivers/sound/sound_proc.c
int sys_snd_period(struct syscall_ctx *c);
int sys_snd_ring_map(struct syscall_ctx *c);
int sys_setpriority(struct syscall_ctx *c);
int sys_getpriority(struct syscall_ctx *c);    // beside it
// A ring-3 sound driver's registration dies with its address space --
// sound_proc.c.
void sound_proc_space_gone(uint64_t pml4);
int  sound_proc_registered(void);
// SYS_DEV_MAP_BAR's validation, split out so a KTEST can reach it --
// the syscall itself refuses the kernel context at its first line.
int dev_bar_check(int index, int which, uint64_t pml4,
                  uint64_t *phys, uint64_t *npages);
int sys_shm_open(struct syscall_ctx *c);   // kernel/mm/shm.c
int sys_shm_unlink(struct syscall_ctx *c);
int sys_futex_wait(struct syscall_ctx *c);
int sys_kdfile(struct syscall_ctx *c);      // kernel/debug/kdebug_files.c
int sys_futex_wake(struct syscall_ctx *c);
int sys_wakeword(struct syscall_ctx *c);
int sys_shm_grant(struct syscall_ctx *c); // kernel/mm/shm.c
int sys_diag(struct syscall_ctx *c);      // kernel/core/diag.c
int sys_snd_open(struct syscall_ctx *c); // kernel/drivers/sound/sound.c
int sys_snd_ctl(struct syscall_ctx *c);  // kernel/drivers/sound/sound.c
int sys_spawn(struct syscall_ctx *c);
int sys_waitpid(struct syscall_ctx *c);
int sys_kill(struct syscall_ctx *c);
// Signals and process groups (kernel/proc/signal_syscalls.c). Beside the
// process syscalls rather than with the signal core, because these are
// the ring-3 SURFACE of it -- the core has kernel callers of its own
// (the keyboard's INTR key) and must not depend on the syscall layer.
int sys_setpgid(struct syscall_ctx *c);
int sys_setsid(struct syscall_ctx *c);
int sys_getsid(struct syscall_ctx *c);
int sys_getpgid(struct syscall_ctx *c);
int sys_sigaction(struct syscall_ctx *c);
int sys_sigreturn(struct syscall_ctx *c);
int sys_sigprocmask(struct syscall_ctx *c);
int sys_sigsuspend(struct syscall_ctx *c);
int sys_tcsetpgrp(struct syscall_ctx *c);
int sys_tcgetpgrp(struct syscall_ctx *c);

// --- terminals (kernel/tty/tty_syscalls.c) ---------------------------
int sys_openpty(struct syscall_ctx *c);
int sys_set_nonblock(struct syscall_ctx *c);
int sys_dupfd(struct syscall_ctx *c);
int sys_fd_cloexec(struct syscall_ctx *c);
int sys_chmod(struct syscall_ctx *c);
int sys_tcgetwinsz(struct syscall_ctx *c);
int sys_tcsetwinsz(struct syscall_ctx *c);
int sys_tcgetattr(struct syscall_ctx *c);
int sys_tcsetattr(struct syscall_ctx *c);
int sys_proc_info(struct syscall_ctx *c);
int sys_getpid(struct syscall_ctx *c);
int sys_fork(struct syscall_ctx *c);
int sys_exec(struct syscall_ctx *c);
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

// kernel/proc/win_syscalls.c -- the event queue and TWP's carriage
int sys_poll_event(struct syscall_ctx *c);
int sys_wait_event(struct syscall_ctx *c);
int sys_wait_ready(struct syscall_ctx *c);
int sys_fs_watch(struct syscall_ctx *c);      // fswatch.h -- the compositor's path watches
int sys_win_request(struct syscall_ctx *c);

// kernel/core/sys_syscalls.c -- the machine: hardware, settings, power
int sys_gettime(struct syscall_ctx *c);
int sys_settime(struct syscall_ctx *c);
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

// kernel/core/module.c -- loadable modules
int sys_modload(struct syscall_ctx *c);
int sys_modunload(struct syscall_ctx *c);

#endif
