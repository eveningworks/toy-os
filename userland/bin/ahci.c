// ahci -- the SATA host bus adapters: what each offers, the drive on
// every port that carries one, and what is on the ports it does not use.
//
// THE PORT TABLE IS THE POINT. "No drive" and "a drive this driver
// cannot speak to" look identical from `df`, and the signature is what
// tells them apart -- an ATAPI optical drive or a port multiplier
// reports a device on the link and is not a disk.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include <stdio.h>
#include <string.h>

// Ports are bounded by the hardware: PI is 32 bits wide, per HBA.
#define MAX_LIST (32 * 4)

static const char *signature_name(unsigned long long sig) {
    switch (sig) {
        case 0x00000101ull: return "SATA disk";
        case 0xEB140101ull: return "ATAPI";
        case 0xC33C0101ull: return "enclosure";
        case 0x96690101ull: return "port multiplier";
        case 0x00000000ull: return "";
        default:            return "unknown";
    }
}

static const char *speed_name(unsigned long long spd) {
    switch (spd) {
        case 1: return "1.5 Gbps";
        case 2: return "3 Gbps";
        case 3: return "6 Gbps";
        default: return "--";
    }
}

static void print_drive(const struct query_ahci_port *p) {
    char cap[16];
    human_size(cap, sizeof cap, p->sector_count * 512ull);
    printf("  %s: \"%s\" on port %llu\n", p->device[0] ? p->device : "(not registered)",
           p->model, (unsigned long long)p->port);
    printf("    capacity: %llu sectors (%s), LBA%s\n",
           (unsigned long long)p->sector_count, cap,
           (p->flags & QUERY_AHCI_PORT_LBA48) ? "48" : "28");
    printf("    transfers: DMA, up to %llu sectors each, %s\n",
           (unsigned long long)p->max_sectors_xfer,
           (p->flags & QUERY_AHCI_PORT_IRQ) ? "IRQ-driven" : "polled");
    // A wait from a process SLEEPS and lets the machine run; one from
    // the kernel itself still halts or polls (docs/blocking-design.md).
    printf("    waits that slept: %llu\n", (unsigned long long)p->cmd_sleeps);
    // Stated rather than left as a flag, like `ata` does: TRIM is
    // about what happens to the HOST IMAGE, not to throughput.
    printf("    TRIM: %s\n", (p->flags & QUERY_AHCI_PORT_TRIM)
           ? "in use -- freed blocks are discarded to the host image"
           : "not supported by this drive");
    // NCQ is used only for a BATCH -- the block layer's submit_batch,
    // whose one caller today is the filesystem's journal commit. One
    // request at a time needs no queue, so the round count is the honest
    // measure of how often it mattered.
    if (p->ncq_depth)
        printf("    NCQ: %llu tags; %llu batch(es) of %llu commands queued, %llu replayed one at a time\n",
               (unsigned long long)p->ncq_depth, (unsigned long long)p->ncq_rounds,
               (unsigned long long)p->ncq_cmds, (unsigned long long)p->ncq_fallbacks);
    else
        printf("    NCQ: not in use (the drive or its interrupt lacks it)\n");
}

// This HBA's ports, read whole from the list (every HBA's, in order).
static void print_hba(unsigned hba) {
    struct query_ahci_port p;
    int drives = 0;
    for (int i = 0; i < MAX_LIST; i++) {
        if (sys_query_record(QUERY_AHCI_PORT, (unsigned)i, &p, sizeof p) < (int)sizeof p) break;
        if (p.hba != hba || p.drive < 0) continue;
        print_drive(&p);
        drives++;
    }
    if (!drives) printf("  no SATA drive on any implemented port\n");

    int any = 0;
    for (int i = 0; i < MAX_LIST; i++) {
        if (sys_query_record(QUERY_AHCI_PORT, (unsigned)i, &p, sizeof p) < (int)sizeof p) break;
        if (p.hba != hba) continue;
        if (!any) {
            printf("  port  link       speed     signature\n");
            any = 1;
        }
        // PxSIG ONLY MEANS ANYTHING ONCE A DEVICE HAS SIGNALLED. An
        // empty port reads back its reset value (0xFFFF0101 on QEMU),
        // which decodes to a plausible-looking "unknown device".
        int device = (p.flags & QUERY_AHCI_PORT_DEVICE) != 0;
        char sig[24];
        if (device) snprintf(sig, sizeof sig, "0x%08llx", (unsigned long long)p.signature);
        else        snprintf(sig, sizeof sig, "--");
        char used[24] = "";
        if (p.flags & QUERY_AHCI_PORT_ACTIVE) snprintf(used, sizeof used, "  <- %s", p.device);
        printf("  %-4llu  %-9s  %-8s  %-10s  %s%s\n",
               (unsigned long long)p.port,
               device ? "device" : (p.det ? "link only" : "empty"),
               device ? speed_name(p.speed) : "--",
               sig,
               device ? signature_name(p.signature) : "",
               used);
    }
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { cmd_usage("ahci"); return 1; }

    struct query_ahci a;
    if (sys_query_record(QUERY_AHCI, 0, &a, sizeof a) < (int)sizeof a) {
        cmd_fail("ahci", 0);
        return 1;
    }

    if (!(a.flags & QUERY_AHCI_PRESENT)) {
        // A real answer, not a failure: this machine may have a legacy
        // IDE controller or a virtio disk instead.
        printf("ahci: no AHCI controller on this machine\n");
        return 0;
    }

    for (unsigned h = 0; sys_query_record(QUERY_AHCI, h, &a, sizeof a) >= (int)sizeof a; h++) {
        if (!(a.flags & QUERY_AHCI_PRESENT)) break;
        if (h) printf("\n");
        printf("ahci: HBA version %llu.%llu, %llu port%s implemented, %llu command slot%s, %s\n",
               (unsigned long long)((a.version >> 16) & 0xFFFF),
               (unsigned long long)((a.version >> 8) & 0xFF),
               (unsigned long long)a.ports_impl, a.ports_impl == 1 ? "" : "s",
               (unsigned long long)a.command_slots, a.command_slots == 1 ? "" : "s",
               (a.flags & QUERY_AHCI_IRQ) ? "IRQ-driven" : "polled");
        printf("  NCQ: %s; 64-bit addressing: %s\n",
               (a.flags & QUERY_AHCI_NCQ) ? "offered" : "not offered",
               (a.flags & QUERY_AHCI_64BIT) ? "offered, used for a queued buffer above 4 GiB"
                                            : "not offered");
        print_hba(h);
    }
    return 0;
}
