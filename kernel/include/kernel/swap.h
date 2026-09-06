#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>

struct block_device;

// THE SWAP AREA: pages parked on a block device, addressed as SECTORS.
//
// WHY IT IS NOT A PATH. A page fault can happen anywhere, including
// inside an FS_OP -- which is exactly why mmap_fault_in() refuses a
// file-backed fault-in there (kernel/mm/mmap.c). Reading swap through
// fs_read_range() would re-enter tfs3.c's module-level scratch buffers,
// recursion the preemption guard cannot see. So this layer knows a
// `struct block_device` and an LBA and nothing else, and a swap FILE --
// when there is one -- becomes an extent list resolved ONCE at swapon,
// which is what Linux's swap extents are for. See docs/swap-design.md.
//
// SLOT 0 IS THE HEADER AND IS NEVER HANDED OUT. That is two things at
// once: the area has to say it is one (an unrecognised device is
// REFUSED, never formatted -- a swapon that ate a TFS3 partition would
// be the worst bug this file could have), and a zeroed PTE therefore
// cannot read as "swapped to slot 0".

#define SWAP_PAGE_SIZE        4096
#define SWAP_SECTORS_PER_PAGE (SWAP_PAGE_SIZE / 512)

// THE ON-DISK FORMAT, here rather than in swap.c because it is a
// format: a test forging a field, and any future host-side writer, must
// name it rather than an offset. Bumped only if the layout changes; a
// version this kernel does not know is refused, never assumed.
#define SWAP_MAGIC   "TOYSWAP"
#define SWAP_MAGIC_N 8          // including the NUL, so it is a C string
#define SWAP_VERSION 1

struct swap_header {
    char     magic[SWAP_MAGIC_N];
    uint32_t version;
    uint32_t slots;        // total, INCLUDING slot 0 (this header)
    uint32_t page_size;    // refuse a mismatch rather than reinterpret
};

// Writes the header, making `dev` a swap area and destroying whatever
// was on it. This is `mkswap`, and it is deliberately separate from
// swap_on() so that turning swap on can never reformat anything.
int swap_format(const struct block_device *dev, const char **why);

// Adopts an already-formatted area. Refuses a device with no header, a
// header it does not recognise, or one claiming more slots than the
// device has sectors for. *why is a short English reason on failure.
int swap_on(const struct block_device *dev, const char **why);

// Refuses while any slot is still allocated: those pages have nowhere
// to come back from, and dropping them would be silent corruption.
int swap_off(const char **why);

int swap_active(void);
const char *swap_device_name(void);   // "" when inactive

// 0 means "none free", which is why slot 0 is the header.
uint32_t swap_slot_alloc(void);
void     swap_slot_free(uint32_t slot);

// One page to or from a slot. `phys` is a physical address, used
// through the identity map, so any managed frame works and no temporary
// mapping is needed. Both return 1 on success.
int swap_write_page(uint32_t slot, uint64_t phys);
int swap_read_page(uint32_t slot, uint64_t phys);

// Slots including the header, and slots allocated. Both 0 when
// inactive. Either pointer may be NULL.
void swap_stats(uint32_t *out_total, uint32_t *out_used);

#endif
