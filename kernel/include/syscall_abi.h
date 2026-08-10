#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

#include <stdint.h>

// Syscall numbers (passed in RAX) and argument conventions for `int
// 0x80`, shared between the kernel's dispatcher (kernel/core/syscall.c)
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
// experimental GUI ones above) -- see kernel/core/syscall.c for the
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

#endif
