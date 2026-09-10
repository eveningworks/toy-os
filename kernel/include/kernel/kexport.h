#ifndef KEXPORT_H
#define KEXPORT_H

// WHAT A LOADABLE MODULE MAY LINK AGAINST: the kernel's export table.
//
// A module (kernel/core/module.c) is a relocatable object whose
// undefined symbols are resolved here, and ONLY here -- Linux's
// EXPORT_SYMBOL, without modversions or namespaces. A module that
// reaches for anything unexported fails to load with the symbol NAMED,
// which is the point: the list is the module ABI, and it is deliberately
// short. Every entry lives in kernel/core/kexports.c, grouped by header,
// so `grep EXPORT_SYMBOL kernel/core/kexports.c` is the whole contract.
//
// An entry is two pointers into a linker section (`.kexports`), the
// mechanism .drivers/.initcalls use; the address is fixed up by the
// boot relocation like any other absolute reference, so the table holds
// RUNTIME addresses whatever KASLR chose.
struct kexport {
    const char *name;
    const void *addr;
} __attribute__((aligned(16)));

#define EXPORT_SYMBOL(sym)                                                    \
    static const struct kexport kexport_##sym                                 \
        __attribute__((used, section(".kexports"))) = {                       \
            .name = #sym, .addr = (const void *)&sym,                         \
        }

// The runtime address of an exported symbol, or NULL. Linear -- the
// table is a few dozen entries and a load is not a hot path.
const void *kexport_lookup(const char *name);
int kexport_count(void);
const struct kexport *kexport_at(int i);

#endif
