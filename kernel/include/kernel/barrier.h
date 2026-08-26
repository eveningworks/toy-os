#ifndef BARRIER_H
#define BARRIER_H

// Ordering primitives, for drivers that share memory with a device.
//
// READ THIS BEFORE "FIXING" THE ONES THAT LOOK EMPTY.
//
// x86-64 is TSO (Total Store Order), which is a strong memory model:
// stores are never reordered with other stores, and loads are never
// reordered with other loads. The ONLY reordering the architecture
// permits is a later LOAD passing an earlier STORE.
//
//     wrote:              may execute as:
//     store A             store A
//     store B             store B      store-store: ordered, always
//     load  C             load  C      load-load:   ordered, always
//
//     store A             load  C      store-load:  MAY be reordered --
//     load  C             store A                   the only one
//
// So for a virtqueue, which is the reason this header exists:
//
//   - descriptors and avail->ring[] must be visible before avail->idx
//     is bumped .................................. store-store: FREE
//   - used->idx must be read before the used->ring[] entry it
//     publishes .................................. load-load:   FREE
//   - the notify (MMIO) write must follow the avail->idx store
//     ............................................ store-store: FREE
//
// "Free" from the CPU. NOT free from the COMPILER, which reorders,
// hoists and caches freely and cannot see that a device is watching --
// so those three still need kbarrier(), which emits no instruction and
// only constrains GCC.
//
// The one case that would need a real fence is store-then-load:
// publishing avail->idx and then READING the device's suppression hint
// to decide whether to notify at all. This kernel's virtio code does
// not negotiate VIRTIO_F_EVENT_IDX, so that path does not exist and
// kmb() is declared here without being called. That is deliberate. If
// you are adding EVENT_IDX, kmb() is what you need and its absence
// elsewhere is not an oversight to copy.
//
// AND THE HONEST PART: none of this is testable here. Every automated
// test runs under TCG, which serialises execution, so a build with
// every barrier deleted passes identically. These are correctness by
// ARGUMENT, not by test -- the same standing caveat paging.h carries
// about write-combining, and for the same reason.
//
// Implemented with GCC's atomic builtins rather than inline asm, which
// also keeps kernel/README.md's "nothing outside arch/ contains inline
// assembly" rule intact without an exemption.

// Compiler-only. Stops GCC moving or caching a load/store across this
// point. Emits NO instruction on any architecture.
static inline void kbarrier(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }

// A real StoreLoad fence (MFENCE on x86-64). Needed only for the one
// reordering TSO permits -- see this header's top comment.
// A SPIN-WAIT HINT, and on a virtual machine it is not an optimisation.
//
// `pause` tells the CPU this is a spin loop. Three things follow, and
// the second is the one that matters here:
//
//   * it stops the pipeline speculating down a loop that is going
//     nowhere, and drops power draw;
//   * **KVM's Pause-Loop Exiting watches for it.** A guest spinning
//     WITHOUT pause is indistinguishable from a guest doing work, so the
//     host runs it for its whole timeslice -- including when the thread
//     it is waiting FOR is the QEMU main loop that would complete the
//     I/O. A spin with pause exits to the host, which can then schedule
//     that thread. A busy-wait with no pause starves the very thread it
//     is waiting on;
//   * it makes each iteration cost tens of cycles rather than a few, so
//     a loop bounded by an ITERATION COUNT is worth far more wall-clock
//     time -- which is what virtqueue.c's backstop is (see its comment
//     on why the bound is a count and not a duration).
//
// None of this is visible under TCG, where the emulator yields
// constantly and the host is never starved. That is why a suite that is
// entirely TCG can be green while a KVM guest hangs.
static inline void cpu_relax(void) { __asm__ volatile ("pause" ::: "memory"); }

static inline void kmb(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#endif
