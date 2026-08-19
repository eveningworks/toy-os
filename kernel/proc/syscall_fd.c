// The file-descriptor namespace, and every syscall that operates on one.
//
// One table for files, sockets and pipe ends, as in real Unix rather
// than a parallel table per kind: SYS_CLOSE and fd_release_all() only
// ever look at `used` and `owner_pml4`, so a new kind costs them
// nothing. Each entry records which process owns it BY CR3, which is
// what stops one process reading, writing or closing another's fds --
// the same isolation trick the legacy heap and window state use.
//
// SYS_WRITE and SYS_READ live here rather than with the filesystem
// because what they mostly do is ROUTE: a write goes to the console,
// to a pipe, or to a file depending only on the fd. The file-shaped
// ends of each are the two sys_do_*_file() helpers below.
#include "syscalls.h"
#include "syscall_abi.h"
#include "klog.h"
#include "heap.h"  // bounce buffers come from here, not the stack
#include "vga.h"
#include "keyboard.h"
#include "vmm.h"
#include "scheduler.h"
#include "pipe.h"
#include "fs.h"
#include "string.h"
#include <stddef.h>

// The table itself. Its TYPES are in syscalls.h, because SYS_OPEN
// (kernel/fs/fs_syscalls.c) and SYS_SPAWN (proc_syscalls.c) index it
// too; the storage is here, with the code that owns it.

struct open_file fd_table[FD_TABLE_SIZE];

// Which address space holds the console claim, so fd_release_all() can
// tell "this process is dying" from "some other process is". Keyed by
// CR3 like the fd table itself, because that is what the release hook
// is handed.
static uint64_t g_console_owner_pml4;

// Slot allocation and lookup, factored out when pipes needed both and
// found the logic inlined at half a dozen call sites. `fd_lookup()` in
// particular carries the ownership check that keeps one process from
// touching another's fds -- having that written once is worth more than
// the line count saved.
int alloc_fd(uint64_t pml4, enum fd_kind kind, int pipe_idx) {
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].used) continue;
        fd_table[i].used = 1;
        fd_table[i].owner_pml4 = pml4;
        fd_table[i].kind = kind;
        if (kind == FD_KIND_PIPE_R || kind == FD_KIND_PIPE_W) fd_table[i].pipe.idx = pipe_idx;
        return FD_BASE + i;
    }
    return -1;
}

struct open_file *fd_lookup(int fd, uint64_t pml4) {
    int slot = fd - FD_BASE;
    if (slot < 0 || slot >= FD_TABLE_SIZE) return NULL;
    struct open_file *f = &fd_table[slot];
    if (!f->used || f->owner_pml4 != pml4) return NULL;
    return f;
}

// Returns 1 if the caller was PARKED (its syscall has no return value
// yet -- the wake writes it), 0 otherwise. That is the one thing the
// dispatcher still needs to know, so it is the return value rather than
// another out-parameter.
static __attribute__((noinline)) int
sys_do_read_pipe(uint64_t *regs, uint64_t pml4, int pipe_idx,
                 uint64_t buf_ptr, uint64_t len) {
    // pipe_read() fills a kernel buffer, which is then handed out -- it
    // cannot write into the user pointer itself once SMAP is on
    // (vmm.h). len is capped at SYS_WRITE_MAX by the caller. Heap
    // rather than stack, for the reason sys_do_write_console() gives.
    char *kbuf = kmalloc(SYS_WRITE_MAX);
    if (!kbuf) { regs[14] = (uint64_t)-1; return 0; }

    int64_t n = pipe_read(pipe_idx, kbuf, (uint32_t)len);
    int blocked = 0;
    if (n >= 0 && !vmm_copy_to_user(pml4, buf_ptr, kbuf, (uint64_t)n)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)-1;
    } else if (n >= 0) {
        regs[14] = (uint64_t)n; // bytes, or 0 for EOF
    } else if (!scheduler_block_current(regs, SCHED_WAIT_PIPE)) {
        // Nowhere to park (kernel code or the legacy path). Report EOF
        // rather than spinning: a caller that cannot block must not be
        // told "try again forever".
        regs[14] = 0;
    } else {
        blocked = 1;
    }
    kfree(kbuf);
    return blocked;
}

// READING THE PHYSICAL CONSOLE FROM RING 3 -- fd 0.
//
// The counterpart of sys_do_write_console() below, and the reason
// /bin/tosh can exist: a ring-3 shell needs somewhere to read a line
// from, and until this there was nowhere. SYS_READ_KEY is the only
// other way in and is non-blocking BY REQUIREMENT (see its handler in
// win_syscalls.c), so a program wanting a keystroke had to spin.
//
// Blocking here is safe for the reason the pipe path above is safe: the
// handler does not WAIT, it PARKS and returns. keyboard.c's ring_push()
// is the wake site.
//
// THREE THINGS THIS DELIBERATELY DOES NOT DO, all of them the TTY
// milestone's job (docs/roadmap.md):
//   - No line discipline. Bytes arrive as typed; there is no cooked
//     mode, no editing and no echo, so the reader echoes what it reads.
//     That is a program in raw mode, which is what a shell wanting its
//     own editor asks for anyway.
//   - No translation. One byte per key, exactly the code
//     keyboard_try_getchar() returns -- every value this driver
//     produces fits in a byte, specials (KEY_ARROW_* et al, 0x91-0xA6)
//     included. An ANSI escape encoding belongs above a real TTY, not
//     baked in here where it could not be turned off.
//   - No per-terminal queue. There is ONE console, so the first ring-3
//     reader claims it (keyboard.h) and the kernel shell stands down
//     until that process dies.
static __attribute__((noinline)) int
sys_do_read_console(uint64_t *regs, uint64_t pml4, uint64_t buf_ptr, uint64_t len) {
    if (len == 0) { regs[14] = 0; return 0; }

    // A DESKTOP OWNING THE SCREEN OWNS THE KEYBOARD WITH IT. Park
    // without popping anything and without claiming: win_input.c drains
    // this same ring to feed the compositor, so taking a key here would
    // make keystrokes vanish from the desktop at random. The reader
    // simply gets nothing until the desktop exits, which is exactly what
    // the kernel shell behind the desktop already does.
    if (keyboard_compositor_owns()) {
        if (scheduler_block_current(regs, SCHED_WAIT_KEY)) return 1;
        regs[14] = (uint64_t)SYS_RETRY;
        return 0;
    }

    // Claiming on the FIRST read rather than at spawn: a process that
    // never reads the console must not silence the kernel shell, and
    // there is no other moment the kernel could learn the difference.
    keyboard_claim_console(1);
    g_console_owner_pml4 = pml4;

    // Whoever waits for a key is also who flushes the screen -- the
    // console draws into a back buffer and the ring-0 reader's idle loop
    // is what normally presents it (vga.h). That loop is now suspended
    // on this reader's behalf, so this is the moment "output is
    // finished, we are waiting for a human" is true. Cheap when nothing
    // changed.
    vga_present();

    char kbuf[SYS_WRITE_MAX < 64 ? SYS_WRITE_MAX : 64];
    uint64_t want = len < sizeof kbuf ? len : sizeof kbuf;
    uint64_t got = 0;
    while (got < want) {
        int k = keyboard_try_getchar();
        if (k < 0) break;
        kbuf[got++] = (char)(unsigned char)k;
    }

    if (got == 0) {
        // Nothing queued. Park rather than report EOF: a console has no
        // end of file, and 0 would tell a shell its input had closed.
        if (scheduler_block_current(regs, SCHED_WAIT_KEY)) return 1;
        // Nowhere to park -- kernel code, or the legacy loader's single
        // slot. Answer "nothing yet" the only way a non-blocking caller
        // can be answered, rather than lying about end of input.
        regs[14] = (uint64_t)SYS_RETRY;
        return 0;
    }

    if (!vmm_copy_to_user(pml4, buf_ptr, kbuf, got)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)-1;
    } else {
        regs[14] = got;
    }
    return 0;
}

SYSCALL_HANDLER sys_do_write_console(uint64_t *regs, uint64_t pml4, int fd,
                                      uint64_t buf_ptr, uint64_t len) {
    // len is capped at SYS_WRITE_MAX by the caller, so the
    // bounce buffer is always big enough. Copying first (rather
    // than reading through the user pointer as this used to) is
    // what SMAP requires -- see vmm.h.
    // From the HEAP, not the stack: a KiB of bounce buffer at the top of
    // a syscall is a KiB of a 16 KiB per-process kernel stack, and this
    // one sits above pipe_write() and the console. A write already
    // costs far more than an allocation. Same call as the one
    // etc_config_file.c makes, and for the same reason.
    char *kbuf = kmalloc(SYS_WRITE_MAX);
    if (!kbuf) { regs[14] = (uint64_t)-1; return; }
    if (!vmm_copy_from_user(pml4, kbuf, buf_ptr, len)) {
        klog_write("syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)-1; // simplified error indicator (no errno yet)
    } else {
        const char *buf = kbuf;
        // stderr goes to the KERNEL LOG, never to the pipe.
        //
        // Redirecting stdout is a request to capture a program's
        // OUTPUT; folding its diagnostics into the same stream
        // corrupts whatever the parent was trying to read, which
        // is exactly why Unix has two descriptors rather than
        // one. The kernel log is the right sink for the second:
        // it reaches the serial console and `dmesg` no matter
        // who spawned the process or where its stdout went, so a
        // GUI client with no terminal attached can still say
        // something a test (or a person) can read -- the same
        // path strace's lines take.
        if (fd == 2) {
            for (uint64_t i = 0; i < len; i++) klog_putc(buf[i]);
            regs[14] = len;
        } else {
            // A process spawned with SYS_SPAWN's stdout redirection
            // writes into a pipe instead of the console. That is
            // what lets a parent READ this output; without it every
            // child's stdout goes to whatever sink the console has
            // installed and the parent never sees it.
            int out_pipe = scheduler_stdout_pipe(scheduler_current_pid());
            if (out_pipe >= 0) {
                regs[14] = (uint64_t)pipe_write(out_pipe, buf, (uint32_t)len);
            } else {
                for (uint64_t i = 0; i < len; i++) vga_putc(buf[i]);
                regs[14] = len; // bytes written, back via RAX
            }
        }
    }
    kfree(kbuf);
}

SYSCALL_HANDLER sys_do_write_file(uint64_t *regs, uint64_t pml4, int slot,
                                   uint64_t buf_ptr, uint64_t len) {
    // fs_write() (fs.c) works on NUL-terminated C strings, not
    // explicit-length buffers -- see syscall_abi.h's comment on
    // SYS_OPEN for why. Copy into a NUL-terminated scratch buffer (len
    // is already capped at SYS_WRITE_MAX by the caller, so this is
    // always big enough) before handing it over. The copy is done the
    // one way SMAP permits (vmm.h).
    // Heap, not stack -- see sys_do_write_console(). This one sits
    // directly above the whole TFS3 journal and ATA path, which is the
    // deepest chain in the kernel.
    char *tmp = kmalloc(SYS_WRITE_MAX + 1);
    if (!tmp) { regs[14] = (uint64_t)-1; return; }
    if (!vmm_copy_from_user(pml4, tmp, buf_ptr, len)) {
        klog_write("syscall: write() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)-1;
        kfree(tmp);
        return;
    }
    tmp[len] = '\0';
    fs_write(fd_table[slot].file.name, tmp, 1); // 1 = append
    regs[14] = len;
    kfree(tmp);
}

SYSCALL_HANDLER sys_do_read_file(uint64_t *regs, uint64_t pml4, int slot,
                                  uint64_t buf_ptr, uint64_t len) {
    // fs_read_range() reports 0 both at EOF and on any error (fs.h says
    // so explicitly), which is exactly the behaviour wanted here -- a
    // file deleted mid-read by another shell should read as EOF, not
    // fabricate data or fault. It fills a KERNEL buffer which is then
    // copied out; handing it the user pointer directly is what SMAP
    // forbids (vmm.h). len is capped at SYS_WRITE_MAX by the caller.
    uint32_t off = fd_table[slot].file.offset;
    char *kbuf = kmalloc(SYS_WRITE_MAX);   // heap -- see sys_do_write_console()
    if (!kbuf) { regs[14] = (uint64_t)-1; return; }
    uint32_t n = fs_read_range(fd_table[slot].file.name, off, kbuf, (uint32_t)len);
    if (!vmm_copy_to_user(pml4, buf_ptr, kbuf, n)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        regs[14] = (uint64_t)-1;
        kfree(kbuf);
        return;
    }
    fd_table[slot].file.offset += n;
    regs[14] = n;
    kfree(kbuf);
}

int sys_write(struct syscall_ctx *c) {
    // ABI: RDI = fd (was the buffer pointer before SYS_OPEN/SYS_READ
    // existed -- see the "ABI NOTE" on SYS_WRITE in syscall_abi.h),
    // RSI = buffer pointer, RDX = length.
    int fd = (int)c->a0;
    uint64_t buf_ptr = c->a1;
    uint64_t len = c->a2;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

    // Validate the buffer before touching it -- CR3 is still the
    // calling process's own page tables at this point (a syscall
    // doesn't switch address spaces), so without this check a
    // process could hand the kernel a kernel-only address (still
    // *present*, since PML4 entry 0 is shared with every process --
    // see vmm.h) and get the kernel, running at full privilege, to
    // read it on the process's behalf, even though the process
    // could never legally read that address itself.
    uint64_t pml4 = c->pml4;

    if (fd == 1 || fd == 2) { // stdout / stderr
        sys_do_write_console(c->regs, pml4, fd, buf_ptr, len);
    } else { // a real file, opened via SYS_OPEN
        int slot = fd - FD_BASE;
        // kind check: a socket fd (SYS_SOCKET) reaching here means
        // the caller used the wrong syscall -- SYS_SEND is the only
        // way to write to a socket fd -- so it's rejected the same
        // as any other bad fd, not silently treated as a file.
        if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
            fd_table[slot].kind != FD_KIND_FILE ||
            fd_table[slot].owner_pml4 != pml4 || fd_table[slot].file.mode != FD_MODE_WRITE) {
            klog_write("syscall: write() rejected -- bad fd\n");
            c->regs[14] = (uint64_t)-1;
        } else {
            // fs_write() (fs.c) works on NUL-terminated C strings,
            // not explicit-length buffers -- see syscall_abi.h's
            // comment on SYS_OPEN for why. Copy into a NUL-terminated
            // scratch buffer (len is already capped at
            // SYS_WRITE_MAX, so this is always big enough) before
            // handing it to fs_write(), rather than changing fs.c
            // itself this round. The copy was a manual loop through
            // the user pointer; it is the same copy, done the one way
            // SMAP permits (vmm.h).
            sys_do_write_file(c->regs, pml4, slot, buf_ptr, len);
        }
    }
    return 0;
}

int sys_read(struct syscall_ctx *c) {
    int fd = (int)c->a0;
    uint64_t buf_ptr = c->a1;
    uint64_t len = c->a2;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;

    uint64_t pml4 = c->pml4;

    if (fd == 0) { // stdin -- the physical console
        if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
            klog_write("syscall: read() rejected -- invalid buffer pointer\n");
            c->regs[14] = (uint64_t)-1;
            return 0;
        }
        return sys_do_read_console(c->regs, pml4, buf_ptr, len);
    }

    // A pipe fd reads from the pipe, and BLOCKS when it is empty
    // with a writer still alive. Handled before the file path
    // because the two have nothing in common beyond the fd table.
    struct open_file *pf = fd_lookup(fd, pml4);
    if (pf && pf->kind == FD_KIND_PIPE_R) {
        if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
            klog_write("syscall: read() rejected -- invalid buffer pointer\n");
            c->regs[14] = (uint64_t)-1;
            return 0;
        }
        // The one handler that can PARK its caller and still be a plain
        // read: its return value is this function's.
        return sys_do_read_pipe(c->regs, pml4, pf->pipe.idx, buf_ptr, len);
    }

    int slot = fd - FD_BASE;
    // Same kind check as SYS_WRITE above -- SYS_RECV is the only
    // way to read from a socket fd.
    if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
        fd_table[slot].kind != FD_KIND_FILE ||
        fd_table[slot].owner_pml4 != pml4 || fd_table[slot].file.mode != FD_MODE_READ) {
        klog_write("syscall: read() rejected -- bad fd\n");
        c->regs[14] = (uint64_t)-1;
    } else if (!vmm_validate_user_range(pml4, buf_ptr, len)) {
        klog_write("syscall: read() rejected -- invalid buffer pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        // fs_read_range(), NOT fs_read(). This used to call
        // fs_read(), which reads the WHOLE file into a kmalloc'd
        // buffer, and then copied out just the `len` bytes at the
        // fd's offset -- so streaming a file cost (file size) of
        // disk reads per call. Fine while the only things ring 3
        // ever opened were a few hundred bytes; quadratic the
        // moment anything real showed up. /bin/lspci reading the
        // 1.6MB pci.ids in 1KB chunks turned that into ~2.6GB of
        // reads and took 35 seconds. With a range read it's ~0.6s.
        //
        // fs_read_range() reports 0 both at EOF and on any error
        // (fs.h says so explicitly), which happens to be exactly
        // the behaviour wanted here -- a file deleted mid-read by
        // another shell should read as EOF, not fabricate data or
        // fault.
        //
        // fs_read_range() fills a KERNEL buffer, which is then
        // copied out -- it used to be handed the user pointer
        // directly, which SMAP forbids (vmm.h). len is capped at
        // SYS_WRITE_MAX above, so the bounce buffer always fits.
        sys_do_read_file(c->regs, pml4, slot, buf_ptr, len);
    }
    return 0;
}

int sys_close(struct syscall_ctx *c) {
    // Kind-agnostic on purpose -- a socket fd (SYS_SOCKET) has no
    // file-specific state to tear down, so the same "just clear
    // `used`" logic that's always worked for file fds already works
    // for socket fds too, with no changes needed here.
    uint64_t pml4 = c->pml4;
    int fd = (int)c->a0;
    int slot = fd - FD_BASE;
    if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
        fd_table[slot].owner_pml4 != pml4) {
        c->regs[14] = (uint64_t)-1;
    } else {
        // A pipe end is reference counted, unlike a file or socket
        // fd: closing the LAST writer is what turns a blocked
        // reader's wait into EOF, so this cannot just clear `used`.
        if (fd_table[slot].kind == FD_KIND_PIPE_R) pipe_close_reader(fd_table[slot].pipe.idx);
        else if (fd_table[slot].kind == FD_KIND_PIPE_W) pipe_close_writer(fd_table[slot].pipe.idx);
        fd_table[slot].used = 0;
        c->regs[14] = 0;
    }
    return 0;
}

int sys_socket(struct syscall_ctx *c) {
    // See syscall_abi.h's SYS_SOCKET doc comment -- domain/type are
    // reserved for future use and must be 0 for now, rejected
    // otherwise so a caller relying on a real value being honored
    // fails loudly today rather than silently once one exists.
    uint64_t pml4 = c->pml4;
    uint64_t domain = c->a0;
    uint64_t type = c->a1;
    if (domain != 0 || type != 0) {
        klog_write("syscall: socket() rejected -- nonzero domain/type (not supported yet)\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        int slot = -1;
        for (int i = 0; i < FD_TABLE_SIZE; i++) {
            if (!fd_table[i].used) { slot = i; break; }
        }
        if (slot < 0) {
            klog_write("syscall: socket() rejected -- fd table full\n");
            c->regs[14] = (uint64_t)-1;
        } else {
            fd_table[slot].kind = FD_KIND_SOCKET;
            fd_table[slot].used = 1;
            fd_table[slot].owner_pml4 = pml4;
            c->regs[14] = (uint64_t)(FD_BASE + slot);
        }
    }
    return 0;
}

// Both share one body -- same fd validation, same "no transport yet"
// outcome (see syscall_abi.h). Doesn't touch the caller's buffer at all
// (nothing is actually sent/received), so unlike SYS_WRITE/SYS_READ
// there's no buffer pointer to validate here -- only the fd itself.
// The direction is a PARAMETER rather than a re-test of the syscall
// number: with a table there is one entry per number, so a handler that
// asks which one it is has lost information the table already had.
static int send_recv(struct syscall_ctx *c, int is_send) {
    uint64_t pml4 = c->pml4;
    int fd = (int)c->a0;
    int slot = fd - FD_BASE;
    if (slot < 0 || slot >= FD_TABLE_SIZE || !fd_table[slot].used ||
        fd_table[slot].kind != FD_KIND_SOCKET || fd_table[slot].owner_pml4 != pml4) {
        klog_write(is_send ? "syscall: send() rejected -- bad fd\n"
                                    : "syscall: recv() rejected -- bad fd\n");
    } else {
        klog_write(is_send ? "syscall: send() -- no transport yet, failing\n"
                                    : "syscall: recv() -- no transport yet, failing\n");
    }
    c->regs[14] = (uint64_t)-1; // always fails for now -- see syscall_abi.h
    return 0;
}

int sys_send(struct syscall_ctx *c) { return send_recv(c, 1); }
int sys_recv(struct syscall_ctx *c) { return send_recv(c, 0); }

int sys_pipe(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(int) * 2)) {
        klog_write("syscall: pipe() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        int idx = pipe_create();
        int rfd = idx >= 0 ? alloc_fd(pml4, FD_KIND_PIPE_R, idx) : -1;
        int wfd = rfd >= 0 ? alloc_fd(pml4, FD_KIND_PIPE_W, idx) : -1;
        if (idx < 0 || rfd < 0 || wfd < 0) {
            // Unwind rather than leak. A half-made pipe with only
            // one end is worse than none: the caller cannot tell,
            // and would block forever on the end that is missing.
            if (rfd >= 0) fd_table[rfd - FD_BASE].used = 0;
            if (idx >= 0) { pipe_close_reader(idx); pipe_close_writer(idx); }
            klog_write("syscall: pipe() failed -- no free pipe or fd\n");
            c->regs[14] = (uint64_t)-1;
        } else {
            int out[2] = { rfd, wfd };
            vmm_copy_to_user(pml4, c->a0, out, sizeof out); // range validated above
            c->regs[14] = 1;
        }
    }
    return 0;
}

// Everything this process left open. Not part of its address space
// (fs.c and pipe.c are separate kernel resources, nothing about them is
// mapped into the process), so vmm_destroy_address_space() would not
// reclaim any of it.
void fd_release_all(uint64_t pml4_phys) {
    // The console claim is not an fd, but it is the same kind of thing:
    // a kernel resource keyed by address space that nothing else would
    // reclaim. Releasing it here is what brings the kernel shell's
    // prompt back when a ring-3 shell exits OR crashes -- both reach
    // this hook, which is why the claim is not tracked in the reader.
    if (g_console_owner_pml4 == pml4_phys) {
        g_console_owner_pml4 = 0;
        keyboard_claim_console(0);
    }
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].used && fd_table[i].owner_pml4 == pml4_phys) {
            // A pipe end must be RELEASED, not just forgotten: a
            // process that dies holding the last write end is exactly
            // how a reader learns there is no more output coming, and
            // dropping the reference silently would leave that reader
            // blocked forever on a dead writer.
            if (fd_table[i].kind == FD_KIND_PIPE_R) pipe_close_reader(fd_table[i].pipe.idx);
            else if (fd_table[i].kind == FD_KIND_PIPE_W) pipe_close_writer(fd_table[i].pipe.idx);
            fd_table[i].used = 0;
        }
    }
}
