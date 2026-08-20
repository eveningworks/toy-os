#ifndef KRANDOM_H
#define KRANDOM_H

#include <stdint.h>
#include <stddef.h>

// The kernel's entropy source (Milestone 2, docs/roadmap.md) -- the gap
// that blocked both a randomised stack canary and kernel ASLR, since
// this kernel previously had no source of unpredictability at all.
//
// **This is not a CSPRNG, and the API says so out loud.** It is a
// hardware source when the CPU has one, a timing-jitter harvest when it
// does not, and a strong 64-bit mixing function over whichever it got.
// There is no entropy accounting, no reseed schedule and no
// backtracking resistance, because none of those can be made honest on
// top of a jitter source measured inside an emulator. What there IS is
// krandom_quality(), so a caller that needs to know how much to trust
// its bytes can ask instead of assuming -- the same "a setting says
// whether it actually persisted" instinct as enum setting_result.
//
// The values are ORDERED BY TRUST and compared as such (a registered
// source may raise the quality, never lower it), so a new tier goes in
// its place in that order rather than on the end.
enum krandom_quality {
    KRANDOM_NONE = 0,   // krandom_init() has not run; output is a fixed sequence
    KRANDOM_JITTER = 1, // TSC jitter only -- unpredictable in principle, WEAK
                        // under an emulator, where the "hardware" timing this
                        // measures is itself software (see krandom.c)
    KRANDOM_VIRTIO = 2, // virtio-rng -- entropy the HOST supplies. Real
                        // randomness, and far better than jitter under an
                        // emulator, but it arrives by device round trip and
                        // is only as trustworthy as the hypervisor, which
                        // already owns this machine's memory anyway.
    KRANDOM_HW = 3,     // RDSEED/RDRAND -- the CPU's own entropy source
};

// Seeds the pool. Call once from kernel_main(), AFTER idt_init() and
// cpu_info_init(): the CPUID feature check needs the latter, and the
// jitter fallback needs the PIT ticking to have something to beat
// against. Safe to call again (re-seeds; a caller mid-way through
// krandom_bytes() is not a case that exists, since nothing here blocks).
void krandom_init(void);

// A 64-bit random value. Never fails -- when there is no entropy at
// all it still returns a well-mixed value, it just isn't unpredictable,
// which is what krandom_quality() is for.
uint64_t krandom_u64(void);

// Fills `buf` with `n` random bytes. Handles any n, including a tail
// shorter than 8 bytes.
void krandom_bytes(void *buf, size_t n);

// --- an external entropy source ---------------------------------------
//
// A source that cannot exist at krandom_init() time -- it needs PCI,
// the frame allocator and a virtqueue, all of which come up long after
// the pool is first seeded. virtio-rng is the one implementation
// (kernel/drivers/virtio/virtio_rng.c), and this seam is why krandom.c
// includes nothing about it: the same shape as display_driver, minus
// the registry, since a second entropy device is not a thing that is
// coming.
//
// `fill` returns 1 when it wrote all n bytes, 0 otherwise -- a source
// that declines is not an error, it just contributes nothing that time.
typedef int (*krandom_source_fn)(void *buf, size_t n);

// Mixes an immediate draw into the pool and keeps the source for
// periodic reseeding. The quality is raised to `q` if `q` is higher
// than what the pool already has, and NEVER lowered: a CPU with RDSEED
// does not become less trustworthy because a virtio device turned up.
//
// Seeding rather than serving: a draw costs a device round trip and a
// spin-poll, so krandom_u64() cannot be one. Linux does the same thing
// (virtio-rng feeds the hwrng framework, which reseeds the CRNG; it is
// not the per-call source), and the reason is the same.
void krandom_register_source(krandom_source_fn fill, enum krandom_quality q);

// Which source the values are actually coming from.
enum krandom_quality krandom_quality(void);

// "hardware (RDSEED/RDRAND)" / "virtio-rng (host entropy)" /
// "TSC jitter" / "none". Never NULL, so a caller can print it without a
// null check.
const char *krandom_quality_name(enum krandom_quality q);

#endif
