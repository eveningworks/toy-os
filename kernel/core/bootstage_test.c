// KTESTs for the boot-order guard. See kernel/include/kernel/bootstage.h.
//
// These cannot test the PANIC -- it halts the machine, which is the
// point of it, so the failing case is verified by a positive control at
// the source (put a pmm_alloc_contiguous() before pmm_init() and the
// boot smoke test reports "PANIC: pmm_alloc_contiguous ran before
// pmm_init()") rather than from inside the suite.
//
// What they DO cover is the half that rots silently: a subsystem whose
// init stopped marking itself up. Nothing else would notice, because
// the guard's whole design is to be invisible when everything is in
// order.
#include "bootstage.h"
#include "ktest.h"

KTEST("bootstage", "every guarded subsystem marked itself up during boot") {
    // If either of these fails, the guard is inert -- and an inert
    // guard is worse than none, because the next ordering bug goes back
    // to presenting as a device fault while this file says it is
    // covered.
    KTEST_ASSERT(boot_subsystem_is_up(BOOT_SUB_PCI));
    KTEST_ASSERT(boot_subsystem_is_up(BOOT_SUB_PMM));

    // Asking about a bit nothing defines is false, not true -- an
    // is_up() that answered "yes" to everything would pass the two
    // assertions above with the flags never set at all.
    KTEST_ASSERT_EQ(boot_subsystem_is_up(1u << 31), 0);

    // A multi-bit query means "all of these", not "any of them".
    KTEST_ASSERT(boot_subsystem_is_up(BOOT_SUB_PCI | BOOT_SUB_PMM));
    KTEST_ASSERT_EQ(boot_subsystem_is_up(BOOT_SUB_PMM | (1u << 31)), 0);
}
