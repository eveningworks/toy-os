// The x86-64 half of the kernel debugger -- see kdebug_arch.h. Linux
// splits KGDB the same way (arch/x86/kernel/kgdb.c beside
// kernel/debug/gdbstub.c).
#include "kdebug_arch.h"
#include "paging.h"   // paging_identity_limit()
#include "context_switch.h"

// isr.asm's frame, as uint64_t slots: r15 is regs[0], rax regs[14].
#define F_RIP    17
#define F_CS     18
#define F_RFLAGS 19
#define F_RSP    20
#define F_SS     21

#define RFLAGS_TF (1ULL << 8)
#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_RF (1ULL << 16)

// GDB register number -> frame slot; -1 is a live segment selector.
static const int8_t g_slot[KDB_NREGS] = {
    14, 13, 12, 11, 10, 9, 8, F_RSP,   // rax rbx rcx rdx rsi rdi rbp rsp
    7, 6, 5, 4, 3, 2, 1, 0,            // r8 .. r15
    F_RIP, F_RFLAGS, F_CS, F_SS,
    -1, -1, -1, -1,                    // ds es fs gs
};

int kdb_arch_reg_size(int n) { return n <= 16 ? 8 : 4; }

uint64_t kdb_arch_reg_get(const uint64_t *regs, int n) {
    if (n < 0 || n >= KDB_NREGS) return 0;
    if (g_slot[n] >= 0) return regs[g_slot[n]];
    uint16_t sel = 0;
    switch (n) {
    case 20: __asm__ volatile ("mov %%ds, %0" : "=r"(sel)); break;
    case 21: __asm__ volatile ("mov %%es, %0" : "=r"(sel)); break;
    case 22: __asm__ volatile ("mov %%fs, %0" : "=r"(sel)); break;
    default: __asm__ volatile ("mov %%gs, %0" : "=r"(sel)); break;
    }
    return sel;
}

// CS and SS are not writable: a wrong selector in an iretq frame is a
// #GP inside the return path, with the debugger no longer holding it.
void kdb_arch_reg_set(uint64_t *regs, int n, uint64_t v) {
    if (n < 0 || n >= KDB_NREGS || g_slot[n] < 0) return;
    if (n == 18 || n == 19) return;
    if (n == 17) v = (regs[F_RFLAGS] & ~0xFFFFFFFFULL) | (v & 0xFFFFFFFFULL);
    regs[g_slot[n]] = v;
}

uint64_t kdb_arch_ctx_reg(const struct kernel_context *k, int n, int *have) {
    *have = 1;
    switch (n) {   // GDB's numbering, as in g_slot above
    case 1:  return k->rbx;
    case 6:  return k->rbp;
    case 7:  return k->rsp;
    case 12: return k->r12;
    case 13: return k->r13;
    case 14: return k->r14;
    case 15: return k->r15;
    case 16: return k->rip;
    default: *have = 0; return 0;
    }
}

uint64_t kdb_arch_pc(const uint64_t *regs) { return regs[F_RIP]; }
void kdb_arch_set_pc(uint64_t *regs, uint64_t pc) { regs[F_RIP] = pc; }

// --- memory ------------------------------------------------------------

#define PTE_P  (1ULL << 0)
#define PTE_W  (1ULL << 1)
#define PTE_U  (1ULL << 2)
#define PTE_PS (1ULL << 7)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

#define CR0_WP   (1ULL << 16)
#define CR4_SMAP (1ULL << 21)

// The leaf mapping `va` in the current address space. W and U are ANDed
// down the walk, as the CPU does. 0 when anything on the way is absent.
static int walk(uint64_t va, uint64_t *flags, uint64_t *page_end) {
    uint64_t top = va >> 47;
    if (top != 0 && top != 0x1FFFF) return 0;   // non-canonical
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t table = cr3 & PTE_ADDR;
    uint64_t acc = PTE_W | PTE_U;
    for (int shift = 39; shift >= 12; shift -= 9) {
        // A table beyond the identity map cannot be dereferenced here.
        if (table >= paging_identity_limit()) return 0;
        uint64_t e = ((const uint64_t *)(uintptr_t)table)[(va >> shift) & 0x1FF];
        if (!(e & PTE_P)) return 0;
        acc &= e;
        if (shift == 12 || ((shift == 30 || shift == 21) && (e & PTE_PS))) {
            uint64_t size = 1ULL << shift;
            *flags = acc;
            *page_end = (va & ~(size - 1)) + size;
            return 1;
        }
        table = e & PTE_ADDR;
    }
    return 0;
}

static uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static void write_cr0(uint64_t v) { __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory"); }
static int smap_on(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return (v & CR4_SMAP) != 0;
}

// ONE ACCESS OF THE ASKED WIDTH when it is a naturally aligned 2, 4 or
// 8, so `x/w` on a device register is one 32-bit read -- a byte loop
// over MMIO reads something else, or has side effects four times.
static void copy_sized(volatile void *dst, const volatile void *src, uint64_t n) {
    uintptr_t a = (uintptr_t)src | (uintptr_t)dst;
    if (n == 8 && !(a & 7)) { *(volatile uint64_t *)dst = *(const volatile uint64_t *)src; return; }
    if (n == 4 && !(a & 3)) { *(volatile uint32_t *)dst = *(const volatile uint32_t *)src; return; }
    if (n == 2 && !(a & 1)) { *(volatile uint16_t *)dst = *(const volatile uint16_t *)src; return; }
    for (uint64_t i = 0; i < n; i++)
        ((volatile uint8_t *)dst)[i] = ((const volatile uint8_t *)src)[i];
}

// One contiguous access inside a page. SMAP forbids a supervisor touch
// of a user page unless AC is set, so it is set for exactly this copy.
static void mem_access(uint64_t va, void *buf, uint64_t n, int write, int user) {
    int ac = user && smap_on();
    if (ac) __asm__ volatile ("stac" ::: "memory");
    if (write) copy_sized((volatile void *)(uintptr_t)va, buf, n);
    else       copy_sized(buf, (const volatile void *)(uintptr_t)va, n);
    if (ac) __asm__ volatile ("clac" ::: "memory");
}

uint64_t kdb_arch_mem_read(uint64_t va, void *dst, uint64_t len) {
    uint64_t done = 0;
    while (done < len) {
        uint64_t flags, end, at = va + done;
        if (!walk(at, &flags, &end)) break;
        uint64_t n = end - at;
        if (n > len - done) n = len - done;
        mem_access(at, (uint8_t *)dst + done, n, 0, (flags & PTE_U) != 0);
        done += n;
    }
    return done;
}

uint64_t kdb_arch_mem_write(uint64_t va, const void *src, uint64_t len) {
    uint64_t done = 0;
    while (done < len) {
        uint64_t flags, end, at = va + done;
        if (!walk(at, &flags, &end)) break;
        int user = (flags & PTE_U) != 0, ro = !(flags & PTE_W);
        if (ro && user) break;
        uint64_t n = end - at;
        if (n > len - done) n = len - done;
        // Read-only kernel text, W^X's doing: WP off for this one copy.
        // Safe on one CPU with interrupts off; with SMP it needs the
        // others stopped first (docs/kdebug-design.md).
        uint64_t cr0 = 0;
        if (ro) { cr0 = read_cr0(); write_cr0(cr0 & ~CR0_WP); }
        mem_access(at, (uint8_t *)src + done, n, 1, user);
        if (ro) write_cr0(cr0);
        done += n;
    }
    return done;
}

void kdb_arch_breakpoint(void) { __asm__ volatile ("int3"); }

// --- debug registers ---------------------------------------------------

#define DR6_BS    (1ULL << 14)
#define DR6_CLEAR 0xFFFF0FF0ULL   // the architectural "nothing hit" value

enum kdb_trap kdb_arch_classify(uint64_t vector, int stepping, int *slot) {
    if (vector == 3) return KDB_TRAP_BREAK;
    if (vector == 2) return KDB_TRAP_NMI;
    if (vector != 1) return KDB_TRAP_NONE;
    uint64_t dr6;
    __asm__ volatile ("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile ("mov %0, %%dr6" :: "r"(DR6_CLEAR));
    for (int i = 0; i < KDB_HW_SLOTS; i++) {
        if (dr6 & (1ULL << i)) { *slot = i; return KDB_TRAP_HW; }
    }
    // A TF trap nobody asked for is a ring-3 program's own popf.
    if ((dr6 & DR6_BS) && stepping) return KDB_TRAP_STEP;
    return KDB_TRAP_NONE;
}

int kdb_arch_hw_valid(uint64_t addr, int type, int len) {
    if (type == 1) return len == 1;
    if (type != 2 && type != 4) return 0;
    if (len != 1 && len != 2 && len != 4 && len != 8) return 0;
    return (addr & (uint64_t)(len - 1)) == 0;
}

// DR7: a G bit per slot, then per slot 2 bits of RW (00 exec, 01 write,
// 11 read/write) and 2 of LEN (00 1, 01 2, 11 4, 10 8) from bit 16 up.
void kdb_arch_hw_install(const struct kdb_hw *s) {
    uint64_t dr7 = 0;
    for (int i = 0; i < KDB_HW_SLOTS; i++) {
        if (!s[i].used) continue;
        uint64_t rw = s[i].type == 1 ? 0 : s[i].type == 2 ? 1 : 3;
        uint64_t len = s[i].len == 1 ? 0 : s[i].len == 2 ? 1 : s[i].len == 8 ? 2 : 3;
        uint64_t a = s[i].addr;
        switch (i) {
        case 0: __asm__ volatile ("mov %0, %%dr0" :: "r"(a)); break;
        case 1: __asm__ volatile ("mov %0, %%dr1" :: "r"(a)); break;
        case 2: __asm__ volatile ("mov %0, %%dr2" :: "r"(a)); break;
        default: __asm__ volatile ("mov %0, %%dr3" :: "r"(a)); break;
        }
        dr7 |= (2ULL << (i * 2)) | (rw << (16 + i * 4)) | (len << (18 + i * 4));
    }
    __asm__ volatile ("mov %0, %%dr7" :: "r"(dr7));
}

void kdb_arch_hw_disable(void) {
    __asm__ volatile ("mov %0, %%dr7" :: "r"(0ULL));
}

// The IF a step masked. The trap (the next #DB) finishes the step, so
// there is only ever one outstanding.
static uint64_t g_step_if;

void kdb_arch_resume(uint64_t *regs, int step) {
    if (step) {
        g_step_if = regs[F_RFLAGS] & RFLAGS_IF;
        regs[F_RFLAGS] = (regs[F_RFLAGS] | RFLAGS_TF) & ~RFLAGS_IF;
    }
    regs[F_RFLAGS] |= RFLAGS_RF;
}

// TRAP: a stepped cli/sti/popf changed IF itself, and this puts back the
// value from before it. Linux's KGDB has the same blind spot.
void kdb_arch_step_done(uint64_t *regs) {
    regs[F_RFLAGS] = (regs[F_RFLAGS] & ~RFLAGS_TF) | g_step_if;
}
