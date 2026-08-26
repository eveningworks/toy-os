// Thread-local storage in ring 3: laying out one thread's TLS block and
// pointing %fs at it.
//
// THE KERNEL OWNS ONE NUMBER -- FS.base, set by SYS_SET_TLS and
// reloaded on every switch. Everything the number points AT is decided
// here, which is the same division Linux makes: arch_prctl(ARCH_SET_FS)
// is a register write, and glibc owns the layout.
//
// The layout is the x86-64 psABI's variant II, and the shape is the
// part worth knowing:
//
//     +------------------- block ------------------+--- TCB ---+
//     | .tdata copy          | .tbss zeros         | self ptr  |
//     +--------------------------------------------+-----------+
//     ^ base                                       ^ tp = %fs.base
//
// The thread pointer sits at the END of the block, and a `__thread`
// variable is read at a NEGATIVE offset from it -- the linker computes
// each one as (its offset in the TLS segment) - (the block's size), so
// the block must be exactly `__tls_memsz` bytes long and end at tp.
// Getting the size wrong does not fail: it silently shifts every
// variable, and the neighbouring bytes are the TCB.
//
// The self-pointer at tp[0] is what makes `%fs:0` read tp itself. This
// build never needs it -- the stack-protector guard is `global` here,
// so nothing reads %fs:0x28 -- but it costs one store and it is what
// any ported code expects to find.
#include "rt/sys.h"
#include <stdint.h>

// From userland/rt/link.ld. The ADDRESS of each is the value.
extern char __tls_template[];
extern char __tls_filesz[];
extern char __tls_memsz[];
extern char __tls_align[];

#define TLS_MAX_ALIGN 16 // what an allocation here is guaranteed to give
#define TCB_SIZE      64 // tp[0] is the self pointer; the rest is headroom

// A LINKER SYMBOL'S ADDRESS IS A NUMBER, AND GCC DOES NOT BELIEVE THAT.
// The address of a declared object cannot be null, so a loop bounded by
// one of these is compiled BOTTOM-TESTED -- and a `filesz` of 0 then
// counts to 2^64, which is a page fault in every ring-3 program a few
// thousand bytes past the buffer. The empty asm makes the value opaque
// again and costs no instruction.
static uint64_t linker_value(const char *sym) {
    uint64_t v = (uint64_t)(uintptr_t)sym;
    __asm__ ("" : "+r"(v));
    return v;
}

// THE SIZE THE LINKER USED, which is the size the block must be: every
// `%fs:offset` in the program was resolved against `memsz` rounded up
// to the segment's own alignment, so rounding to anything else here
// silently shifts the whole block under the offsets that read it.
static uint64_t tls_block_size(void) {
    uint64_t memsz = linker_value(__tls_memsz);
    uint64_t align = linker_value(__tls_align);
    if (align < 1) align = 1;
    return (memsz + align - 1) & ~(align - 1);
}

uint64_t rt_tls_size(void) { return tls_block_size() + TCB_SIZE; }

void *rt_tls_install(void *mem) {
    uint64_t block = tls_block_size();
    char *base = (char *)mem;
    char *tp   = base + block;

    uint64_t filesz = linker_value(__tls_filesz);
    for (uint64_t i = 0; i < filesz; i++) base[i] = __tls_template[i];
    for (uint64_t i = filesz; i < block; i++) base[i] = 0;
    for (int i = 0; i < TCB_SIZE; i++) tp[i] = 0;
    *(void **)tp = tp; // %fs:0 reads the thread pointer, as variant II says
    return tp;
}

// THE INITIAL THREAD'S BLOCK IS STATIC, and it has to be: the first
// thing that would allocate one is malloc(), and malloc() sets errno on
// failure -- which is itself a `__thread` variable, so a heap-allocated
// bootstrap block would be written through a %fs that is still 0.
//
// Sized generously against what this build actually uses (errno, four
// bytes). A program that outgrows it says so at startup rather than
// corrupting whatever follows.
#define TLS_STATIC_MAX 1024
static char g_main_tls[TLS_STATIC_MAX + TCB_SIZE] __attribute__((aligned(TLS_MAX_ALIGN)));

void __rt_tls_init(void) {
    if (linker_value(__tls_align) > TLS_MAX_ALIGN) {
        static const char msg[] =
            "rt: this program's thread-local storage is aligned past 16\n";
        sys_write(2, msg, sizeof msg - 1);
        sys_exit(127);
    }
    if (rt_tls_size() > sizeof g_main_tls) {
        static const char msg[] =
            "rt: this program's thread-local storage exceeds TLS_STATIC_MAX\n";
        sys_write(2, msg, sizeof msg - 1);
        sys_exit(127);
    }
    sys_set_tls(rt_tls_install(g_main_tls));
}
