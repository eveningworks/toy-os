#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

#include <stdint.h>
#include "errno.h" // error numbers -- what a failed syscall returns
#include "timer.h" // struct rtc_time -- reused by SYS_GETTIME and struct dirent's `modified` below

// Syscall numbers (passed in RAX) and argument conventions for `int
// 0x80`, shared between the kernel's dispatcher (kernel/proc/syscall.c)
// and userland test programs (userland/*.c) so both sides agree on the
// numbers without duplicating them -- kernel/include is on both build's
// include path (see the Makefile), so both just include this file.

// HOW TO READ THE PER-CALL RETURN NOTES BELOW, since error codes landed.
//
// Many of them say "returns -1 on failure". That is what a RING-3 CALLER
// sees, and it is still true -- but it is true one layer up: the raw
// syscall returns the NEGATED error number (abi/errno.h), and libsys's
// wrapper turns that into -1 while stashing the code for sys_errno()
// (userland/rt/sys.h). So `open()` of a missing file puts -2 in RAX and
// hands a program -1 with ENOENT behind it.
//
// Three things follow, and the second is the one that bites:
//
//   - Code using libsys is unaffected. `if (fd < 0)` means what it
//     always did.
//   - Code poking the RAW interface -- the /tests diagnostics, which
//     exist to do exactly that -- sees the code, not -1. A raw test
//     asserting `== -1` is asserting EPERM, since EPERM is 1.
//   - The calls documented as returning 0 ON FAILURE (SYS_UNLINK,
//     SYS_KILL, SYS_GETTIME, SYS_PROC_INFO, SYS_WIN_CREATE) were
//     deliberately NOT converted: a negative code is TRUTHY, so
//     returning one would make every `if (!sys_unlink(p))` caller read a
//     failure as success. A call whose SUCCESS is 1 and whose failure
//     was -1 (SYS_SET_COLOR) was converted like any other -- it is the
//     failure value, not the success value, that decides.
//     See docs/errno-design.md.
//
// Stated once here rather than edited into two dozen comments that would
// then have to be kept true individually.

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

// The value a BLOCKING syscall returns when the process was woken but
// must call again -- the spurious-wakeup contract every blocking call
// here shares (see SYS_WAIT_EVENT for why the kernel cannot simply
// hand over the result at wake time: the wake runs in an interrupt,
// under an address space where the caller's buffer is not addressable).
//
// It is NOT 0, for a concrete reason: 0 is a legitimate result for
// SYS_READ (end of file). Using it as the retry sentinel made a reader
// treat "woken, ask again" as "there will never be more data" -- a pipe
// read that returned empty the instant its writer produced something. A
// sentinel has to be a value the call can never otherwise return.
//
// It was -2 until error codes existed, and -2 is now -ENOENT. So it
// sits one past the top of the error range instead (abi/errno.h), which
// keeps the same property against a set of values that did not exist
// when it was chosen.
//
// AND IT IS NOT SIMPLY -EAGAIN, which is the obvious-looking move. The
// two mean different things here: EAGAIN is an ERROR a caller reports,
// while SYS_RETRY means the call did not fail at all -- the process was
// woken and must ask again, which libsys does in a loop the caller
// never sees (sys_read/sys_write). Merging them would make every
// blocking call's spurious wakeup look like a failure to the layer
// above, which is the same mistake as using 0.
#define SYS_RETRY (-4095)

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
                      //
                      // fd 0 is STDIN: the physical console keyboard.
                      // BLOCKS when nothing is typed -- the process is
                      // parked and the keyboard IRQ releases it, so an
                      // idle reader costs no CPU. It NEVER returns 0:
                      // a console has no end of file, and reporting one
                      // would tell a shell its input had closed.
                      //
                      // Raw, one byte per key, exactly the code
                      // keyboard.h's KEY_* namespace uses (specials are
                      // 0x91-0xA6) -- no echo, no line editing, no
                      // escape-sequence translation. All three are a
                      // line discipline, which belongs above a real TTY
                      // (docs/roadmap.md), so a reader echoes what it
                      // reads and does its own editing.
                      //
                      // THE FIRST fd-0 READ CLAIMS THE CONSOLE: the
                      // kernel shell stops taking keys until the
                      // claiming process dies. There is one keyboard,
                      // and two readers splitting it at random is worse
                      // than one reader having it.
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

#define SYS_LISTDIR_MAX 256 // caps how many entries a single SYS_LISTDIR
                            // call can fill. A `max` argument above this
                            // is silently clamped down to it, not
                            // rejected.
                            //
                            // IT WAS 32, and its comment claimed that
                            // "matches FS_MAX_FILES (fs.h), since that's
                            // the most any directory could ever hold" --
                            // which was wrong twice: FS_MAX_FILES is 256,
                            // and TFS3 has no per-directory cap at all.
                            // So `ls` silently listed the first 32
                            // entries of a bigger directory and stopped,
                            // with nothing said. Measured 2026-08-19 by
                            // putting 40 files in one directory.
                            //
                            // 256 matches FS_MAX_FILES for real, which
                            // bounds a TFS2 volume. IT STILL DOES NOT
                            // BOUND TFS3, so this remains a truncation
                            // point rather than a guarantee -- the real
                            // fix is an offset argument so a caller can
                            // page through, which changes this call's
                            // ABI and is a roadmap item. What changed
                            // here is that a caller can now DETECT it:
                            // a full array means "there may be more",
                            // and /bin/ls says so instead of stopping
                            // quietly.

#define SYS_LISTDIR 13 // RDI = pointer to a NUL-terminated directory
                        // path (same length limit as SYS_OPEN), RSI =
                        // pointer to an array of `struct dirent` (out),
                        // RDX = capacity of that array (clamped to
                        // SYS_LISTDIR_MAX). Wraps fs_list() (fs.c).
                        // Returns the number of entries written (RAX,
                        // 0..max), or a NEGATIVE ERRNO. Table order,
                        // not sorted, same as fs_list().
                        //
                        // **0 MEANS AN EMPTY DIRECTORY AND NOTHING
                        // ELSE.** It used to also mean "no such
                        // directory", because fs_list() returns void
                        // and simply does nothing for a path that is
                        // not a listable directory -- so `ls /nope`
                        // printed an empty listing and exited 0. The
                        // handler now probes the path when the count
                        // comes back zero: -ENOENT if nothing is
                        // there, -ENOTDIR if something is but is not a
                        // directory. -EFAULT for a bad path or
                        // output-array pointer, as before (the older
                        // comment here said -1, which this syscall has
                        // not returned since it grew errnos).

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

// The windowing protocol's delivery syscalls -- see abi/win_proto.h for
// the message format, which is the part meant to outlive this transport
// (a shared-memory ring is the intended successor; the event bytes
// don't change when it lands).
#include "win_proto.h" // struct win_event

#define SYS_POLL_EVENT 23 // RDI = pointer to a `struct win_event` (out).
                           // Never blocks. Returns 1 if an event was
                           // written, 0 if the queue is empty, -1 on a
                           // bad pointer or from a process that has no
                           // event queue (kernel code, or the legacy
                           // process_run_ring3() path -- neither has a
                           // scheduler slot to own one).

#define SYS_WAIT_EVENT 24 // RDI = pointer to a `struct win_event` (out).
                           // Returns 1 with an event written, or 0
                           // meaning "you were woken, ask again" -- see
                           // below. -1 on a bad pointer, or from a
                           // process with no queue (same cases as
                           // SYS_POLL_EVENT), which is also what a
                           // caller that cannot be parked gets, so a
                           // non-schedulable caller fails loudly rather
                           // than spinning forever on a syscall that
                           // silently never blocks.
                           //
                           // CALLERS MUST LOOP. A 0 return does not
                           // mean "no event" -- it means the process
                           // was woken and should call again:
                           //
                           //   while (sys_wait_event(&ev) != 1) { }
                           //
                           // This is the same spurious-wakeup contract
                           // a condition variable has, and it is here
                           // for a concrete reason rather than
                           // sloppiness: the wake happens inside an
                           // interrupt handler, under whatever address
                           // space happened to be current, so the
                           // kernel CANNOT copy the event into the
                           // waiting process's buffer at that moment.
                           // The copy has to happen back inside the
                           // client's own syscall, which means the
                           // client has to re-enter it. (Linux's
                           // alternative is to rewind RIP over the
                           // trapping instruction and let the syscall
                           // restart itself -- ERESTARTSYS. Not used
                           // here: it hides a hard assumption about the
                           // syscall instruction's length inside the
                           // scheduler, and the explicit loop costs a
                           // client three lines.)
                           //
                           // The loop does NOT spin the CPU: each pass
                           // that finds nothing parks the process again
                           // via scheduler_block_current(), so a
                           // waiting client uses no timeslices at all.

#define SYS_WIN_REQUEST 25 // RDI = pointer to a `struct win_request_msg`
                            // (in/out -- WIN_REQ_CREATE writes the new
                            // window id back into `window`). Returns 1
                            // on success, 0 if the server refused the
                            // request, -1 on a bad pointer, an unknown
                            // request type, a caller with no scheduler
                            // slot, or no window server registered
                            // (i.e. the desktop isn't running).
                            //
                            // ONE syscall for every windowing
                            // operation, dispatched on the message's
                            // own `type`, rather than a syscall per
                            // operation. That is the whole point: the
                            // client/server boundary stays a protocol,
                            // so adding an operation is a new message
                            // type and moving the server to ring 3 is a
                            // transport swap. See abi/win_proto.h.
                            //
                            // This supersedes SYS_WIN_CREATE/
                            // SYS_WIN_PRESENT (7/8) above, which stay
                            // for userland/win_test.c: those are modal
                            // and single-window, and their window never
                            // enters the WM's window list at all.

#define SYS_PIPE    26 // RDI = pointer to int[2] (out): [0] = read fd,
                        // [1] = write fd. Returns 1, or -1 (bad
                        // pointer, or no free pipe).

#define SYS_SPAWN   27 // RDI = path, RSI = whitespace-separated args
                        // (NULL/"" for none), RDX = an fd from
                        // SYS_PIPE's WRITE end to use as the child's
                        // stdout, or -1 to inherit. Returns the child's
                        // pid (> 0), or -1.
                        //
                        // THE CHILD INHERITS THE CALLER'S WHOLE
                        // DESCRIPTOR TABLE -- every fd names the same
                        // open file in both, refcounted. That is what
                        // makes redirection possible with no fork():
                        // the parent points its own fd 1 wherever it
                        // wants the child's to go, spawns, and puts its
                        // own back. RDX stays as the one-call shortcut
                        // for the common "capture this child's stdout"
                        // case, and is applied after inheriting.
                        //
                        // This is what lets a ring-3 program run
                        // another and read its output -- the thing a
                        // terminal does, and the first time one
                        // process here could see another's stdout.

// Flag for SYS_WAITPID's RDX. POSIX's WNOHANG under a shorter name and
// with the same meaning: ask, do not wait.
//
// It exists because the kernel primitive underneath (scheduler_poll())
// has ALWAYS been non-blocking, and this syscall was throwing that
// answer away -- so a ring-3 process could not ask "has it finished?"
// without committing to wait for it. A compositor has to check the
// processes it launched once a frame and can never block on one, so
// without this the first client that does not exit stops the desktop.
#define SYS_WNOHANG 1

#define SYS_WAITPID 28 // RDI = pid from SYS_SPAWN, RSI = pointer to an
                        // int (out) for the exit code, or NULL.
                        // RDX = flags: SYS_WNOHANG to poll instead of
                        // wait; 0 for the blocking behaviour that
                        // predates it.
                        // BLOCKS until that child exits, then reaps it,
                        // UNLESS SYS_WNOHANG is set -- in which case a
                        // child that is still running answers
                        // SYS_RETRY (0) immediately and nothing is
                        // reaped.
                        // Returns the pid on success, or -1 for a pid
                        // that isn't this caller's live child.
                        //
                        // Same "0 means woken, ask again" retry
                        // contract as SYS_WAIT_EVENT, and for the same
                        // reason -- see that entry. libsys wraps the
                        // loop (sys_waitpid()).
                        //
                        // RDI = -1 means ANY child of the caller,
                        // POSIX's wait() convention -- it cannot
                        // collide with a real pid, which is 1-based.
                        // It returns the pid it reaped, or -1 when the
                        // caller has NO CHILDREN AT ALL. That -1 is
                        // permanent rather than "not yet", so a caller
                        // must not loop on it: with no children,
                        // nothing can ever change the answer. "Has
                        // children, none dead yet" is the ordinary
                        // block (or SYS_RETRY under SYS_WNOHANG).

#define SYS_GETRANDOM 29 // RDI = buffer (out), RSI = byte count. Fills
                          // the buffer with random bytes and returns
                          // how many it wrote, or -1 for an invalid
                          // pointer or a count over
                          // SYS_GETRANDOM_MAX.
                          //
                          // ALWAYS fills the whole buffer or fails --
                          // it never returns a short count the way
                          // Linux's getrandom() can, because nothing
                          // here blocks waiting for entropy. What it
                          // cannot tell the caller is how GOOD the
                          // bytes are: the kernel may be running on a
                          // CPU with no RDSEED/RDRAND, in which case
                          // they come from timing jitter that is weak
                          // under emulation (see api/krandom.h's
                          // krandom_quality, which has deliberately not
                          // been exposed here -- a ring-3 program that
                          // could read it would mostly use it to decide
                          // to carry on anyway).

#define SYS_PROC_INFO 30 // RDI = process-table slot index, RSI = pointer
                          // to a `struct proc_info` (out, see
                          // abi/proc_info.h). Returns 1 on success, 0
                          // for a bad index or pointer.
                          //
                          // Indexed by SLOT, not by pid, so a caller can
                          // walk the whole table without knowing which
                          // pids exist -- which is what a task manager
                          // does. An EMPTY slot is a successful call
                          // reporting pid 0, so enumeration skips rather
                          // than stops. The bound is SYS_PROC_MAX below.
                          //
                          // Read-only and unprivileged: every process
                          // can see every other. This kernel has no user
                          // model to hang a permission on, and inventing
                          // one here would be a check with nothing
                          // behind it -- see SYS_KILL, which makes the
                          // same call about a far more dangerous
                          // operation and says so.

#define SYS_KILL      31 // RDI = pid, RSI = exit code to report.
                          // Terminates that process immediately.
                          // Returns 1 if it was killed, 0 if no such
                          // process.
                          //
                          // **UNPRIVILEGED, DELIBERATELY.** Any process
                          // may kill any other, including the window
                          // manager once it is a process (Milestone 41
                          // stage 4). There is no user model, no
                          // capability and no process-group notion in
                          // this kernel, so a permission check here
                          // would be decoration -- it would have to
                          // invent the very thing it claims to enforce.
                          // Written down rather than quietly assumed;
                          // when a privilege model lands, THIS is the
                          // syscall it has to gate first.
                          //
                          // Killing the WM is not a hole to be closed,
                          // it is the property stage 4 has to prove: the
                          // kernel must survive it. See
                          // docs/wm-ring3-design.md.
                          //
                          // This is the FORCE path. The polite one is
                          // the window close handshake (WIN_EV_CLOSE),
                          // which an app may refuse; both exist for the
                          // same reason Windows separates End Task from
                          // End Process.

#define SYS_TICKS     32 // No arguments. Returns the monotonic timer tick
                          // count since boot.
                          //
                          // MONOTONIC, unlike SYS_GETTIME, which reports
                          // RTC wall-clock time -- that one goes
                          // backwards when the clock is set and has
                          // one-second resolution, so it can measure
                          // neither an interval nor an animation. This
                          // TICKS, at whatever rate the timer runs
                          // (PIT_HZ, 100 today). Fine for pacing
                          // something coarse; useless for measuring
                          // anything shorter than 10ms, which is why
                          // CPU accounting no longer uses it -- see
                          // SYS_MONOTONIC_NS below, which is what a
                          // proc_info cpu_ns delta divides by.

#define SYS_MONOTONIC_NS 33 // No arguments. Returns NANOSECONDS since
                          // boot, from the kernel's best available
                          // clocksource (kernel/clocksource.h) -- the
                          // TSC where it is invariant and calibrated,
                          // the 100Hz timer otherwise.
                          //
                          // The DENOMINATOR for a CPU percentage: the
                          // numerator is a delta of proc_info's cpu_ns
                          // and this is a delta of the same clock, which
                          // is what makes the ratio a real percentage
                          // rather than one counter over an unrelated
                          // other one.
                          //
                          // Its RESOLUTION is not promised and a caller
                          // must not infer one -- on a machine with no
                          // invariant TSC this advances in 10ms steps
                          // and two reads inside one tick return the
                          // same value. Code that needs to know should
                          // measure across a long enough interval, not
                          // ask how precise the clock is.

#define SYS_SETTING   34 // RDI = pointer to a `struct setting_msg`
                          // (abi/setting_abi.h), IN and OUT -- the
                          // kernel copies it in, fills the out fields
                          // and copies it back. Returns 0, or -1 for a
                          // bad op, a bad index, or an unreadable
                          // pointer.
                          //
                          // One syscall with an op field rather than
                          // five syscalls, for the same reason
                          // SYS_WIN_REQUEST is one: the boundary is
                          // then a MESSAGE, and adding an operation
                          // costs an enum value instead of an ABI
                          // number that can never be reused.
                          //
                          // Note SETTING_OP_SET reports its outcome in
                          // `result`, not in the return value -- the
                          // syscall succeeded in asking; whether the
                          // setting applied AND persisted is a separate
                          // three-way answer (etc_config.h's `enum
                          // setting_result`), and collapsing it to
                          // ok/failed is exactly the lie that made
                          // `timezone Helsinki` claim success while
                          // writing nothing.

#define SYS_SYSINFO   35 // RDI = pointer to a `struct sys_info` (out,
                          // abi/setting_abi.h). Memory and disk usage
                          // -- the whole-machine facts no other syscall
                          // reports. CPU identity is SYS_CPU_INFO, the
                          // PCI count SYS_PCI_COUNT and uptime
                          // SYS_MONOTONIC_NS; none are duplicated here.
                          // Returns 0, or -1 if the pointer is bad.

#define SYS_CRASHTEST 37 // RDI = pointer to a `struct crash_msg`
                          // (abi/crash_abi.h), in and out.
                          //
                          // Enumerates the kernel's DELIBERATE fault
                          // kinds, and triggers one. It exists because
                          // the panic path is the one path a kernel
                          // cannot exercise by accident and must not
                          // get wrong -- validating a panic report
                          // otherwise means editing a debug command to
                          // dereference a bad pointer and taking it out
                          // again. Linux ships the same thing (lkdtm).
                          //
                          // CRASH_OP_TRIGGER MAY NOT RETURN: on success
                          // the machine has panicked. It returns 0 with
                          // CRASH_F_ARMED clear when the kernel is not
                          // armed -- which is the default, since a
                          // deliberate crash hole has no business being
                          // open unless `faultinject` was asked for on
                          // the GRUB command line.
                          //
                          // Ring-0 kinds only: a ring-3 program needs
                          // no help to dereference NULL, only to make
                          // the KERNEL fault.
                          // Returns 0, or -1 for a bad pointer or op.

#define SYS_WIN_DEBUG 39 // RDI = pointer to a `struct win_debug_msg`
                          // (abi/win_proto.h), in and out.
                          //
                          // TWP's DIAGNOSTIC channel, which needs its
                          // own carriage for the reason that struct
                          // exists at all: a `gui` command is 128 bytes
                          // and its reply up to 512, and widening
                          // SYS_WIN_REQUEST's message to fit would put
                          // that on the path of every request -- and
                          // WIN_REQ_PRESENT is the hot path.
                          //
                          // It was kernel-internal until now: the serial
                          // console called the window server directly.
                          // A ring-3 compositor has to answer these, so
                          // it needs a way to be handed the command and
                          // send the output back (WIN_REQ_DEBUG_TAKE /
                          // WIN_REQ_DEBUG_REPLY), and both are refused
                          // to anyone but the registered compositor.
                          //
                          // Returns what the server returned, or -1 for
                          // a bad pointer.

#define SYS_POWEROFF  38 // RDI = 0 to power off, 1 to reboot. Does not
                          // return on success.
                          //
                          // For the DESKTOP, which offers both from the
                          // Start menu and stops being able to reach
                          // system_poweroff() the moment it is a ring-3
                          // process (Milestone 41). The physical shell's
                          // own `poweroff`/`reboot` are unaffected --
                          // they are already ring 0.
                          //
                          // **Unprivileged, on purpose, and the same
                          // reasoning as SYS_KILL**: there is no user
                          // model here to gate it on, so a gate would be
                          // decoration. Anything that can spawn a
                          // process can already end the session.
                          //
                          // Both paths flush the disk cache first, so a
                          // write that returned success is on the
                          // platter before the machine stops -- the one
                          // thing this must not get wrong.
                          //
                          // Returns -1 for an op it does not recognise.
                          // On success it does not return at all, so a
                          // caller that continues past it should treat
                          // that as failure.

#define SYS_FS_GENERATION 36 // No arguments. Returns fs_generation()
                          // (api/fs.h) -- a counter the VFS bumps on
                          // every mutation of the filesystem, so a
                          // caller can answer "has anything changed?"
                          // with one integer compare instead of a
                          // directory scan.
                          //
                          // Deliberately its own syscall rather than a
                          // field in SYS_SYSINFO: the desktop polls
                          // this ONCE PER FRAME to decide whether to
                          // re-read /usr/wm/desktop, and a free poll is
                          // the entire reason the counter exists. A
                          // sysinfo field would cost a validated struct
                          // copy per frame for figures nobody asked
                          // for. Never zero once a filesystem is
                          // mounted, so 0 is usable as "not sampled
                          // yet"; it only ever increases.

// --- descriptor plumbing --------------------------------------------
//
// fds 0/1/2 are ORDINARY DESCRIPTORS, not special numbers: they simply
// start out naming the console (0, 1) and the kernel log (2). So they
// can be redirected like any other, which is what these two are for.
//
// Together with SYS_SPAWN's inheritance they give a shell the classic
// redirection dance WITHOUT fork() -- the parent redirects itself
// around the spawn, rather than the child redirecting itself before an
// exec that does not exist here:
//
//     int saved = dup(1);
//     dup2(file_fd, 1);
//     spawn("/bin/ls", 0, -1);   // inherits fd 1 -> the file
//     dup2(saved, 1);
//     close(saved);              // refcounted: does NOT close the file
#define SYS_DUP  41 // RDI = fd. Returns the LOWEST FREE descriptor
                     // naming the same open file, or -1. The two share
                     // everything about the stream, including a file's
                     // read offset -- they are two names, not two opens.

#define SYS_DUP2 42 // RDI = oldfd, RSI = newfd. Makes newfd name what
                     // oldfd names, CLOSING whatever newfd named first,
                     // and returns newfd (or -1 if oldfd is not open).
                     //
                     // dup2(fd, fd) is a NO-OP and specifically does not
                     // close -- POSIX says so, and getting it wrong
                     // destroys the stream the call was asked to
                     // preserve.

// The console's size in TEXT CELLS: rows in the low 32 bits of RAX,
// columns in the high 32. No arguments, cannot fail.
//
// Ring 3 had no way to ask. The console's size is not a constant here
// -- it is derived from the active font (gfx_char_h()/gfx_char_w()),
// and `font_size` is a runtime setting -- so a program that pages or
// draws columns had nothing to work from but a guess, and a guess is
// wrong on any machine whose font was changed. That is the same
// font-derived rule the GUI's whole layout already follows.
//
// Rows and columns in ONE return value rather than a struct through a
// pointer: two small numbers fit, and it keeps the call free of a
// user-memory copy (and of the validation that goes with one).
//
// First caller is /bin/less. The TTY milestone wants this too --
// klineedit's console front end already has a documented bug from not
// knowing the width (a line longer than the console repaints wrongly).
#define SYS_CONSOLE_SIZE 43

// ---- the current directory, and the rest of the path-keyed calls ----
//
// THE CWD IS THE KERNEL'S, NOT EACH SHELL'S. Every path argument in
// this ABI -- open, unlink, listdir, and the six below -- is resolved
// against the calling process's cwd inside the kernel, so a relative
// path means one thing everywhere. Before this, `apps/shell.c` and
// `struct tosh` each held a private `cwd` and resolved before calling,
// which made a bare "docs" mean "/docs" to any program started from
// either -- silently creating the wrong thing rather than failing.
// A cwd is inherited across SYS_SPAWN and starts at "/".
//
// Absolute paths are unaffected, and a process that never calls
// SYS_CHDIR sees exactly the old behaviour.

#define SYS_CHDIR 44 // RDI = path (relative paths resolve against the
                      // current cwd, as everywhere else). Returns 0, or
                      // -ENOENT / -ENOTDIR / -ENAMETOOLONG / -EFAULT.
                      // -EPERM for a caller with no scheduler slot and
                      // no legacy slot armed.

#define SYS_GETCWD 45 // RDI = buffer (out), RSI = its capacity. Copies
                       // the cwd as a NUL-terminated normalized
                       // absolute path and returns its LENGTH (not
                       // counting the NUL), or -ERANGE if the buffer is
                       // too small -- never a truncated path, which is
                       // a different directory, not a shorter answer.

#define SYS_MKDIR 46 // RDI = path. Returns 0, or -EEXIST / -ENOENT (no
                      // parent) / -ENAMETOOLONG / -EFAULT / -EIO.

#define SYS_RENAME 47 // RDI = old path, RSI = new path. Returns 0, or a
                       // negative errno. Whether a cross-directory
                       // rename of a DIRECTORY is possible depends on
                       // the volume's journal (fs.h / TFS3 credits): a
                       // v1 image refuses exactly that one case.

#define SYS_TRUNCATE 48 // RDI = path, RSI = new size in bytes (grow or
                         // shrink). Returns 0, or a negative errno.

#define SYS_STAT 49 // RDI = path, RSI = pointer to a `struct sys_stat`
                     // (out, below). Returns 0, or a negative errno.

#define SYS_LINK 50 // RDI = existing path, RSI = the new name. Returns
                     // 0, or -EPERM when the mounted filesystem's format
                     // has no link counts (tfs2 -- ask
                     // SYS_STAT/FS_CAP_HARDLINKS through sysinfo first
                     // for a better message), -EISDIR for a directory.

#define SYS_SYNC 51 // No arguments. Flushes the disk write-back cache
                     // and returns the number of SECTORS written, or a
                     // negative errno if some could not be. Zero is a
                     // real answer (nothing was pending), which is why
                     // this reports a count rather than a bare status:
                     // "the flush failed and your data is still only in
                     // RAM" is the one disk answer a caller must not
                     // read as success.

// What SYS_STAT reports. Deliberately NOT POSIX's `struct stat` -- there
// are no modes, owners, devices or link counts to put in one, and a
// struct full of zeroed fields invites a caller to believe them. The
// timestamps are `struct rtc_time` for the same reason `struct dirent`'s
// is: the epoch shape is kernel-internal (fs.h's fs_stat_info) and is
// converted back to civil time at this boundary.
struct sys_stat {
    uint64_t size;          // bytes; 0 for a directory, as fs_list() reports
    uint64_t ino;           // real on a backend with inodes, otherwise a
                            // stable synthetic (the table slot) -- see
                            // SYS_STAT_INODES below for which
    uint32_t is_dir;
    uint32_t flags;         // SYS_STAT_*
    struct rtc_time created;
    struct rtc_time modified;
};

// The inode number above is the filesystem's own, not a synthetic one.
// Mirrors fs.h's FS_CAP_INODES for the one caller that needs to say so.
#define SYS_STAT_INODES (1u << 0)

#define SYS_QUERY 52 // RDI = pointer to a `struct query_msg`
                      // (abi/query_abi.h), in and out.
                      //
                      // Reads a FACT: live kernel state, computed on
                      // every read and never persisted (see
                      // docs/settings-and-queries.md's "The vocabulary").
                      // One syscall with an information class rather
                      // than one syscall per fact, which is the growth
                      // SYS_SYSINFO / SYS_PROC_INFO / SYS_PCI_INFO /
                      // SYS_CPU_INFO were on -- and none of those four
                      // could answer "what facts exist?".
                      //
                      // Class 0 is the registry describing itself, so a
                      // caller needs to know exactly one number to
                      // discover every other class.
                      //
                      // Returns 0, or a negative errno.

#define SYS_SLEEP 40 // RDI = milliseconds. Parks the caller until that
                          // long has passed, then returns 0. Returns -1
                          // for a caller with no scheduler slot (the
                          // legacy loader, kernel code), which is a
                          // refusal rather than an instant return: a
                          // caller that cannot sleep must decide what to
                          // do instead, not silently spin.
                          //
                          // The RESOLUTION is one timer tick, so a 1 ms
                          // sleep lasts until the next tick and a sleep
                          // never returns EARLY. 0 ms is a valid
                          // request and means "until the next tick",
                          // which is a yield that gives up the rest of
                          // the slice rather than re-entering the
                          // rotation immediately -- SYS_YIELD is still
                          // the call for the latter.
                          //
                          // Why this exists: an idle loop with nothing
                          // to wait ON had no way to stop consuming CPU.
                          // init spends most of its life in exactly that
                          // state -- it blocks in waitpid(-1) while it
                          // has children and has nothing to block on
                          // when it does not. See docs/init-design.md.

// The longest single SYS_SLEEP, one hour. Not a security limit: it
// keeps a garbage argument from parking a process for the rest of the
// boot, which looks exactly like a hang. A caller wanting longer calls
// it again.
#define SYS_SLEEP_MAX_MS 3600000

// The number of process-table slots SYS_PROC_INFO can be asked about.
// Mirrors the kernel's SCHED_MAX_PROCS; a caller loops 0..this-1.
#define SYS_PROC_MAX 64

// The largest single SYS_GETRANDOM request. Not a security limit -- it
// stops a bad count from turning into a long uninterruptible fill in
// ring 0, the same reasoning as every other bounded copy across this
// boundary.
#define SYS_GETRANDOM_MAX 4096

#endif
