#ifndef INITCALL_H
#define INITCALL_H

#include <stdint.h>

// INITCALLS: how a subsystem, a driver or a provider gets its init()
// run at boot -- by DECLARING it, at file scope, into a linker section
// (`.initcalls`, the mechanism KTEST and DRIVER_DECLARE use), instead of
// by a line in kernel_main(). Linux's module_init()/initcall levels,
// sized for here: the levels below are the ORDER; within a level the
// order is link order, which the Makefile fixes (`find | sort`) and
// nothing may depend on. A real dependency gets a level.
//
// kernel_main() keeps the genuinely sequential bring-up by hand (serial,
// ACPI, PCI, pmm, GDT/IDT, LAPIC, clocks, the heap) and walks these
// levels after it. BOOT_REQUIRE() (bootstage.h) is still what makes a
// wrong order LOUD; this only removes the list. See docs/decisions.md.
enum init_level {
    INIT_CORE = 0, // a class core that owns a table others register into (net)
    INIT_BUS,      // buses and transports: AHCI, xHCI, the virtio devices
    INIT_DEVICE,   // devices on those buses' peers: NICs, sound cards
    INIT_FS,       // the filesystem
    INIT_CONFIG,   // readers of /etc -- needs INIT_FS
    INIT_QUERY,    // SYS_QUERY providers -- before settings_init()
    INIT_LEVELS,
};

struct initcall {
    void (*fn)(void);
    const char *name;
    const char *file;
    uint32_t level;
} __attribute__((aligned(32)));

// Declares `fn` (a `void fn(void)`) to run at `lvl`. File scope.
#define INITCALL(f_, lvl)                                                     \
    static const struct initcall initcall_##f_                                \
        __attribute__((used, section(".initcalls"))) = {                      \
            .fn = f_, .name = #f_, .file = __FILE__, .level = (lvl),          \
        }

// Runs every initcall declared at `level`, in link order, and logs one
// line. Called from kernel_main(), once per level, in level order.
void initcalls_run(enum init_level level);

// For `lsdrv`-style reporting and the KTEST: how many are declared, and
// whether each has run.
int initcall_count(void);
const struct initcall *initcall_at(int i);
int initcall_ran(int i);

#endif
