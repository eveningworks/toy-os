// A PROCESS TAKES A DEVICE OFF THE KERNEL -- stage 2 of
// docs/umdf-design.md, from the side that matters. Stage 1 could only
// ever be handed a device no ring-0 driver wanted.
//
// **WHAT A BROKEN VERSION WOULD STILL PASS.** A claim that recorded
// nothing, and a dev_bar_check() that never consulted it, would leave
// every "the map succeeded" check green -- stage 1's test already
// passed without a claim existing at all. So every assertion here is
// about the DIFFERENCE a claim makes on ONE device and ONE bar: the
// same call refused before the claim (EACCES), granted after it,
// refused again after the release. Nothing about the device changes
// between those three lines, so nothing but the claim can explain them.
//
// The HDA leg is the one that proves the kernel LET GO, and it only
// runs on a machine that has an HD Audio controller -- tools/
// devclaim_test.py launches one deliberately. It is restricted to
// class 04:03 ON PURPOSE: the NIC drivers are releasable too, and a
// test that claimed whatever happened to be removable would unbind
// the network under whatever else the machine is doing.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>   // usleep -- a driver in a PROCESS may sleep
#include "rt/sys.h"
#include "lib/utest.h"
#include "pci.h"
#include "syscall_abi.h"

#define HDA_CLASS 0x04
#define HDA_SUBCLASS 0x03

int main(void) {
    utest_begin("devclaim_test", "a process can take a PCI device off the kernel",
                UTEST_VERDICT_FILE);

    int count = sys_pci_count();
    utest_check(count > 0, "the machine enumerates PCI devices");
    if (count <= 0) return utest_end();

    errno = 0;
    utest_check(sys_dev_claim(count) == -1 && errno == EINVAL,
                "an index past the enumeration cannot be claimed");
    errno = 0;
    utest_check(sys_dev_release(count, 0) == -1 && errno == EINVAL,
                "nor released");

    // --- the difference a claim makes, on a device nobody was driving.
    int free_dev = -1, free_bar = -1;
    for (int i = 0; i < count && free_dev < 0; i++) {
        struct pci_device d;
        if (sys_pci_info(i, &d) != 0) continue;
        for (int b = 0; b < 6; b++) {
            errno = 0;
            if (sys_dev_map_bar(i, b) != -1) continue;   // already ours?
            if (errno != EACCES) continue;               // bound, I/O, or empty
            free_dev = i; free_bar = b;
            break;
        }
    }

    if (free_dev < 0) {
        utest_notef("note: no unbound memory BAR here -- the claim leg cannot run");
    } else {
        utest_checkf(1, "pci %d bar %d is unbound and unclaimed", free_dev, free_bar);
        // UNCLAIMED IS NOT A FREE-FOR-ALL. This is the line stage 2
        // added: before it, an unbound device could be mapped by
        // anybody, twice over.
        errno = 0;
        utest_check(sys_dev_map_bar(free_dev, free_bar) == -1 && errno == EACCES,
                    "an unclaimed device cannot be mapped");

        utest_check(sys_dev_claim(free_dev) == 0, "it can be claimed");
        utest_check(sys_dev_claim(free_dev) == 0, "and claiming twice is idempotent");

        int64_t addr = sys_dev_map_bar(free_dev, free_bar);
        utest_checkf(addr > 0, "the SAME bar now maps, at %llx",
                     (unsigned long long)addr);

        utest_check(sys_dev_release(free_dev, 0) == 0, "the claim can be dropped");
        errno = 0;
        utest_check(sys_dev_map_bar(free_dev, free_bar) == -1 && errno == EACCES,
                    "and the bar goes back to being unmappable");
        errno = 0;
        utest_check(sys_dev_release(free_dev, 0) == -1 && errno == EACCES,
                    "releasing what we do not hold is refused");
    }

    // --- taking a device the KERNEL was driving. The payoff.
    int hda = -1;
    for (int i = 0; i < count; i++) {
        struct pci_device d;
        if (sys_pci_info(i, &d) != 0) continue;
        if (d.class_code == HDA_CLASS && d.subclass == HDA_SUBCLASS) { hda = i; break; }
    }
    if (hda < 0) {
        utest_notef("note: no HD Audio controller here -- run under "
                    "tools/devclaim_test.py for the unbind leg");
        return utest_end();
    }

    // Bound, so stage 1's rule refuses it: this is the state every
    // interesting device was permanently in before stage 2.
    errno = 0;
    utest_check(sys_dev_map_bar(hda, 0) == -1 && errno == EBUSY,
                "the HDA controller is held by a ring-0 driver");

    utest_check(sys_dev_claim(hda) == 0, "and it can be taken off the kernel");

    int64_t bar0 = sys_dev_map_bar(hda, 0);
    utest_checkf(bar0 > 0, "its register file maps at %llx",
                 (unsigned long long)bar0);
    if (bar0 > 0) {
        volatile unsigned char *r = (volatile unsigned char *)(uintptr_t)bar0;
        volatile unsigned int *gctl = (volatile unsigned int *)(uintptr_t)(bar0 + 0x08);

        // **THE KERNEL HANDS OVER A CONTROLLER IN RESET**, because
        // hda_remove() writes GCTL.CRST low on its way out -- and a
        // controller in reset reads its whole register file as zero.
        // So the first thing this does is what a driver does: bring it
        // up. THIS IS A WRITE, which is the stronger half of the
        // proof -- a read-only mapping, or one of the wrong page,
        // cannot make a device change its own status bits.
        utest_check((*gctl & 1) == 0, "the controller was handed over in reset");
        *gctl = 1;                       // CRST high
        int spins = 0;
        while (!(*gctl & 1) && spins < 1000000) spins++;
        utest_checkf(*gctl & 1, "ring 3 brought it out of reset (%d spins)", spins);
        // AND THEN IT SLEEPS, which is the thing a ring-3 driver can do
        // and `hda_probe()` cannot: CRST reading high is the controller
        // accepting the write, not the link being up, and GCAP reads
        // back 0 until the codecs have come out of reset (the spec's 25
        // frames). The kernel spends 30 ms of a syscall here; a process
        // just blocks.
        usleep(50000);

        // GCAP at +0x00, VMIN at +0x02, VMAJ at +0x03 -- the first
        // three registers of every HD Audio controller. THE VERSION IS
        // WHAT MAKES THIS A REAL READ: 1.0 is a specific pair of bytes
        // that a mapping of the wrong page, or of nothing at all,
        // would not produce.
        //
        // **EACH ONE AT ITS OWN WIDTH.** GCAP is 16 bits, and reading
        // it as two bytes gave 0x0001 against the kernel's 0x4401 --
        // the byte at +1 does not decode, because a device models a
        // register, not memory.
        unsigned gcap = *(volatile unsigned short *)(uintptr_t)bar0;
        unsigned vmin = r[2], vmaj = r[3];
        utest_checkf(vmaj == 1 && vmin == 0,
                     "the codec answers HD Audio %u.%u (gcap %#x)", vmaj, vmin, gcap);
        utest_check(gcap != 0xFFFF, "gcap is not the bus's all-ones");
        // An output stream is what stage 3 will go on to drive.
        utest_checkf((gcap >> 12) & 0xF, "it reports %u output stream(s)",
                     (gcap >> 12) & 0xF);
    }

    // AND GIVE IT BACK, which is the half a sticky unbind cannot do.
    // After the rebind the kernel driver holds it again, so the same
    // call that worked a line ago is EBUSY rather than EACCES -- two
    // different refusals, and only the rebind produces the first.
    utest_check(sys_dev_release(hda, DEV_RELEASE_REBIND) == 0,
                "it can be handed back");
    errno = 0;
    utest_check(sys_dev_map_bar(hda, 0) == -1 && errno == EBUSY,
                "and the ring-0 driver has it again");
    return utest_end();
}
