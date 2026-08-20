#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

// Pipes: a bounded byte buffer with a read end and a write end.
//
// Built for the thing that actually needed it -- a ring-3 terminal
// reading the output of a program it spawned. That is the first time
// one process in this kernel has had to see another's stdout, and it is
// the missing piece behind `fork`/`exec`-style process plumbing
// generally (docs/roadmap.md's Milestone 10).
//
// BLOCKING IS THE POINT, and it reuses machinery that already exists
// rather than inventing a second waiting mechanism. A read with no data
// and a live writer parks the caller via scheduler_block_current();
// pipe_write() and the last pipe_close_writer() wake it with
// scheduler_wake(). Waking hands back the same "0 = you were woken, ask
// again" contract SYS_WAIT_EVENT uses (see abi/syscall_abi.h), so the
// re-read happens inside the reader's own syscall -- which is required,
// not stylistic: the wake can run in an interrupt under a different
// address space, where the reader's buffer is not addressable.
//
// Deliberately NOT a full POSIX pipe: no SIGPIPE (there are no signals
// yet), and a write with no readers left is dropped and reported rather
// than raising anything. Both are recorded in the roadmap instead of
// being half-implemented here.

// This pipe's wait channel, for a caller that needs to park on it.
// 0 for an invalid index.
const void *pipe_wait_chan(int idx);

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX      8 // concurrent pipes, kernel-wide

// Allocates a pipe with one reader and one writer already counted.
// Returns its index, or -1 if none are free.
int pipe_create(void);

// Reference counting for the two ends. A pipe's storage is released
// once BOTH counts reach zero -- a reader that outlives its writer must
// still be able to drain what was already written, which is what makes
// "read until EOF" mean what it should.
void pipe_add_reader(int idx);
void pipe_add_writer(int idx);
void pipe_close_reader(int idx);
void pipe_close_writer(int idx);

// Copies `len` bytes in, ALL OR NOTHING. Returns `len` on success, -1
// when they do not fit right now and a reader still exists ("would
// block" -- park and retry, exactly as pipe_read() means it), and 0
// when there are no readers left (the write is discarded; see the
// header note on SIGPIPE).
//
// It used to take what fitted and report a short count. Nothing in ring
// 3 loops on a short write, so a producer faster than its reader
// silently lost the remainder -- latent while the only reader was a
// shell draining continuously, and unavoidable once `|` made the reader
// a second process that may not have run yet.
//
// Atomic because it can afford to be: a single write is capped at
// SYS_WRITE_MAX (1024) against a PIPE_BUF_SIZE (4096) buffer, so one
// write always fits once the pipe drains and a parked writer cannot
// wait on a request too large to ever satisfy. POSIX guarantees the
// same for writes up to PIPE_BUF.
int64_t pipe_write(int idx, const char *src, uint32_t len);

// Copies up to `len` bytes out.
//
// Returns the byte count, or 0 for END OF FILE -- meaning empty AND no
// writers remain. Returns -1 to mean WOULD BLOCK: empty, but a writer
// is still alive, so the caller must park rather than report EOF.
// Distinguishing those two is the whole contract; conflating them is
// how a terminal decides a running program has finished.
int64_t pipe_read(int idx, char *dst, uint32_t len);

// 1 if `idx` names a live pipe. For syscall-layer validation.
int pipe_valid(int idx);

#endif
