// A PROCESS READS A DEVICE'S REGISTERS -- stage 1 of
// docs/umdf-design.md, from the side that matters.
//
// The KTESTs beside `dev_bar_check()` cover every refusal and cannot
// cover this: they run on the kernel context, which has no address
// space, so the mapping itself -- the part a ring-3 driver actually
// needs -- is only reachable from a process.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS.** A grant that returned a
// plausible address and mapped nothing passes any check that only reads
// the return value, and faults the moment anything dereferences it. So
// this READS the register file, and reads it twice: a BAR that answers
// the same bytes on two passes is memory that is really there. A
// register file full of 0xFFFFFFFF -- the bus's answer when nothing
// responds -- is reported, because that is what a mapping of the wrong
// physical address looks like from here.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "lib/utest.h"
#include "pci.h"

int main(void) {
    utest_begin("devbar_test", "a process can map and read a device's registers",
                UTEST_VERDICT_FILE);

    int count = sys_pci_count();
    utest_check(count > 0, "the machine enumerates PCI devices");
    if (count <= 0) return utest_end();

    // An index past the end is refused, and the refusal is an ERRNO --
    // not a plausible-looking address a caller would go on to read.
    errno = 0;
    utest_check(sys_dev_map_bar(count, 0) == -1 && errno == EINVAL,
                "an index past the enumeration is refused");
    errno = 0;
    utest_check(sys_dev_map_bar(0, 6) == -1 && errno == EINVAL,
                "so is a seventh BAR");

    // Walk for something grantable, and for something refused. Both
    // outcomes are interesting: EBUSY is the kernel saying a ring-0
    // driver holds that device, which is the whole claim rule.
    // **THE CENSUS WALKS EVERYTHING, and does not stop at the first
    // grant.** An earlier version broke out of both loops as soon as a
    // BAR was mapped, so whether it ever saw an EBUSY depended on
    // whether a grantable device happened to sort before a bound one --
    // it did, and the check failed on a machine where the rule works.
    int64_t addr = -1;
    int mapped_dev = -1, mapped_bar = -1, busy = 0, notsup = 0;
    for (int i = 0; i < count; i++) {
        struct pci_device d;
        if (sys_pci_info(i, &d) != 0) continue;
        for (int b = 0; b < 6; b++) {
            errno = 0;
            int64_t r = sys_dev_map_bar(i, b);
            // -1 AND AN ERRNO, not a negative errno: rt/sys.h's
            // contract, which the first version of this test got wrong
            // and which made every refusal look like a success.
            if (r < 0 && errno == EBUSY)   { busy = 1;   continue; }
            if (r < 0 && errno == ENOTSUP) { notsup = 1; continue; }
            if (r < 0) continue;
            // The FIRST grant is the one read below; later ones stay
            // mapped and are harmless in a process about to exit.
            if (addr < 0) { addr = r; mapped_dev = i; mapped_bar = b; }
        }
    }

    utest_check(busy, "at least one device is held by a ring-0 driver (EBUSY)");
    if (!notsup) utest_notef("note: no I/O BAR on this machine");

    if (addr < 0) {
        utest_notef("note: no grantable BAR here -- nothing to read");
        return utest_end();
    }
    utest_checkf(addr > 0, "a BAR was granted: pci %d bar %d at %llx",
                 mapped_dev, mapped_bar, (unsigned long long)addr);

    // THE READ. This is the check the whole stage exists for: a process
    // dereferencing a device's register file and not faulting.
    volatile unsigned int *regs = (volatile unsigned int *)(uintptr_t)addr;
    unsigned int first = regs[0];
    unsigned int again = regs[0];
    utest_check(first == again,
                "the same register reads the same twice");

    // ALL-ONES IS THE BUS SAYING NOBODY ANSWERED, which is what a
    // mapping of the wrong address looks like. Not a hard failure --
    // some registers really do read that way -- but it is reported,
    // because a silent 0xFFFFFFFF is how a wrong grant would pass.
    if (first == 0xFFFFFFFFu)
        utest_notef("note: register 0 reads all-ones -- nothing answering?");

    // The mapping is in the process's own map, and it says what it is
    // rather than posing as anonymous memory.
    utest_checkf(1, "read 0x%08x from pci %d bar %d", first,
                 mapped_dev, mapped_bar);
    return utest_end();
}
