#ifndef USERLAND_SYS_H
#define USERLAND_SYS_H

#include <stdint.h>
#include <stddef.h>
#include "syscall_abi.h"
#include "pci.h"     // struct pci_device, for sys_pci_info()
#include "cpuinfo.h" // struct cpu_info, for sys_cpu_info()

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
// The return convention is the kernel's own, unchanged and documented
// per-call in abi/syscall_abi.h: mostly "0 or positive on success, -1
// on failure", with the exceptions called out there. Nothing here
// translates it into errno, because there is no errno yet.

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
int     sys_unlink(const char *path);
int     sys_listdir(const char *path, struct dirent *out, int max);

// Convenience over sys_write(): writes a NUL-terminated string to
// stdout. The one wrapper here that is not a bare syscall, because
// "print this string" is what nearly every caller actually wants and
// hand-rolling a strlen at each call site is noise.
int64_t sys_print(const char *s);

// --- input and time --------------------------------------------------

// Non-blocking: returns the next queued key, or -1 if none. See
// syscall_abi.h for why there is no blocking variant of THIS call --
// use sys_wait_event() instead for anything new.
int sys_read_key(void);

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

// --- windowing -------------------------------------------------------

// One typed message in, one out. See abi/win_proto.h.
int sys_win_request(struct win_request_msg *req);

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
