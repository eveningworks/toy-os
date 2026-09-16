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
#include "toyabi.h"
#include "rt/sys.h"
#include <stdint.h>

// From userland/rt/link.ld: the template's address, and its geometry
// as DATA -- link.ld writes three QUADs into .rodata. They were *ABS*
// symbols whose address was the value, until -fpie: RIP-relative
// addressing can only name something inside the image, so a value like
// 16 stopped linking. Values read from memory are also bounds GCC
// believes, which retired the linker_value() laundering that used to
// live here (a loop bounded by a symbol's "address" was compiled
// bottom-tested, and a 0 counted to 2^64).
extern char __tls_template[];
struct rt_tlsdesc { uint64_t filesz, memsz, align; };
extern const struct rt_tlsdesc __rt_tlsdesc;

// What the PROGRAM side of the ABI contract says it was built against.
// Here rather than in a file of its own because this is the runtime
// object already linked into every executable, and because the geometry
// just above is exactly what a skew gets wrong: the crash that made
// this necessary was a write through a TLS block whose size the program
// and the library disagreed about. /lib/ld-toy.so compares it with
// libc.so's `__toy_abi_provided` (abi/toyabi.h).
const struct toy_abi_stamp __toy_abi_required = {
    TOY_ABI_MAGIC, TOY_ABI_VERSION,
};

#define TLS_MAX_ALIGN 16 // what an allocation here is guaranteed to give
#define TCB_SIZE      64 // tp[0] is the self pointer; the rest is headroom

// THE SIZE THE LINKER USED, which is the size the block must be: every
// `%fs:offset` in the program was resolved against `memsz` rounded up
// to the segment's own alignment, so rounding to anything else here
// silently shifts the whole block under the offsets that read it.
static uint64_t tls_block_size(void) {
    uint64_t memsz = __rt_tlsdesc.memsz;
    uint64_t align = __rt_tlsdesc.align;
    if (align < 1) align = 1;
    return (memsz + align - 1) & ~(align - 1);
}

uint64_t rt_tls_size(void) { return tls_block_size() + TCB_SIZE; }

void *rt_tls_install(void *mem) {
    uint64_t block = tls_block_size();
    char *base = (char *)mem;
    char *tp   = base + block;

    uint64_t filesz = __rt_tlsdesc.filesz;
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
    if (__rt_tlsdesc.align > TLS_MAX_ALIGN) {
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
