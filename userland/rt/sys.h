#ifndef USERLAND_SYS_H
#define USERLAND_SYS_H

#include <stdint.h>
#include "partition_abi.h" // struct mkpart_request, for sys_mkpart()
#include "mount_abi.h"     // struct mount_request, for sys_mount()
#include "net_abi.h"       // struct net_msg / struct net_ifconfig, for the socket calls
#include <stddef.h>
#include "syscall_abi.h"
#include "errno.h"   // sys_errno()'s values
#include "proc_info.h" // struct proc_info -- sys_proc_info() below
#include "pci.h"     // struct pci_device, for sys_pci_info()
#include "sound_abi.h" // struct snd_register_msg, for sys_snd_register()
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
// WHICH CALLS SET IT: every wrapper below whose failure value is -1 --
// which since the polarity flip is ALL of them. sys_sbrk() is the
// exception in the other direction: it keeps returning (void *)-1 and
// sets this to ENOMEM.
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

// The same, starting at the `start`'th entry. SYS_LISTDIR_MAX caps ONE
// call, so a directory bigger than it needs paging: call with start = 0,
// then start += the count returned, until it returns fewer than `max`.
// A directory being WRITTEN while it is paged can repeat or skip an
// entry -- readdir()'s hazard, and the same answer.
int     sys_listdir_at(const char *path, struct sys_dirent *out, int max, int start);

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
int     sys_rename(const char *oldpath, const char *newpath);   // refuses an existing destination
int     sys_rename2(const char *oldpath, const char *newpath, unsigned flags); // RENAME2_* (syscall_abi.h)
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
// fork(): the child's pid, 0 in the child, -1 with errno (EPERM under
// the legacy loader, EAGAIN with no free slot, ENOMEM). See unistd.h.
int     sys_fork(void);
// execve(): replace this program with `path`. `argv` is NULL-terminated
// (NULL = {path}); `envp` likewise (NULL = an empty environment). Does
// not return on success; -1 with errno otherwise, the program intact.
int     sys_execve(const char *path, char *const argv[], char *const envp[]);

// --- thread-local storage (userland/rt/tls.c) ------------------------
//
// How big one thread's TLS block is, and how to build one in memory the
// caller supplies. <pthread.h> is the only caller: a program gets TLS by
// declaring a `__thread` variable, and crt0 has already installed the
// initial thread's block before main() runs.

// Bytes rt_tls_install() needs, block plus TCB.
uint64_t rt_tls_size(void);

// Lay a thread's TLS out in `mem` (at least rt_tls_size() bytes,
// 16-byte aligned) and return the THREAD POINTER to hand to
// sys_thread_create(). Does not install it -- the block belongs to a
// thread that does not exist yet.
void    *rt_tls_install(void *mem);

// --- threads ---------------------------------------------------------
//
// The raw calls. <pthread.h> is the interface a program should use;
// these are what it is built out of, and what a program that wants a
// thread without a pthread_t can call directly.
//
// **A THREAD SHARES EVERYTHING EXCEPT ITS STACK AND ITS TLS.** Same
// memory, same descriptors, same cwd, same pid -- sys_getpid() answers
// the same value in every thread of a program, and sys_gettid() is what
// tells them apart.

// This THREAD's id, where sys_getpid() is its process's. Equal in a
// program that never creates one.
int     sys_gettid(void);

// Start `entry(arg)` on a stack whose TOP is `stack_top` -- the CALLER
// allocates it, and the kernel never grows it, so it is a fixed extent
// like every pthread stack. `tls` is the new thread's thread pointer
// (NULL for none); `detached` non-zero means nobody will join it.
// Returns the new tid, or -1 with sys_errno() set.
int     sys_thread_create(void (*entry)(void *), void *stack_top, void *arg,
                          void *tls, int detached);

// End the calling thread. From a program's INITIAL thread this exits
// the process instead -- see SYS_THREAD_EXIT in abi/syscall_abi.h.
// Does not return.
void    sys_thread_exit(int code);

// Block until `tid` exits and return its exit code, or -1 with
// sys_errno() set when it is not a joinable thread of this process.
int     sys_thread_join(int tid);

// Say nobody will join `tid`, so its exit frees its slot. 0, or -1.
int     sys_thread_detach(int tid);

// Point this thread's %fs at `base`. userland/rt/tls.c owns the layout
// behind it; a program should not call this directly.
int     sys_set_tls(void *base);

// Announce that this process has finished starting up -- see
// SYS_NOTIFY_READY in abi/syscall_abi.h. Returns 0, or -1 for a caller
// with no scheduler slot.
//
// **ONLY init READS IT**, and only for a service whose descriptor says
// `Ready=notify` (data/etc/services.d/README.md). Calling it from a
// program nobody supervises is harmless and does nothing -- which is
// the systemd property worth copying: sd_notify() in a process started
// from a shell is a no-op, so a program need not know how it was
// started to be correct.
//
// WHERE TO PUT THE CALL is the whole design decision, and it is the
// caller's: it means "somebody can use me now", not "main() has been
// entered". The desktop announces at its FIRST COMPOSITED FRAME, not
// when it claims the compositor role.
int     sys_notify_ready(void);

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
// One file's durability: commits what the filesystem holds back for
// that file's volume and flushes the device under it. Scoped to the
// VOLUME rather than the file -- see SYS_FSYNC's ABI comment.
int     sys_fsync(int fd);

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

// The console's size in text cells. Returns rows, and stores columns
// through `cols` when it is non-NULL. Never fails.
//
// Ask rather than assume: the console is font-derived here, and
// `font_size` is a runtime setting, so a baked 80x25 is wrong on any
// machine whose font was changed.
int sys_console_size(int *cols);

int sys_gettime(struct rtc_time *out);

// Steps the wall clock to `sec` seconds plus `nsec` nanoseconds since
// 1970-01-01 UTC, and writes the RTC. 0 on success, -EINVAL for a time
// out of range. Pass 0 for `nsec` only if you genuinely have no
// sub-second part: dropping it costs up to a second of accuracy.
//
// **UTC**, like sys_gettime() and libc's time() -- a local time a
// person typed goes through mktime() first (abi/syscall_abi.h).
int sys_settime(uint64_t sec, uint32_t nsec);

// --- memory ----------------------------------------------------------

// Grows the heap by `increment` bytes and returns the OLD break (a
// pointer to that many fresh zeroed bytes), or (void *)-1 on failure.
void *sys_sbrk(int64_t increment);

// SYS_MMAP / SYS_MUNMAP, the raw shape. tolibc's <sys/mman.h> is the
// POSIX face over these; the arguments here ARE POSIX's, packed into
// abi/syscall_abi.h's struct mmap_msg at the call. Failure is
// (void *)-1 -- MAP_FAILED, mmap's own contract, the same reasoning as
// sys_sbrk() above -- with the reason in sys_errno(). sys_munmap()
// follows the ordinary 0/-1 rule.
void *sys_mmap(void *addr, uint64_t length, int prot, int flags,
               int fd, uint64_t offset);
int sys_munmap(void *addr, uint64_t length);

// SYS_SHM_OPEN / SYS_SHM_UNLINK: a named shared-memory object, which
// sys_mmap() maps with MAP_SHARED. Returns an fd, or -1 with the reason
// in sys_errno().
//
// NOT SPELLED shm_open(3), and the difference is the SIZE. POSIX's takes
// a mode and sizes the object with a later ftruncate(); here the size is
// fixed at creation, so the POSIX pair is not yet expressible and a
// function with that name would take arguments meaning something else.
// See docs/roadmap.md's IPC track.
int sys_shm_open(const char *name, uint64_t length, int flags);

// Park until somebody changes *word and says so, or `timeout_ms` passes
// (0 = no deadline). Returns 0 when woken, -EAGAIN if *word already
// holds something other than `expected` -- which is the answer, not an
// error: it is what stops a caller parking on a stale read and waiting
// for a wake that already happened.
//
// THE WORD IS NAMED BY ITS FRAME, so two processes sharing an shm page
// meet on one futex at whatever address each of them mapped it.
int sys_futex_wait(volatile uint32_t *word, uint32_t expected, int timeout_ms);

// Release up to `count` waiters on that word (0 = all); returns how
// many were woken. A lock's unlock passes 1.
int sys_futex_wake(volatile uint32_t *word, int count);

// Names ONE word this process waits on for everything: the kernel bumps
// it and wakes it whenever it queues a window/input event, and anything
// sharing the page may do the same. 0 deregisters.
//
// It exists because a futex waits on one word and a compositor has two
// sources -- its event queue and its clients' messages -- with no poll()
// here to wait on both. See SYS_WAKEWORD in abi/syscall_abi.h.
int sys_wakeword(volatile uint32_t *word);

// Lets `pid` open a named object THIS process created. The creator's to
// give: a private object is a channel between two processes, and this is
// how the second one is let in. Returns 0, or -1 with the reason.
int sys_shm_grant(const char *name, int pid);
int sys_shm_unlink(const char *name);

// --- sockets ---------------------------------------------------------
//
// There is no NIC driver or protocol stack yet, so send/recv always
// fail -- deliberately, see syscall_abi.h. The fd namespace and the ABI
// are real, which is the point of them existing this early.

// AF_INET/SOCK_DGRAM with IPPROTO_ICMP or IPPROTO_UDP is the supported combination
// (abi/net_abi.h). Addresses are HOST byte order throughout -- there is
// no htonl() to forget here.
int sys_socket(int domain, int type, int protocol);
int64_t sys_send(int fd, const void *buf, size_t len);   // no peer: -EINVAL
int64_t sys_recv(int fd, void *buf, size_t len);         // no peer: -EINVAL
int64_t sys_sendto(int fd, const void *buf, size_t len, uint32_t dst_ip, uint16_t dst_port);
// BLOCKS until a datagram arrives. `timeout_ms` of 0 waits forever;
// otherwise it is a ceiling, and 0 comes back when it expires -- NOT
// -EAGAIN, because a datagram socket has no end-of-stream for a zero
// to be confused with, and every call site already tests `n > 0`.
// Interruptible: a signal rewinds the call, so Ctrl-C reaches a
// program parked here. sys_set_nonblock() restores the old
// poll-and-return-0 behaviour for a caller that wants it.
int64_t sys_recvfrom(int fd, void *buf, size_t cap, uint32_t *out_src,
                     uint16_t *out_port, unsigned timeout_ms);
// Connect a STREAM socket. Blocks until the handshake finishes;
// `timeout_ms` of 0 uses the kernel's default. Afterwards read() and
// write() work on the fd, as POSIX guarantees -- so code taking a
// descriptor can be handed one.
int sys_connect(int fd, uint32_t ip, uint16_t port, unsigned timeout_ms);
// Make a BOUND stream socket a listener. There is no backlog argument:
// the depth is the stack's, because each queued connection costs a
// whole connection block.
int sys_listen(int fd);
// Wait for a client and return a NEW fd for its connection, with the
// peer's address written back. Blocks; `timeout_ms` of 0 waits forever.
int sys_accept(int fd, uint32_t *out_ip, uint16_t *out_port, unsigned timeout_ms);
// A local port (0 picks an ephemeral one), optionally on ONE device --
// SO_BINDTODEVICE, which is what a DHCP client needs. Returns the port.
int sys_bind(int fd, uint32_t addr, uint16_t port, const char *dev);
// A zero field is left alone, so one address can be changed on its own.
int sys_net_config(const char *dev, uint32_t ip, uint32_t netmask, uint32_t gateway);
// SYS_NET_CONFIG's NET_IFC_* flags alone: clear the address, set the card
// administratively down or up (abi/net_abi.h). netd and netctl's half.
int sys_net_admin(const char *dev, unsigned flags);

// Change a card's adapter settings (abi/net_abi.h's struct net_linkcfg:
// rates, EEE, pause frames, interrupt moderation). 0, or -1 with
// sys_errno(): ENODEV, ENOTSUP for a setting the driver lacks, EINVAL.
// lib/unetlink.h is what a program should use -- it also SAVES them.
struct net_linkcfg;
int sys_net_link(const struct net_linkcfg *req);

// Give an interface a different name. 0, or -ENODEV / -EINVAL.
// Naming POLICY is /bin/netd's -- see /etc/net.conf.
int sys_net_rename(const char *dev, const char *to);

// Does anybody answer for `ip` on `dev`? 1 yes, 0 not yet, -1 with
// sys_errno(). NON-BLOCKING: it puts one ARP request on the wire (at
// most one a second) and reports whether a reply has come back YET, so
// a caller asks repeatedly. That split is deliberate -- RFC 3927's
// probe count and spacing are policy, and policy lives here rather than
// in the kernel. On a device with no address the frame is an ARP Probe;
// on one that has an address it is an ARP Announcement.
int sys_net_arp_probe(const char *dev, uint32_t ip);

// Tell the kernel that `name` resolved to `ip`, so the connection log
// (QUERY_CONNLOG, `/bin/netlog`) can print a name beside the address.
// A REPORT with no answer to read: a program that skips it loses
// nothing but the name in somebody else's log. uresolv_lookup() calls
// it for every caller, so nothing else normally needs to.
int sys_net_resolved(const char *name, uint32_t ip);

// --- machine info ----------------------------------------------------

int sys_pci_count(void);
int sys_pci_info(int index, struct pci_device *out);
// A device's register file, mapped into this process -- stage 1 of
// docs/umdf-design.md. `index` is SYS_PCI_INFO's, `bar` is 0..5.
// Returns the address, or -1 with errno: EACCES when this process has
// not CLAIMED the device, EBUSY when another one has (or a ring-0
// driver still holds it), ENOTSUP for an I/O BAR, EINVAL for a BAR
// with nothing behind it.
int64_t sys_dev_map_bar(int index, int bar);

// Take the device off the kernel -- stage 2. Unbinds its ring-0
// driver, if any, and records this process as the holder; the claim is
// what SYS_DEV_MAP_BAR then requires. Re-claiming is idempotent.
// 0, or -1 with errno: ENOTSUP when the bound driver cannot let go
// (the only gate there is -- see abi/syscall_abi.h), EBUSY when
// another process holds it, EPERM from the legacy `run` loader.
//
// THE CLAIM IS DROPPED IF THIS PROCESS DIES, and the device is then
// left unbound so a restarted driver finds it free.
int sys_dev_claim(int index);

// Give it back. With DEV_RELEASE_REBIND the kernel re-probes the
// device so its ring-0 driver takes it again; without, it stays
// unbound. -1 with EACCES when this process is not the holder.
int sys_dev_release(int index, unsigned flags);

// A pinned, physically contiguous buffer for the device this process
// holds, and THE CALL THAT LETS THAT DEVICE REACH MEMORY AT ALL: it
// raises PCI bus mastering, which drops again when the claim does.
// `bytes` is rounded up to whole pages, at most DEV_DMA_MAX_BYTES, and
// `*phys` receives the physical address to program the card with.
// Returns the virtual address, or -1 with errno: EACCES when this
// process does not hold the device, EBUSY when it already has a
// buffer, ENOMEM when no contiguous run that long is free.
//
// The mapping is uncacheable; munmap does NOT free the frames, because
// they belong to the claim and the device has to be stopped first.
int64_t sys_dev_dma_alloc(int index, uint64_t bytes, uint64_t *phys);

// Route this device's interrupt to THIS PROCESS's wakeword -- so park
// in sys_futex_wait() on the word registered with sys_wakeword(), and
// the kernel bumps it when the device fires. The line is MASKED until
// sys_dev_irq_ack(), because a level-triggered one re-asserts the
// instant the handler returns otherwise.
// 0, or -1 with errno: EACCES not the holder, EBUSY already armed,
// ENODEV no wakeword registered, ENOTSUP no usable interrupt.
int sys_dev_irq_enable(int index);

// Unmask, and return HOW MANY interrupts arrived since the last ack --
// 0 is a legitimate answer, meaning something else woke you.
int sys_dev_irq_ack(int index);

// CLAIM A USB DEVICE by its xHCI slot (QUERY_USB reports it), taking
// it off whatever class driver had it. Releasing with
// USB_RELEASE_REBIND offers it back to them; without, it is left
// UNBOUND. syscall_abi.h says why this is not SYS_DEV_CLAIM.
// 0, or -1 with errno.
// Scheduling priority, nice-style: LOWER runs first, 0 the default.
// <sys/resource.h>'s setpriority()/getpriority() are the names to use.
int sys_setpriority(int which, int who, int value);
int sys_getpriority(int which, int who);

int sys_usb_claim(int slot);
int sys_usb_release(int slot, unsigned flags);

// A CONTROL TRANSFER on a device you hold. The caller builds the
// 8-byte setup packet; the kernel performs it, because the host
// controller is shared and is not the holder's to drive.
// SET_ADDRESS and SET_CONFIGURATION are REFUSED -- both change state
// the kernel tracks. Returns the bytes transferred, or -1 with errno.
int sys_usb_control(int slot, const uint8_t setup[8], void *buf,
                    unsigned len, int in);

// AN ISOCHRONOUS OUT ENDPOINT on a device you hold, plus the packet
// buffer to feed it -- granted here because a transfer descriptor
// names a PHYSICAL address. The holder cannot touch the transfer ring
// (the controller is shared), so it hands over packets and the kernel
// queues them.
int sys_usb_isoch_open(struct usb_isoch_msg *m);

// QUEUE A GROUP of `count` descriptors, the Nth at `offset + N*stride`,
// with a completion asked for on the LAST when `ioc` is set. One call
// per group rather than per packet, because an endpoint at 125 us wants
// 8000 a second and a syscall each does not reach it -- syscall_abi.h
// has the measurement. Returns how many were queued, or -1 with errno.
int sys_usb_isoch_post(int slot, int ep, unsigned offset, unsigned len,
                       int ioc, unsigned count, unsigned stride);

// COMPLETIONS SINCE THE LAST CALL. The kernel's completion callback
// runs in interrupt context and cannot call into a process, so it
// counts and bumps this holder's wakeword; this is how many landed.
// Read-and-clear, so a missed wakeup still reports the true number.
int sys_usb_isoch_status(int slot, int ep);

// READ OR WRITE AN I/O BAR of a device you hold. Ring 3 cannot run
// in/out, so the kernel performs the access -- validated against that
// device's OWN BARs, so it can never reach another device's ports.
// VFIO's answer rather than ioperm()'s; syscall_abi.h has why.
// A read answers the value; a write answers 0; -1 with errno on a bad
// width, a BAR that is not an I/O BAR, or an offset past its end.
// THE VALUE COMES BACK THROUGH `out`, NOT THE RETURN, deliberately: a
// failure answering -1 is indistinguishable from a register that reads
// all ones once a caller casts it to the width it asked for, and a
// driver treating -EINVAL as 0xFF is how an unmapped card looks ready.
int sys_dev_io_read(int index, int bar, uint32_t offset, int width,
                    uint32_t *out);
int     sys_dev_io_write(int index, int bar, uint32_t offset, int width,
                         uint32_t value);

// BECOME THE MACHINE'S SOUND DEVICE. `m` is filled in by the caller and
// read back: `ring_phys` is where the core's ring lives, which is what
// a driver points a descriptor at. soundd then mixes into that ring
// without knowing a process is driving the card.
// 0, or -1 with errno -- EACCES without a claim on the device, ENODEV
// without a wakeword, EBUSY when one is already registered.
int sys_snd_register(struct snd_register_msg *m);

// Tell the core where the hardware is in the ring, in bytes and on a
// chunk boundary. It advances hw_pos and zeroes what the card has
// consumed, which is what makes a stalled app play silence.
int sys_snd_period(uint32_t pos);

// THE SOUND RING, READ-ONLY. Only the registered driver may ask, and
// only a driver that must COPY samples needs to -- a card that DMAs
// out of the ring is told a physical address instead and never sees
// them. Returns the ring's length in bytes with `*addr` filled in, or
// -1 with errno.
int64_t sys_snd_ring_map(uint64_t *addr);

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

// Reports on process-table SLOT `index`, not on a pid -- so a caller can
// walk the table without knowing which pids exist:
//     for (int i = 0; sys_proc_info(i, &p) == 0; i++) { if (!p.pid) continue; ... }
// An empty slot is a SUCCESSFUL call reporting pid 0: skip it, do not
// stop. Returns -1 (with sys_errno()) past the table's end -- which is
// what ends the walk -- or for a bad pointer.
int sys_proc_info(int index, struct proc_info *out);

// How many processes and threads may exist at once (QUERY_PROCLIMITS),
// so a per-process table is sized at startup; 0 if the kernel did not
// say. There is no compile-time count: the limit depends on the RAM.
int sys_proc_max(void);

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

// WALKING A LIST CLASS. A record-by-record loop that stops on the first
// short read, which is how every list class ends -- there is no count to
// ask for, deliberately, because a count read separately from the
// records is a count that can disagree with them by the time they are
// read. `idx` is declared by the macro and usable in the body.
//
//     struct query_blkdev d;
//     QUERY_FOREACH(QUERY_BLKDEV, d, i) { ... }
//
// It exists because about twenty /bin programs wrote this out, in three
// dialects that differed only in whether the index was `int` or
// `unsigned` and whether the short read was compared inline or through
// a named variable.
#define QUERY_FOREACH(cls, var, idx) \
    for (unsigned idx = 0; \
         sys_query_record((cls), (idx), &(var), sizeof (var)) >= (int)sizeof (var); \
         (idx)++)
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

// A NEW SESSION, which is what a controlling terminal belongs to. The
// caller leads it and a group of its own, and keeps no controlling
// terminal. -EPERM if it already LEADS a group. See
// abi/syscall_abi.h's SYS_SETSID for what sessions are for here.
int sys_setsid(void);
int sys_getsid(int pid);
int sys_remote_log(int kind, unsigned ip, const char *text);

// --- signals: dispositions and handlers -------------------------------

// What a handler is: one argument, the signal number. No siginfo and no
// ucontext -- POSIX's SA_SIGINFO shape needs the kernel to build two
// more structures on the user stack for information nothing here has
// (no sender pid is recorded, no fault address is passed through), and a
// struct full of zeroes is worse than not offering one.
typedef void (*sighandler_t)(int);

// Install `h` -- SIG_DFL, SIG_IGN, or a function -- and return the
// PREVIOUS one, or SIG_ERR with errno set.
//
// **THIS IS THE ONE TO REACH FOR.** It fills in the restorer and sets
// SA_RESTART, which is what a caller almost always wants and cannot
// sensibly supply itself: the restorer is a private detail of this
// runtime (userland/rt/sigtramp.c). sys_sigaction() below is for the
// caller that wants to READ an action back, or wants EINTR instead of a
// restart.
//
// BSD's `signal()` semantics, which is also glibc's: the handler stays
// installed across deliveries, and interrupted syscalls restart. The
// ancient System V behaviour -- reset to SIG_DFL on every delivery --
// is a race nothing should have to write around.
//
// SIGKILL, SIGQUIT and SIGSTOP are refused with EPERM, so there is
// always something that works.
//
// A SHELL SHOULD IGNORE SIGINT. Its own group is in front of the console
// whenever no job is running, and without a handler to redraw a prompt
// there is nothing else it could usefully do with one.
sighandler_t sys_signal(int sig, sighandler_t h);

// What sys_signal() returns on failure. -1 rather than 0, because 0 is
// SIG_DFL and a perfectly good previous disposition.
//
// Guarded because <signal.h> defines the same pointer under its own
// handler typedef, and a translation unit reaching for both (the libc's
// own signal.c does) would otherwise get a redefinition warning for two
// spellings of one value.
#ifndef SIG_ERR
#define SIG_ERR ((sighandler_t)-1)
#endif

// The full call: install `act` (or NULL to only read), write the
// previous action to `old` (or NULL). Returns 0, or -1 with errno.
//
// The restorer is YOURS to supply with a handler -- `__sigrestore` is
// the one this runtime provides, and passing 0 with a handler is EINVAL
// rather than a guess. sys_signal() exists so that almost nobody has to
// know that.
int sys_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old);

// The blocked mask. `how` is SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK
// (abi/signal_abi.h); either pointer may be NULL. 0, or -errno.
int sys_sigprocmask(int how, const uint64_t *set, uint64_t *old);

// Install `mask`, wait for a signal it does not block, put the old mask
// back. Returns -EINTR ALWAYS -- see abi/syscall_abi.h for why it is a
// syscall of its own rather than a mask swap around a pause.
int sys_sigsuspend(const uint64_t *mask);

// The restorer userland/rt/sigtramp.c provides. Declared so that a
// caller building its own `struct k_sigaction` has something to put in the
// field; there is no reason to write another one.
void __sigrestore(void);

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

// dup to the lowest free descriptor AT OR ABOVE `min` -- fcntl's
// F_DUPFD, which neither sys_dup() (lowest free) nor sys_dup2() (an
// exact number) can express. The copy does NOT inherit close-on-exec.
int sys_dupfd(int fd, int min);

// Close-on-exec, per DESCRIPTOR rather than per description: -1 queries,
// 0 clears, 1 sets. Returns the flag as it was before the call.
int sys_fd_cloexec(int fd, int op);

// Change a path's permission bits. 0, or -ENOENT / -ENOTSUP / -EFAULT.
// The type bits are masked off by the kernel -- see SYS_CHMOD.
int sys_chmod(const char *path, unsigned mode);

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
// The same for ONE path: moves when `path` or a direct child of it
// changes, so a reader of one directory is not woken by every write on
// the disk. May move for an unrelated change (a false positive), never
// misses a real one. -1 and errno for a bad path.
long long sys_fs_generation_of(const char *path);
// Watch `path` (and a directory's direct children) for the compositor:
// a change posts WIN_EV_FSWATCH carrying the returned id. -EPERM from
// anyone else. See SYS_FS_WATCH.
int sys_fs_watch(const char *path);

// Writes an MBR or GPT partition table to the disk. See
// abi/partition_abi.h for the request shape and why this takes a table
// description rather than a raw sector write.
//
// DESTRUCTIVE, and it does NOT remount anything -- the volume in use is
// unaffected and the new table takes effect at the next boot. Returns 0
// or a negative errno (-EPERM without MKPART_CONFIRM, -EINVAL for a
// table that overlaps or runs off the disk, -EIO on a write failure).
int sys_mkpart(const struct mkpart_request *req);

// Attaches a filesystem at a path, and detaches one. See
// abi/mount_abi.h for the request shape -- in particular why `source`
// is a PARTITION NUMBER rather than a device path (this OS has no
// /dev). Both return 0 or a negative errno; the interesting ones are
// -EBUSY (something is mounted there, or a file on it is open) and
// -ENODEV (nothing recognises the filesystem on that volume).
//
// NOT DESTRUCTIVE: nothing is formatted or overwritten, which is why
// there is no confirm flag of the kind SYS_MKPART needs.
int sys_mount(const struct mount_request *req);

// Writes an empty filesystem onto ONE partition. DESTRUCTIVE, so the
// request carries MKFS_CONFIRM and the kernel refuses without it -- the
// same shape SYS_MKPART uses, and the same caveat: a speed bump, not a
// permission check. Refuses a volume something is mounted from; use
// `fsformat` for the running root, which is a different operation.
int sys_mkfs(const struct mkfs_request *req);
// SYS_FS_CHECK: check (FSCK_REPAIR: and repair) the mounted volume
// holding `path`, in the kernel. 0 or -1 with errno; abi/mount_abi.h.
int sys_fs_check(const char *path, unsigned flags, struct fs_check_result *out);

// Makes a disk BOOT: the boot sector at LBA 0, and the core image into
// that disk's BIOS boot partition, with the two patches that depend on
// where they landed. DESTRUCTIVE, so the request carries
// INSTALL_BOOT_CONFIRM for a disk in use -- the same shape and the same
// caveat as SYS_MKPART. The kernel knows nothing about GRUB: it is
// handed the bytes, and where the core image goes comes from the
// target's own partition table.
int sys_install_boot(const struct install_boot_request *req);
int sys_umount(const char *point);

// SYS_KDFILE: files the kernel debugger staged (`remote put`) -- kdfile_abi.h.
// Only /bin/kdfiled has a use for it.
int64_t sys_kdfile(int op, uint64_t arg1, uint64_t arg2);

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

// --- processes and pipes ----------------------------------------------

// Creates a pipe. fds[0] is the read end, fds[1] the write end.
// Returns 0, or -1 with sys_errno().
int sys_pipe(int fds[2]);

// The PCM stream (abi/sound_abi.h): open maps the control page + ring
// at UADDR_SND_BASE (exclusive; -EBUSY while held, -ENODEV without
// hardware); ctl takes a SND_CTL_* op. Both 0 or -1 with sys_errno().
int sys_snd_open(void);
int sys_snd_ctl(int op);
// SND_CTL_FORMAT: the rate and the card's width (0 = its deepest) the
// next start plays at, while stopped. 0 or -1 with sys_errno().
int sys_snd_format(uint32_t rate, uint32_t bits);

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

// The bottom of the chain, and the only link that is not a convenience:
// SYS_SPAWN's flag word (SPAWN_* in abi/syscall_abi.h). `/bin/strace`
// is the only caller, and it passes SPAWN_TRACE.
//
// **THE CHAIN ENDS HERE, and the next thing to add belongs in the
// STRUCT, not in a seventh parameter.** Each of sys_spawn ->
// sys_spawn_env -> sys_spawn_group -> this one exists because spawn
// grew one capability; the syscall ABI already learned this lesson one
// parameter earlier and became `struct spawn_msg` for it
// (abi/syscall_abi.h). Six arguments is where the same argument starts
// applying on this side.
int sys_spawn_flags(const char *path, const char *args, int stdout_fd,
                     char **env, int pgid, unsigned flags);

// Everything a spawn can be told, as a struct -- which is what the note
// above said the next capability had to be, and `stdin_fd` is it. Zero
// the struct and set what you need; `stdin_fd`/`stdout_fd` are -1 for
// "leave it", so sys_spawn_opts_init() is not optional bookkeeping.
//
// A connected SOCKET is accepted on both, which is the point: it is how
// a handler spawned per connection reads and writes the client as an
// ordinary filter (userland/bin/inetd.c).
struct sys_spawn_opts {
    const char *args;   // whitespace-separated, or NULL; ignored if `argv` is set
    // The child's argv as a NULL-terminated VECTOR, argv[0] included --
    // execv's shape. Carried whole (SPAWN_ARGV), so an entry may hold a
    // space or be empty; `args` is the string form the kernel splits.
    // Refused with E2BIG if the flattened vector exceeds SPAWN_ARGS_MAX.
    char *const *argv;
    char      **env;    // NULL for an empty environment, not for `environ`
    int         stdin_fd;   // pipe read end or socket, or -1
    int         stdout_fd;  // pipe write end or socket, or -1
    // Anything writable, SPAWN_FD_LOG or SPAWN_FD_KMSG; -1 inherits the
    // caller's fd 2.
    int         stderr_fd;
    int         pgid;       // 0 inherits, PGID_NEW leads a new group
    unsigned    flags;      // SPAWN_* (abi/syscall_abi.h)
};

// The -1s that a zeroed struct would get wrong. Call it, then override.
void sys_spawn_opts_init(struct sys_spawn_opts *o);

int sys_spawn_opts(const char *path, const struct sys_spawn_opts *o);

// execv(): `argv` (argv[0] included, NULL-terminated) carried WHOLE, with
// the caller's environment. What a caller passing a PATH wants -- the
// string form of sys_spawn() splits on whitespace, so a file name with a
// space in it arrives as two arguments.
int sys_spawn_argv(const char *path, char *const *argv);

// BLOCKS until `pid` exits, then reaps it. Writes the exit code to
// `*out_code` if non-NULL. Returns the pid, or -1.
//
// Wraps the kernel's "0 means woken, ask again" retry contract, like
// sys_wait_event() -- the loop lives here rather than in every caller.
int sys_waitpid(int pid, int *out_code);

// Blocks as sys_waitpid() does, but a SIGNAL ENDS THE WAIT: returns
// -EINTR when a handler ran, rather than going back to sleep.
//
// For a process whose reason to wake may not be a child at all -- init
// blocks on its children and is asked to do things by SIGHUP. Every
// other caller here wants sys_waitpid()'s retry.
int sys_waitpid_intr(int pid, int *out_code);

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

// Both at once and neither blocking: returns the pid if the child has
// exited OR stopped, SYS_RETRY if it is still running, or -1 for a pid
// that is not this caller's live child. SIGNAL_IS_STOP() on the code
// tells the two apart -- and a STOP result leaves the child unreaped,
// so a caller must remember it rather than assume it is gone.
//
// What a shell calls once per prompt over its background jobs.
int sys_waitpid_check(int pid, int *out_code);

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


// SYS_DIAG -- ask a NAMED ring-3 service a question, or answer as one.
// See abi/diag_abi.h. Returns 1 on success, 0 when refused, -EBUSY when
// another caller holds the channel.
struct diag_msg;
int sys_diag(struct diag_msg *msg);

// Loadable kernel modules. 0 or -errno; the reason is in `dmesg`.
int sys_modload(const char *path);
int sys_modunload(const char *name);


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
// Blocks until this process has an event queued or `timeout_ms` passes,
// consuming nothing. 1 = an event is waiting, 0 = neither known nor an
// error. For a caller that drains its own queue; see SYS_WAIT_READY.
int sys_wait_ready(uint32_t timeout_ms);

int sys_wait_event(struct win_event *out);

#endif
