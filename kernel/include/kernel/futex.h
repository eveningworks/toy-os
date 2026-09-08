#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

#include <stdint.h>

// THE WAKEWORD: one word per process that everything which makes it
// ready bumps, so a process can wait for SEVERAL kinds of thing at once.
//
// The futex beside it waits on ONE word, and a compositor has two
// sources -- its event queue and, once the channel exists, a client's
// messages. There is no poll() here to wait on both, so instead every
// source bumps the SAME word: the kernel when it queues an event, a
// sender when it writes a message. The waiter parks on that word and,
// when it wakes, looks at all of its sources.
//
// This is the self-pipe trick, or eventfd, in futex form -- what an
// event loop without a unified poll turns into. A third source later
// (a signal, a timer) bumps the same word and needs nothing new.

// `pid` has something to look at: bump its wakeword and release anyone
// parked on it. Safe when the process registered none, which is the
// normal case -- only a process that waits this way ever does.
//
// Called from the event-post path, including from interrupt context.
void futex_note_ready(int pid);

// Drops the registration for an address space that is going away. The
// word is a user page; a stale physical address here would be a write
// into whatever the allocator handed out next.
void futex_wakeword_release(uint64_t pml4_phys);

#endif
