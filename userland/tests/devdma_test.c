// A CLAIMED DEVICE'S DMA BUFFER, from the side that owns it --
// SYS_DEV_DMA_ALLOC, stage 3 of docs/umdf-design.md.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS.** "The call returned an
// address and the memory is writable" is satisfied by plain anonymous
// memory: it says nothing about the frames being pinned, contiguous,
// reachable by a device, or tied to the claim rather than to this
// address space. So every check here is about a DIFFERENCE that only
// the real grant produces -- refused without a claim, granted with one,
// refused twice over, and GONE from this process's map the moment the
// claim drops.
//
// The last of those is the one with a bug behind it. The mapping is
// BORROWED, so no mapping teardown frees these frames -- the claim
// does. An explicit release therefore used to hand them back to the
// allocator while this process still held a live writable PTE for every
// one of them, which `meminfo audit` calls a DANGLING mapping and is
// the one thing that audit treats as a bug. Asserting the region is
// gone is how that stays fixed.
//
// It runs SPAWNED: the grant resolves the caller's address space the
// way mmap does, and the legacy `run` loader has no scheduler slot.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "pci.h"
#include "syscall_abi.h"
#include "query_abi.h"

// Does this process have a DMA region mapped at `base`? The kernel's
// own answer, through the same query `pmap` prints.
static int dma_region_at(uint64_t base) {
    struct query_procmap q;
    int me = getpid();
    QUERY_FOREACH(QUERY_PROCMAP, q, i) {
        if ((int)q.pid != me) continue;
        if (q.kind == QUERY_PROCMAP_DMA && q.base == base) return 1;
    }
    return 0;
}

// A device that can be claimed, preferring one NO ring-0 driver holds
// so a run on a machine with a sound card does not take it away.
static int claimable_device(void) {
    struct query_pcidev q;
    int bound = -1;
    QUERY_FOREACH(QUERY_PCIDEV, q, i) {
        if (!q.claimable) continue;
        if (!q.driver[0]) return (int)q.index;
        if (bound < 0) bound = (int)q.index;
    }
    return bound;
}

int main(void) {
    utest_begin("devdma_test", "a claimed device's DMA buffer",
                UTEST_VERDICT_FILE);

    int dev = claimable_device();
    if (dev < 0) {
        utest_notef("note: nothing claimable on this machine -- no leg to run");
        return utest_end();
    }
    utest_checkf(1, "pci %d is claimable", dev);

    uint64_t phys = 0;

    // WITHOUT A CLAIM, FIRST. This is the line that makes everything
    // below evidence: the same call on the same device, refused, and
    // the only thing that changes between here and the next check is
    // the claim.
    errno = 0;
    utest_check(sys_dev_dma_alloc(dev, 4096, &phys) == -1 && errno == EACCES,
                "an unclaimed device gets no DMA buffer");

    utest_check(sys_dev_claim(dev) == 0, "the device can be claimed");

    errno = 0;
    utest_check(sys_dev_dma_alloc(dev, 0, &phys) == -1 && errno == EINVAL,
                "a zero-length buffer is refused");
    errno = 0;
    utest_check(sys_dev_dma_alloc(dev, DEV_DMA_MAX_BYTES + 1, &phys) == -1 &&
                errno == EINVAL,
                "and one over DEV_DMA_MAX_BYTES");

    int64_t va = sys_dev_dma_alloc(dev, 4096, &phys);
    utest_checkf(va > 0, "a page is granted, at %llx", (unsigned long long)va);
    if (va <= 0) {
        sys_dev_release(dev, DEV_RELEASE_REBIND);
        return utest_end();
    }

    // BELOW 4 GiB, because a 32-bit DMA engine has to reach it, and
    // PAGE ALIGNED because a descriptor ring's base register has no
    // low bits. Both are properties of the ZONE, not of the address
    // happening to look plausible.
    utest_checkf(phys != 0 && phys < (1ull << 32),
                 "its physical address is in DMA32: %llx",
                 (unsigned long long)phys);
    utest_check((phys & 0xFFF) == 0, "and page aligned");

    // ZEROED BY THE KERNEL, not by whoever had the frames last -- a
    // grant that handed over another process's leftovers would read
    // as working memory right up until it leaked something.
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)va;
    int nonzero = 0;
    for (int i = 0; i < 4096; i++) if (p[i]) nonzero++;
    utest_checkf(nonzero == 0, "the buffer arrives zeroed (%d non-zero byte(s))",
                 nonzero);

    p[0] = 0xA5;
    p[4095] = 0x5A;
    utest_check(p[0] == 0xA5 && p[4095] == 0x5A,
                "it is writable end to end");

    utest_check(dma_region_at((uint64_t)va),
                "the kernel reports it as a `dma` region in this process");

    // ONE BUFFER PER DEVICE. A second grant would be a second physical
    // range the claim has to remember, and nothing needs it.
    errno = 0;
    utest_check(sys_dev_dma_alloc(dev, 4096, &phys) == -1 && errno == EBUSY,
                "a second buffer for the same device is refused");

    // THE DROP, AND THE BUG BEHIND IT. The frames go back to the
    // allocator here; if the mapping outlived them this process would
    // still be holding writable PTEs into free memory.
    utest_check(sys_dev_release(dev, 0) == 0, "the claim can be dropped");
    utest_check(!dma_region_at((uint64_t)va),
                "AND THE MAPPING IS GONE WITH IT, not left dangling");

    // And the frames really were freed rather than merely forgotten:
    // the same device grants again.
    utest_check(sys_dev_claim(dev) == 0, "the device can be claimed again");
    uint64_t phys2 = 0;
    int64_t va2 = sys_dev_dma_alloc(dev, 4096, &phys2);
    utest_checkf(va2 > 0, "and granted a buffer again, at %llx",
                 (unsigned long long)va2);
    utest_check(dma_region_at((uint64_t)va2), "which is mapped in turn");

    utest_check(sys_dev_release(dev, DEV_RELEASE_REBIND) == 0,
                "and the device goes back to the kernel");
    return utest_end();
}
