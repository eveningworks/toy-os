#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include "syscall_table.h"
#include "fs.h" // FS_PATH_MAX -- struct open_file's name

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
// parallel table per kind: SYS_CLOSE and fd_release_all() only ever
// look at `used` and `owner_pml4`, so they work on a socket or a pipe
// end for free. Socket fds carry no real state yet (no domain/type
// distinction, no transport) -- the `socket` arm of the union below is
// deliberately empty; it exists so a socket slot has *some* member to
// be valid C, and as the obvious place to grow per-socket state once a
// NIC driver exists.
//
// Declared here rather than kept private to syscall_fd.c because three
// files index it: the fd layer itself, SYS_OPEN (fs), and SYS_SPAWN
// resolving a pipe write end (proc).
#define FD_TABLE_SIZE 8
#define FD_BASE 3 // fds 0/1/2 are reserved for stdin/stdout/stderr

enum fd_mode { FD_MODE_READ, FD_MODE_WRITE };
enum fd_kind { FD_KIND_FILE, FD_KIND_SOCKET, FD_KIND_PIPE_R, FD_KIND_PIPE_W };

struct open_file {
    int used;
    uint64_t owner_pml4;
    enum fd_kind kind;
    union {
        struct {
            char name[FS_PATH_MAX];
            enum fd_mode mode;
            uint32_t offset; // read position; unused in write mode (always appends)
        } file;
        struct {
            int unused_placeholder; // no real socket state yet -- see SYS_SOCKET's doc comment
        } socket;
        struct {
            int idx; // index into pipe.c's table
        } pipe;
    };
};

extern struct open_file fd_table[FD_TABLE_SIZE];

int alloc_fd(uint64_t pml4, enum fd_kind kind, int pipe_idx);
struct open_file *fd_lookup(int fd, uint64_t pml4);

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
int sys_socket(struct syscall_ctx *c);
int sys_send(struct syscall_ctx *c);
int sys_recv(struct syscall_ctx *c);
int sys_pipe(struct syscall_ctx *c);

// kernel/fs/fs_syscalls.c -- the path-keyed filesystem calls
int sys_open(struct syscall_ctx *c);
int sys_unlink(struct syscall_ctx *c);
int sys_listdir(struct syscall_ctx *c);
int sys_fs_generation(struct syscall_ctx *c);

// kernel/proc/proc_syscalls.c -- processes, the heap, and time
int sys_exit(struct syscall_ctx *c);
int sys_yield(struct syscall_ctx *c);
int sys_sbrk(struct syscall_ctx *c);
int sys_spawn(struct syscall_ctx *c);
int sys_waitpid(struct syscall_ctx *c);
int sys_kill(struct syscall_ctx *c);
int sys_proc_info(struct syscall_ctx *c);
int sys_ticks(struct syscall_ctx *c);
int sys_monotonic_ns(struct syscall_ctx *c);

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
int sys_set_color(struct syscall_ctx *c);
int sys_poweroff(struct syscall_ctx *c);
int sys_crashtest(struct syscall_ctx *c);

#endif
