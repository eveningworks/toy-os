// Builds a real GDT (the tiny one boot.asm sets up is just enough to
// reach long mode) plus a TSS, and loads both. This is the prerequisite
// for ever running anything in ring 3: without a valid TSS.RSP0, the
// first interrupt that fires while the CPU is in ring 3 has no valid
// kernel stack to switch to and the machine triple-faults immediately.
#include "gdt.h"
#include <stddef.h>

// Access-byte/flag bits for 64-bit code/data descriptors. Base and limit
// are ignored by the CPU for code/data segments in long mode (paging is
// what actually enforces addressing), so only these bits matter.
#define GDT_WRITABLE   (1ULL << 41) // data segments only
#define GDT_EXECUTABLE (1ULL << 43) // code segments only
#define GDT_S_CODEDATA (1ULL << 44) // "normal" segment, not a system one
#define GDT_PRESENT    (1ULL << 47)
#define GDT_LONG_MODE  (1ULL << 53) // code segments only: 64-bit
#define GDT_DPL3       (3ULL << 45)

#define KERNEL_CODE_DESC (GDT_EXECUTABLE | GDT_S_CODEDATA | GDT_PRESENT | GDT_LONG_MODE)
#define KERNEL_DATA_DESC (GDT_WRITABLE | GDT_S_CODEDATA | GDT_PRESENT)
#define USER_DATA_DESC   (GDT_WRITABLE | GDT_S_CODEDATA | GDT_PRESENT | GDT_DPL3)
#define USER_CODE_DESC   (GDT_EXECUTABLE | GDT_S_CODEDATA | GDT_PRESENT | GDT_LONG_MODE | GDT_DPL3)

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// index: 0 null, 1 kernel code, 2 kernel data, 3 user data, 4 user code,
// 5-6 TSS (a system-segment descriptor is 16 bytes -- two slots).
static uint64_t gdt[7];
static struct gdt_ptr gdtp;
static struct tss tss;

// A dedicated kernel stack used only for the switch back to ring 0 when
// an interrupt/exception fires while running in ring 3.
static uint8_t kernel_stack0[8192] __attribute__((aligned(16)));

// The DOUBLE FAULT stack (IST slot 1, i.e. tss.ist[0]).
//
// A #DF is raised when the CPU cannot deliver a fault -- and the case
// that matters here is a kernel stack overflow: RSP walks onto the
// guard page, the push faults, and delivering that #PF needs to push
// again onto the same broken stack. Without an IST the second push
// faults too and the CPU triple-faults, which reboots the machine with
// nothing printed. With one, the #DF handler runs on a known-good stack
// and can say what happened.
//
// Its own stack rather than kernel_stack0: the whole point is that it
// must be usable when the current stack is not.
static uint8_t df_stack[8192] __attribute__((aligned(16)));

static void set_tss_descriptor(uint64_t *desc_lo, uint64_t *desc_hi, uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (uint64_t)(base & 0xFFFF) << 16;
    low |= (uint64_t)((base >> 16) & 0xFF) << 32;
    low |= (uint64_t)0x89 << 40; // present, DPL0, type=9 (available 64-bit TSS)
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;

    *desc_lo = low;
    *desc_hi = (base >> 32) & 0xFFFFFFFFULL; // upper 32 bits of base; rest reserved (0)
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = KERNEL_CODE_DESC;
    gdt[2] = KERNEL_DATA_DESC;
    gdt[3] = USER_DATA_DESC;
    gdt[4] = USER_CODE_DESC;

    for (size_t i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    tss.rsp0 = (uint64_t)(kernel_stack0 + sizeof(kernel_stack0));
    // IST slot 1 (the descriptor's IST field is 1-based, this array is
    // not). idt_init() points the #DF gate at it.
    tss.ist[0] = (uint64_t)(df_stack + sizeof(df_stack));
    tss.iomap_base = sizeof(struct tss); // beyond the limit => no IO bitmap

    set_tss_descriptor(&gdt[5], &gdt[6], (uint64_t)&tss, sizeof(struct tss) - 1);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (uint64_t)&gdt;

    __asm__ volatile ("lgdt %0" : : "m"(gdtp));

    __asm__ volatile (
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : "i"(SEL_KERNEL_DATA)
        : "ax"
    );

    // CS can't be loaded with a plain mov -- reload it via a far return.
    __asm__ volatile (
        "pushq %0\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "i"(SEL_KERNEL_CODE)
        : "rax"
    );

    __asm__ volatile ("ltr %0" : : "r"((uint16_t)SEL_TSS));
}

void gdt_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
