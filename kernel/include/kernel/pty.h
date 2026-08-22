#ifndef KERNEL_PTY_H
#define KERNEL_PTY_H

#include <stdint.h>
#include "tty.h"

// Pseudo-terminals: a terminal whose DRIVER is another file descriptor.
//
// A pty is a `struct tty` (kernel/tty.h) with one end held by a program
// that wants to BE a terminal -- a window, a session recorder, a test --
// and the other handed to a program that wants to USE one. Bytes written
// to the master arrive at the terminal as if typed; bytes the terminal
// emits (a program's output, and the discipline's own echo) are what the
// master reads back.
//
//     terminal emulator                        the shell
//        master fd  ── write ──►  ldisc  ──►  slave fd  (read)
//        master fd  ◄── read ───  output ◄──  slave fd  (write)
//
// **NO /dev/ptmx, AND NO PATH AT ALL.** Linux hands out a master by
// opening /dev/ptmx and names the slave /dev/pts/N; this OS has no
// device nodes and vfs.c has no mount table to hang them on (the same
// reason docs/query-design.md refused a /proc). SYS_OPENPTY returns BOTH
// fds instead -- the shape of BSD's openpty(3) rather than posix_openpt
// -- which also deletes the setsid()+TIOCSCTTY dance: a process is
// HANDED its terminal rather than acquiring one by opening a path.
//
// **THE END-OF-FILE RULES ARE THE PIPE'S**, deliberately, because they
// are the ones that already work here and the ones a shell is written
// against: a read with nothing available and the other end still open
// means WOULD BLOCK (park and retry); with the other end gone it means
// end of input. Conflating those two is how a terminal decides a running
// program has finished.

#define PTY_MAX 8 // ptys, kernel-wide. TTY_MAX bounds them anyway --
                  // every pty is a terminal -- but a separate cap keeps
                  // this table's arithmetic its own.

// Allocates a pty with ONE master and ONE slave already counted.
// Returns its index, or -1 when none are free.
//
// **IT HAS NO OWNER YET, AND THAT IS THE CONSOLE'S RULE.** A terminal is
// claimed by the first process to READ it (syscall_fd.c), not by the one
// that made it -- because those are different processes and the reader
// is the one that needs it. A terminal emulator OPENS the pty and then
// hands the slave to a shell; if opening had claimed it, the emulator
// would own a terminal it never reads, and the shell's tcsetpgrp() would
// be refused as -EPERM -- which is exactly the bug this comment replaced.
// The symptom was Ctrl-C in a window signalling the wrong group and
// nothing dying.
int pty_create(void);

// The terminal behind a pty, or NULL. What the termios and foreground
// group syscalls resolve an fd to.
struct tty *pty_tty(int idx);
int pty_valid(int idx);

// Reference counting per END: a pty's storage is released once BOTH
// ends reach zero, so a master that outlives its slave can still drain
// what the slave already wrote.
//
// **THERE IS NO pty_add_master()/_add_slave(), DELIBERATELY.** A second
// fd naming the same end -- dup2, or a child inheriting it -- shares the
// DESCRIPTION and bumps its refcount, so this count is only ever
// decremented, once, when the last descriptor for that end goes away.
// pipe.c grew an add pair that nothing has called since spawn stopped
// needing it; not repeating that here, per CLAUDE.md's note on a slot
// that is present and read by nobody.
void pty_close_master(int idx);
void pty_close_slave(int idx);

// Is the other end still there? What a reader consults to tell WOULD
// BLOCK from END OF FILE.
int pty_master_open(int idx);
int pty_slave_open(int idx);

// The channel a master reader parks on -- the pty's OUTPUT side, which
// is a different thing from the terminal's input queue and so needs its
// own address. A slave reader parks on tty_wait_chan(pty_tty(idx)).
const void *pty_out_wait_chan(int idx);

// Master side. `read` drains what the terminal has emitted; `write`
// feeds bytes in as if typed, through the line discipline.
//
// read:  the byte count, 0 for END OF FILE (empty and no slave left),
//        -1 for WOULD BLOCK (empty, slave still alive).
// write: `len` on success, 0 when there is no terminal left to type at.
//        Never partial: a write that does not fit is dropped a byte at a
//        time by the input queue exactly as a keystroke is, which is the
//        right behaviour for INPUT and the reason this cannot block.
int64_t pty_master_read(int idx, char *dst, uint32_t len);
int64_t pty_master_write(int idx, const char *src, uint32_t len);

// Slave side. A slave READ is tty_read() and needs nothing here; a slave
// WRITE goes through this rather than tty_output() because it is the one
// path that may legitimately BLOCK.
//
// Returns `len`, -1 for WOULD BLOCK (the master is behind and the buffer
// is full), or 0 when no master remains -- the write is discarded and
// reported, the same answer pipe_write() gives with no readers (there is
// no SIGHUP here; docs/roadmap.md carries it).
//
// **ALL OR NOTHING, and that is why it is separate from the echo path.**
// Echo happens in the keyboard IRQ and cannot block, so it DROPS when
// the buffer is full; a program's output must not, because nothing in
// ring 3 loops on a short write. Same split, same reasoning as
// pipe_write()'s atomicity.
int64_t pty_slave_write(int idx, const char *src, uint32_t len);

#endif // KERNEL_PTY_H
