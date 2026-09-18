#ifndef KMUTEX_H
#define KMUTEX_H

// A SLEEPING LOCK: a contender that can sleep yields the CPU instead of
// holding the machine still, which is the whole point of it existing.
//
// It replaces `vfs.c`'s blanket preemption guard, and the difference is
// what `docs/blocking-design.md` is about: the guard stopped EVERY
// context for the length of a backend call, so a compositor waiting on
// nothing at all still waited. A lock stops only the contexts that
// actually want the same thing.
//
// **IT IS ONE LOCK OVER A NON-RE-ENTRANT SUBSYSTEM, which is Linux's
// Big Kernel Lock and is a first step rather than an end state** --
// that one took from 2.0 to 2.6.39 to remove. What would make a finer
// lock possible here is `tfs3.c`'s module-level scratch (`g_blk`,
// `g_ptr_blk` and the journal images), because a per-mount lock cannot
// protect state that is not per mount. `docs/smp-design.md` wants that
// work anyway.
//
// **ONE THING FROM THE BKL IS DELIBERATELY NOT COPIED: it was dropped
// when its holder slept and retaken on return.** Here the lock must be
// HELD across a disk wait, or the serialisation the scratch buffers
// depend on evaporates at exactly the moment it is needed.
struct kmutex {
    int depth;   // 0 = free. RECURSIVE: see below
    int owner;   // pid of the holder, 0 for the kernel context. Only
                 // meaningful while depth > 0
    int owned;   // whether `owner` means anything -- pid 0 is the
                 // KERNEL context, a real owner, so it cannot double as
                 // "nobody"
};

// Blocks until the lock is held. A caller with a scheduler slot and no
// preemption guard raised SLEEPS; anything else spins (see kmutex.c on
// why the spin must not disable preemption).
//
// **RECURSIVE, BECAUSE WHAT IT REPLACED WAS A COUNTER.**
// scheduler_preempt_disable() nests, so an FS_OP() reached from inside
// another one was merely depth 2 and worked. A plain mutex turns every
// one of those into an instant self-deadlock -- which is what it did:
// the boot hung at `init: target graphical` with no output at all. The
// BKL was recursive for the same reason and it is the same trade: it
// does NOT make recursion safe (a backend re-entered through an
// fs_list() callback still walks tfs3.c's scratch twice, which vfs.c's
// comment has always refused), it just keeps a shape that already
// worked from becoming a hang.
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);

// For a diagnostic, and for an assertion in a test. Never a decision.
int kmutex_held(const struct kmutex *m);
int kmutex_owner(const struct kmutex *m);

#endif
