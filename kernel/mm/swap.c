// The swap area -- see swap.h for what it is and why it talks to a
// block device rather than to a path.
//
// Nothing in the tree evicts a page yet: this is the store, built and
// tested on its own so that the reclaim path above it has something
// known-good to sit on (docs/swap-design.md, stage 1).
//
// THE HEADER IS ONE SECTOR AND THE FIRST PAGE IS RESERVED. Reserving a
// whole page for a 24-byte header wastes 4 KiB once and keeps every
// slot page-aligned on the device, so a slot's LBA is a multiply rather
// than a multiply-plus-offset. The alternative -- packing slot 0 in
// beside the header -- buys one page and adds a special case to the one
// arithmetic this file has.
//
// A TRANSFER BUFFER IS NEVER ON THE STACK. The header buffer is
// kmalloc'd because a block read is a DMA target on AHCI and virtio,
// and a DMA write into a stack frame corrupts a saved register
// somewhere else entirely (docs/conventions/kernel.md).
#include "swap.h"
#include "block.h"
#include "heap.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

static const struct block_device *g_dev;
static uint32_t  g_slots;      // total, including the header slot
static uint32_t  g_used;       // allocated, including the header slot
static uint8_t  *g_bitmap;     // one bit per slot; bit 0 is always set
static uint32_t  g_hint;       // where the next search starts

static uint32_t bitmap_bytes(uint32_t slots) { return (slots + 7) / 8; }

static int slot_test(uint32_t s) { return (g_bitmap[s / 8] >> (s % 8)) & 1; }
static void slot_set(uint32_t s)  { g_bitmap[s / 8] |= (uint8_t)(1u << (s % 8)); }
static void slot_clear(uint32_t s) { g_bitmap[s / 8] &= (uint8_t)~(1u << (s % 8)); }

// The first sector of slot `s`. Slot 0 starts at LBA 0, so the header
// is simply the first sector of the area.
static uint32_t slot_lba(uint32_t slot) { return slot * SWAP_SECTORS_PER_PAGE; }

// One page in either direction, split against the device's transfer
// cap. A caller that ignores max_sectors_per_xfer() gets a REFUSED
// transfer rather than a short one (block.h), so the split is not
// optional even for eight sectors.
static int xfer_page(uint32_t slot, uint64_t phys, int write) {
    if (!g_dev || slot == 0 || slot >= g_slots) return 0;
    int cap = blkdev_max_sectors_per_xfer(g_dev);   // whole blocks on a 4K disk
    if (cap < 1) return 0;

    uint8_t *buf = (uint8_t *)(uintptr_t)phys;   // identity-mapped
    uint32_t lba = slot_lba(slot);
    int left = SWAP_SECTORS_PER_PAGE;
    while (left > 0) {
        int n = left < cap ? left : cap;
        int ok = write ? g_dev->write_sectors(lba, n, buf)
                       : g_dev->read_sectors(lba, n, buf);
        if (!ok) return 0;
        lba  += (uint32_t)n;
        buf  += (uint32_t)n * 512;
        left -= n;
    }
    return 1;
}

// ---- turning it on and off ------------------------------------------

// How many whole pages fit on the device. Answered here rather than at
// each caller because the awkward part -- a device whose sector count
// is not a multiple of 8 -- has exactly one right answer and should
// have exactly one implementation of it.
static uint32_t slots_for(const struct block_device *dev) {
    if (!dev || !dev->sector_count) return 0;
    return dev->sector_count() / SWAP_SECTORS_PER_PAGE;
}

int swap_format(const struct block_device *dev, const char **why) {
    const char *ignored = "";
    if (!why) why = &ignored;

    if (!dev || !dev->write_sectors) { *why = "not a writable block device"; return 0; }
    if (g_dev == dev) { *why = "that device is in use as swap"; return 0; }

    uint32_t slots = slots_for(dev);
    // Two slots is the smallest area that can hold anything: the header
    // and one page. Refusing below that here means every later bound in
    // this file can assume g_slots >= 2.
    if (slots < 2) { *why = "too small to hold a page"; return 0; }

    uint8_t *sec = kmalloc(512);
    if (!sec) { *why = "out of memory"; return 0; }
    k_memset(sec, 0, 512);

    struct swap_header *h = (struct swap_header *)sec;
    k_strlcpy(h->magic, SWAP_MAGIC, SWAP_MAGIC_N);
    h->version   = SWAP_VERSION;
    h->slots     = slots;
    h->page_size = SWAP_PAGE_SIZE;

    // Partial: the header is one sector, which is less than a block on a
    // 4K-sector disk. Slot 0 is the header's alone, so the rewrite of the
    // rest of its block touches nothing else.
    int ok = blkdev_write_partial(dev, 0, 1, sec);
    kfree(sec);
    if (!ok) { *why = "the write failed"; return 0; }

    klog_printf("swap: formatted %s -- %u slots, %u KiB\n",
                dev->name ? dev->name : "?", (unsigned)(slots - 1),
                (unsigned)((slots - 1) * (SWAP_PAGE_SIZE / 1024)));
    return 1;
}

int swap_on(const struct block_device *dev, const char **why) {
    const char *ignored = "";
    if (!why) why = &ignored;

    if (g_dev) { *why = "swap is already on"; return 0; }
    if (!dev || !dev->read_sectors || !dev->write_sectors) {
        *why = "not a writable block device"; return 0;
    }

    uint8_t *sec = kmalloc(512);
    if (!sec) { *why = "out of memory"; return 0; }
    if (!blkdev_read_partial(dev, 0, 1, sec)) {
        kfree(sec); *why = "the read failed"; return 0;
    }

    // A PARSER REJECTS RATHER THAN GUESSES, and here that is the whole
    // safety property: nothing on this path may write to a device it
    // has not recognised, or `swapon` on a mistyped name eats a
    // filesystem. Formatting is a separate verb for the same reason.
    struct swap_header *h = (struct swap_header *)sec;
    struct swap_header hdr = *h;
    kfree(sec);

    if (k_strncmp(hdr.magic, SWAP_MAGIC, SWAP_MAGIC_N) != 0) {
        *why = "not a swap area"; return 0;
    }
    if (hdr.version != SWAP_VERSION)   { *why = "unrecognised swap version"; return 0; }
    if (hdr.page_size != SWAP_PAGE_SIZE) { *why = "swap page size does not match"; return 0; }
    if (hdr.slots < 2)                 { *why = "swap area holds no pages"; return 0; }
    // The header is DATA the device carries, so it can disagree with
    // the device -- a partition that shrank, or an image copied onto a
    // smaller one. Believe the device.
    if (hdr.slots > slots_for(dev))    { *why = "swap area is bigger than the device"; return 0; }

    uint8_t *bm = kmalloc(bitmap_bytes(hdr.slots));
    if (!bm) { *why = "out of memory"; return 0; }
    k_memset(bm, 0, bitmap_bytes(hdr.slots));

    g_dev    = dev;
    g_slots  = hdr.slots;
    g_bitmap = bm;
    g_used   = 0;
    g_hint   = 1;
    slot_set(0);          // the header, never handed out
    g_used = 1;

    klog_printf("swap: on -- %s, %u pages (%u KiB)\n",
                dev->name ? dev->name : "?", (unsigned)(g_slots - 1),
                (unsigned)((g_slots - 1) * (SWAP_PAGE_SIZE / 1024)));
    return 1;
}

int swap_off(const char **why) {
    const char *ignored = "";
    if (!why) why = &ignored;
    if (!g_dev) { *why = "swap is not on"; return 0; }
    // A page in a slot has nowhere else to be. Turning swap off under
    // it would not lose the slot, it would lose the page -- silently,
    // and only for whoever touched it next.
    if (g_used > 1) { *why = "pages are still swapped out"; return 0; }

    kfree(g_bitmap);
    g_bitmap = 0;
    g_dev    = 0;
    g_slots  = 0;
    g_used   = 0;
    g_hint   = 0;
    klog_write("swap: off\n");
    return 1;
}

int swap_active(void) { return g_dev != 0; }

const char *swap_device_name(void) {
    return (g_dev && g_dev->name) ? g_dev->name : "";
}

// ---- slots -----------------------------------------------------------

uint32_t swap_slot_alloc(void) {
    if (!g_dev || g_used >= g_slots) return 0;
    // From the hint, then wrap once. A rotating hint rather than a scan
    // from 0 for the same reason pmm uses one: consecutive allocations
    // are the common case and restarting at 0 makes each one walk the
    // whole allocated prefix.
    for (uint32_t n = 0; n < g_slots; n++) {
        uint32_t s = g_hint + n;
        if (s >= g_slots) s -= g_slots;
        if (s == 0) continue;              // the header
        if (!slot_test(s)) {
            slot_set(s);
            g_used++;
            g_hint = (s + 1 < g_slots) ? s + 1 : 1;
            return s;
        }
    }
    return 0;
}

void swap_slot_free(uint32_t slot) {
    if (!g_dev || slot == 0 || slot >= g_slots) return;
    // Freeing a slot nobody holds must not decrement the count -- the
    // same shape as pmm_free_frame() ignoring an already-free frame,
    // and for the same reason: a double free that silently adjusts the
    // books shows up much later as a slot handed out twice.
    if (!slot_test(slot)) return;
    slot_clear(slot);
    g_used--;
}

int swap_write_page(uint32_t slot, uint64_t phys) { return xfer_page(slot, phys, 1); }
int swap_read_page(uint32_t slot, uint64_t phys)  { return xfer_page(slot, phys, 0); }

void swap_stats(uint32_t *out_total, uint32_t *out_used) {
    if (out_total) *out_total = g_slots;
    if (out_used)  *out_used  = g_used;
}
