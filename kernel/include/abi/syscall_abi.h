#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

#include <stdint.h>
#include "timer.h" // struct rtc_time -- reused by SYS_GETTIME and struct dirent's `modified` below

// Syscall numbers (passed in RAX) and argument conventions for `int
// 0x80`, shared between the kernel's dispatcher (kernel/proc/syscall.c)
// and userland test programs (userland/*.c) so both sides agree on the
// numbers without duplicating them -- kernel/include is on both build's
// include path (see the Makefile), so both just include this file.

#define SYS_EXIT  1 // RDI = exit code
#define SYS_WRITE 2 // RDI = fd, RSI = buffer pointer, RDX = length;
                    // returns bytes written (RAX), or -1 on a bad fd or
                    // bad pointer. Capped at SYS_WRITE_MAX per call.
                    //
                    // ABI NOTE: this used to be a 2-arg call (RDI =
                    // buffer, RSI = length, always went to the console)
                    // -- changed to take an fd once SYS_OPEN/SYS_READ
                    // below existed and "which stream" stopped being a
                    // foregone conclusion. Every existing caller
                    // (write_test.c, write_bad_test.c, counter_a.c,
                    // counter_b.c, echo.c) was updated to pass 1
                    // (stdout) as RDI. fd 1 and 2 (stdout/stderr) both
                    // go to the console (vga_putc), same behavior as
                    // before; fd >= 3 must come from SYS_OPEN.

#define SYS_WRITE_MAX 1024

// Experimental userspace-GUI syscalls (see kernel/core/gui_test.c /
// apps/README.md's "GUI in user space" note for the honest scope of
// this: modal only -- one ring-3 process gets the real screen to
// itself while it runs, there's no scheduler yet for it to coexist
// with the kernel-space window manager).
#define SYS_GUI_INIT     3 // RDI = pointer to a `struct gui_info` (out).
                            // Maps the real linear framebuffer directly
                            // into the caller's address space at
                            // GUI_FB_VADDR. Returns 1 (RAX) on success,
                            // 0 on failure (bad pointer, no framebuffer,
                            // or a mapping ran out of memory).
#define SYS_GUI_POLL_KEY 4 // No arguments. Returns (RAX, sign-extended)
                            // a queued key same as keyboard_getchar()
                            // would, or -1 if none is waiting yet --
                            // never blocks.

#define GUI_FB_VADDR 0x8000300000ULL // where SYS_GUI_INIT maps the framebuffer

struct gui_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch; // bytes per row -- may exceed width*(bpp/8)
    uint32_t bpp;
};

// General-purpose syscalls, usable by any ring-3 process (not just the
// experimental GUI ones above) -- see kernel/proc/syscall.c for the
// implementation and userland/echo.c for a program that uses both.
#define SYS_READ_KEY 5 // No arguments. Non-blocking, same contract (and
                        // for the same reason) as SYS_GUI_POLL_KEY above
                        // -- returns (RAX, sign-extended) a queued key,
                        // or -1 if none is waiting. A real blocking
                        // version (sti, then keyboard_getchar()'s hlt
                        // loop) was tried and breaks after exactly one
                        // key: see the long comment in syscall.c's
                        // handler for why blocking-with-interrupts-on
                        // isn't safe inside this dispatcher yet. Callers
                        // that want blocking behavior spin-poll instead
                        // (see userland/echo.c).
#define SYS_SBRK     6 // RDI = increment in bytes (a plain heap bump,
                        // not "true" sbrk's signed shrink support -- 0
                        // or positive only). Returns (RAX) the previous
                        // break -- i.e. a pointer to `increment` freshly
                        // mapped, zeroed bytes -- or -1 if the calling
                        // process never had its heap set up (see
                        // syscall_reset_heap() in syscall.h) or ran out
                        // of physical memory while mapping new pages.

// A real per-window protocol, built on top of SYS_READ_KEY above -- see
// kernel/core/win_test.c and userland/win_test.c. Genuinely different
// from SYS_GUI_INIT: a SYS_GUI_INIT process gets the ENTIRE real
// framebuffer mapped into its own address space and draws straight onto
// the real screen; a SYS_WIN_CREATE process never touches the real
// framebuffer at all -- it only ever sees its own private w*h pixel
// buffer, and the kernel (in SYS_WIN_PRESENT) is the one that composites
// that buffer onto the real screen, drawing a real title bar and close
// button around it. That's a genuine client/server split -- the
// process is a "client" that only knows about its own content, same
// shape as a real windowing protocol.
//
// Still modal, though, same limitation as SYS_GUI_INIT: there's no
// concurrency between this and the kernel-space window manager (wm.c),
// so only one of these can be on screen at a time and it isn't a
// window inside wm.c's own window list. Making that concurrent needs
// the scheduler to give the kernel-space WM loop and a scheduled
// ring-3 process fair turns, which scheduler.c's current design
// doesn't do (once any process is READY, kernel-space code doesn't get
// scheduled again until every process exits -- see scheduler_tick()'s
// comment). See README's "Ideas for what's next".
struct win_request {
    // in: desired content size + top-left position on the real screen
    uint32_t w, h;
    int32_t x, y;
    // out: how the buffer mapped at WIN_BUF_VADDR is laid out
    uint32_t pitch; // bytes per row -- always w * 4, no padding
    uint32_t bpp;   // always 32
};

#define WIN_BUF_VADDR 0x8000400000ULL // where SYS_WIN_CREATE maps the buffer
#define WIN_MAX_W 640 // caps the buffer at 640x480x4 bytes = 300 pages,
#define WIN_MAX_H 480 // an amount syscall.c is happy to track per-page

#define SYS_WIN_CREATE  7 // RDI = pointer to a struct win_request
                           // (in/out). Allocates + maps a private,
                           // zeroed w*h*4-byte pixel buffer at
                           // WIN_BUF_VADDR; also records x/y for
                           // SYS_WIN_PRESENT to use. Returns 1 (RAX) on
                           // success, 0 on failure (bad pointer, size
                           // zero or over WIN_MAX_W/H, or out of
                           // physical memory).
#define SYS_WIN_PRESENT 8 // No arguments. Composites the buffer from
                           // SYS_WIN_CREATE onto the real screen at the
                           // position given there, kernel-drawn title
                           // bar + close button included. Returns 1
                           // (RAX), or -1 if the calling process never
                           // called SYS_WIN_CREATE.

// Real file I/O against the in-memory filesystem (fs.c) -- the piece a
// future libc's fopen()/fread()/fwrite() would sit on top of (see the
// syscall stubs a newlib-style port needs: _open/_read/_write/_close).
// Previously fs.c was reachable only from kernel-space (the shell's
// ls/cat/write/etc commands) -- this is the first time a ring-3 process
// can touch a real, named file at all, not just the console.
//
// Honest limitation: fs_write() (fs.c) works on null-terminated C
// strings via k_strlen(), not explicit-length byte buffers -- there's
// no offset-based partial write in the underlying filesystem. So
// SYS_WRITE to a file fd always APPENDS (never overwrites at an
// arbitrary offset), and a buffer containing an embedded NUL byte will
// truncate early at that byte, same as it would passed to any C string
// function. Fixing that properly means extending fs.c itself to take
// explicit lengths -- not done here; see README's "Ideas for what's
// next". Reads (SYS_READ) don't have this problem: fs_read() already
// returns a pointer + explicit size, so file content read back is
// exact, including any byte value.
//
// fd numbers start at 3, following the same convention libc expects --
// fd 0/1/2 (stdin/stdout/stderr) stay reserved for SYS_WRITE's console
// path above (SYS_READ doesn't support fd 0 yet -- that's SYS_READ_KEY's
// job, kept separate rather than conflated with file reads).
#define SYS_O_WRITE 1 // open for writing (default: read-only)
#define SYS_O_CREAT 2 // create the file if it doesn't exist (write only)
#define SYS_O_TRUNC 4 // truncate to empty on open (write only)

#define SYS_READ  9  // RDI = fd (from SYS_OPEN, read mode), RSI =
                      // buffer pointer, RDX = length. Reads from the
                      // fd's current position (tracked per-fd,
                      // advanced by each call) and returns bytes read
                      // (RAX) -- 0 at end of file, or -1 on a bad fd or
                      // bad pointer. Capped at SYS_WRITE_MAX per call,
                      // same as SYS_WRITE.
#define SYS_OPEN  10 // RDI = pointer to a NUL-terminated path (at most
                      // FS_PATH_MAX - 1 bytes -- fs.c's own limit; a
                      // bare name like "notes.txt" is treated as
                      // "/notes.txt", see fs.h), RSI
                      // = SYS_O_* flags (bitwise OR). Returns a small
                      // fd (RAX, >= 3) on success, or -1 (bad pointer,
                      // file not found without SYS_O_CREAT, or the
                      // open-file table -- 8 slots -- is full).
#define SYS_CLOSE 11 // RDI = fd. Frees the fd table slot. Returns 0, or
                      // -1 if the fd wasn't open (or belongs to a
                      // different process -- fds aren't shared across
                      // processes any more than the heap or window
                      // state above are).

#define SYS_UNLINK 12 // RDI = pointer to a NUL-terminated path (same
                       // length limit as SYS_OPEN). Wraps fs_delete()
                       // (fs.c) -- deletes a file, or an empty
                       // directory. Returns 1 (RAX) on success, 0 on
                       // failure (bad pointer, doesn't exist, or a
                       // non-empty directory -- fs_delete() doesn't do
                       // recursive delete).

// A directory entry as filled in by SYS_LISTDIR below -- deliberately
// reuses FS_PATH_MAX for `name` even though a single path component is
// always shorter than a full path, just to avoid a second size constant
// (fs.c already guarantees every component fits in FS_PATH_MAX, since
// it's a substring of a path that does).
struct dirent {
    char name[64];   // FS_PATH_MAX (fs.h) -- last path component only,
                      // e.g. "notes.txt", not "/docs/notes.txt"
    uint32_t size;    // meaningless (0) for directories, same as fs_list()
    uint32_t is_dir;
    // Added for /bin/ls's `-l` (see userland/ls.c) -- SYS_LISTDIR's
    // kernel-side handler fills this via an extra fs_stat() call per
    // entry (fs.c/fs.h), same `struct rtc_time` SYS_GETTIME already
    // hands to ring-3, reused rather than declaring a syscall-private
    // copy (same precedent as SYS_PCI_INFO reusing struct pci_device).
    // Zeroed (all-0 rtc_time) for the implicit root's own children if
    // fs_stat() ever legitimately fails for an entry -- shouldn't
    // happen for anything fs_list() itself just reported, but the
    // syscall handler doesn't treat that as fatal for the whole call.
    struct rtc_time modified;
};

#define SYS_LISTDIR_MAX 32 // caps how many entries a single SYS_LISTDIR
                            // call can fill -- matches FS_MAX_FILES
                            // (fs.h), since that's the most any
                            // directory could ever hold anyway. A `max`
                            // argument above this is silently clamped
                            // down to it, not rejected.

#define SYS_LISTDIR 13 // RDI = pointer to a NUL-terminated directory
                        // path (same length limit as SYS_OPEN), RSI =
                        // pointer to an array of `struct dirent` (out),
                        // RDX = capacity of that array (clamped to
                        // SYS_LISTDIR_MAX). Wraps fs_list() (fs.c).
                        // Returns the number of entries written (RAX,
                        // 0..max) -- 0 if the directory is empty or
                        // doesn't exist, same as fs_list()'s own
                        // no-op-on-missing-dir behavior; -1 only for a
                        // bad path or output-array pointer. Table
                        // order, not sorted, same as fs_list().

#define SYS_GETTIME 14 // RDI = pointer to a `struct rtc_time` (out, see
                        // timer.h). Wraps rtc_read_local() (tz.c) --
                        // the same timezone-adjusted wall-clock time the
                        // shell's `time` command and the taskbar clock
                        // show, not raw UTC hardware time. Returns 1
                        // (RAX) on success, 0 on a bad pointer.

#define SYS_YIELD 15 // No arguments. Cooperatively gives up the rest of
                      // this process's timeslice to the next
                      // scheduler-managed process (scheduler.c), if any
                      // is ready -- otherwise a plain no-op. Only does
                      // anything for a process spawned under the
                      // preemptive scheduler (scheduler_demo_run());
                      // for the older single-process-at-a-time path
                      // (process_run_ring3(), used by every *test
                      // command except the counter_a/counter_b demo)
                      // there's nothing else to yield to, so it's
                      // always a no-op there. Always returns 0 (RAX).

// Socket-fd scaffolding -- see kernel/proc/syscall.c's `struct open_file`
// and docs/decisions.md for the fuller reasoning. There's no NIC driver
// or protocol stack yet (see README.md's "Basic TCP/IP networking" --
// PCI enumeration, IRQ registration, and contiguous memory are done;
// this is the fd/syscall layer, still ahead of the driver itself), so
// SYS_SEND/SYS_RECV below always fail with -1 for now -- deliberately,
// not a bug. What this DOES get you: a real fd namespace shared between
// files and sockets (SYS_CLOSE, and process exit cleanup, already work
// on a socket fd for free, since neither ever looked at file-specific
// state to begin with), and an ABI that's already settled by the time a
// real transport exists, instead of needing a breaking change then.
#define SYS_SOCKET 16 // RDI = domain, RSI = type -- both reserved for
                       // future use (AF_INET/SOCK_STREAM, say) and must
                       // be passed as 0 for now; a nonzero value is
                       // rejected (-1) rather than silently ignored, so
                       // a caller relying on a real value being honored
                       // later fails loudly today instead of quietly
                       // once a real domain/type distinction exists.
                       // On success, allocates a socket-kind fd (same
                       // table, same fd namespace as SYS_OPEN's file
                       // fds -- see SYS_SOCKET's kernel-side comment)
                       // and returns it (RAX); -1 if the fd table is
                       // full. SYS_READ/SYS_WRITE reject a socket fd
                       // (-1, "bad fd") -- SYS_SEND/SYS_RECV below are
                       // the only way to use one.
#define SYS_SEND   17 // RDI = fd (from SYS_SOCKET), RSI = buffer
                       // pointer, RDX = length. Always returns -1 for
                       // now -- there's no transport to send through
                       // yet (see this section's top comment) -- once a
                       // NIC driver exists this becomes the real send
                       // path; the ABI (which register holds what)
                       // isn't expected to change when that happens.
#define SYS_RECV   18 // RDI = fd (from SYS_SOCKET), RSI = buffer
                       // pointer, RDX = length. Always returns -1, same
                       // reasoning as SYS_SEND above.

// The first syscalls added specifically so a real disk-hosted ELF64
// binary (not just a kernel-space shell built-in) can do something
// other than file I/O -- see docs/roadmap.md's real-disk-hosted-ELF-
// binaries entry and userland/lspci.c, the first program to use them.
// Wrap pci_init()'s already-recorded device table (kernel/drivers/
// pci.c) -- there's no live rescan here, same as the `lspci` shell
// command (apps/shell_sys.c) reading the same table kernel-space-side.
// `struct pci_device` itself lives in pci.h, not duplicated here --
// same precedent as SYS_GETTIME reusing timer.h's `struct rtc_time`
// directly rather than declaring a syscall-private copy; userland code
// just `#include`s "pci.h" for the type (Makefile's USERLAND_CFLAGS
// already has `-Ikernel/include`), even though it can't call any of
// that header's functions (those live in kernel/drivers/pci.c, never
// linked into a userland ELF).
#define SYS_PCI_COUNT 19 // No arguments. Returns the number of PCI
                          // devices pci_init() found at boot (RAX) --
                          // wraps pci_device_count() directly.

#define SYS_PCI_INFO  20 // RDI = device index (0 .. SYS_PCI_COUNT's
                          // result - 1), RSI = pointer to a
                          // `struct pci_device` (out, see pci.h).
                          // Returns 1 (RAX) on success, -1 for an
                          // out-of-range index or an invalid output
                          // pointer. Wraps pci_device_at().

// Added for /bin/ls's `--color=auto`-by-default output (userland/ls.c)
// -- the first syscall letting a ring-3 process affect its own console
// color, mirroring the shell's own `color` command (apps/shell_sys.c's
// cmd_color(), which just calls vga_set_color() directly from kernel
// space). RDI/RSI are raw `enum vga_color` values (vga.h -- see the
// same reuse-the-kernel-struct precedent as SYS_PCI_INFO/SYS_GETTIME;
// userland code #includes "vga.h" for the enum only, same as ls.c does
// for `pci.h`'s struct in lspci.c). Out-of-range values (not 0-15) are
// rejected (-1) rather than clamped or ignored, so a caller passing a
// bad value finds out immediately instead of drawing in some arbitrary
// fallback color.
#define SYS_SET_COLOR 21 // RDI = foreground vga_color, RSI = background
                          // vga_color. Wraps vga_set_color() directly.
                          // Returns 1 (RAX) on success, -1 if either
                          // value is outside 0-15 (VGA_BLACK..VGA_WHITE).

#define SYS_CPU_INFO 22 // RDI = pointer to a `struct cpu_info` (out, see
                         // api/cpuinfo.h). Returns 1 (RAX), or -1 if the
                         // pointer isn't a writable user range.
                         //
                         // Note what this syscall is FOR, because half
                         // of what it returns needs no kernel at all:
                         // CPUID is unprivileged, so a ring-3 program
                         // can identify the CPU by itself. What it
                         // cannot do is read CR0/CR4/EFER to find out
                         // which of those capabilities the OS actually
                         // switched ON -- that's privileged, and it's
                         // the `enabled` field here. Returning the whole
                         // struct rather than only the privileged half
                         // keeps one decoder (family/model combining,
                         // leaf-4 cache maths) in one place instead of
                         // duplicating it into every ring-3 caller --
                         // the mistake userland/lspci.c's own copy of
                         // the PCI class table already demonstrates.

#endif
