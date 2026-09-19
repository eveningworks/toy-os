// netheal -- reboot once when the machine comes up with no network.
//
// THE FAULT THIS EXISTS FOR IS NOT A NETWORK FAULT. The USB NIC on the
// bare-metal ASUS sometimes enumerates at FULL speed instead of high
// speed, wedged, and `address device` then fails for the rest of the
// boot (docs/bugs.md, "A SUPERSPEED DEVICE CAN LAND ON THE USB2
// COMPANION PORT"). The machine boots perfectly and is simply
// unreachable -- which is the expensive part, because it is headless
// and nobody can look at it precisely when something is wrong.
//
// Every software lever has been tried on that controller and measured:
// a port reset, a warm reset of the SuperSpeed companion, and the
// Intel port-mux cycle all leave the device wedged, and PPC=0 means
// there is no VBUS switch to cycle. What DOES clear it, measured, is a
// reboot. So this turns a stranded machine into one that rescues
// itself, which is not a fix and does not pretend to be: the bug entry
// stays open, and this only stops it costing a trip to the machine.
//
// WHY A COUNTER ON DISK IS THE WHOLE DESIGN. A reboot that does not
// help must not become a reboot loop -- on a machine with genuinely no
// NIC (a VM with `--net none`) every boot would qualify, forever. So
// the attempts are counted in a file, the count survives the reboot it
// causes, and it is CLEARED the moment a boot has an address. Two
// attempts, then it gives up and says so.
//
// OFF BY DEFAULT, and one switch rather than two: the service is always
// started and idles until `system.net_recover` is on, which is ntpd's
// shape in this same directory. A desktop that reboots itself unasked
// would be alarming; this is for a headless test machine.

#include "rt/sys.h"
#include "lib/usetting.h"
#include "query.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define STATE_PATH   "/var/lib/netheal"
#define MAX_ATTEMPTS 2
#define DEFAULT_WAIT 45    // seconds from start before giving a verdict

static int have_address(void) {
    struct query_netdev d;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (d.ip) return 1;
    }
    return 0;
}

// The count survives the reboot it causes, which is the only reason it
// is on disk rather than in memory.
static int attempts_read(void) {
    int fd = open(STATE_PATH, O_RDONLY);
    if (fd < 0) return 0;
    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    int v = atoi(buf);
    return v < 0 ? 0 : v;
}

static void attempts_write(int v) {
    // /var/lib may not exist yet: it is created on first use, like
    // /var/crash and /var/games beside it (docs/filesystem-layout.md).
    mkdir("/var/lib", 0755);
    int fd = open(STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%d\n", v);
    if (n > 0) write(fd, buf, (size_t)n);
    // FSYNC, because the very next thing this program does is reboot
    // the machine. A count still in the write-back cache is a count
    // that never happened, and the loop this exists to prevent would
    // be back.
    fsync(fd);
    close(fd);
}

int main(void) {
    char on[16] = {0};
    if (!usetting_get("system.net_recover", on, sizeof on) ||
        strcmp(on, "on") != 0) {
        return 0;   // idle, like ntpd with system.ntp off
    }

    int wait_s = DEFAULT_WAIT;
    int v = 0;
    if (usetting_get_int("system.net_recover_wait", &v) && v >= 5 && v <= 600)
        wait_s = v;

    // POLL RATHER THAN SLEEP THE WHOLE WAIT. DHCP usually lands in the
    // first few seconds, and a boot that is fine should clear its
    // counter promptly rather than after the full timeout -- otherwise
    // a reboot for an unrelated reason inside the window would find a
    // stale count and spend one of the two attempts.
    for (int t = 0; t < wait_s; t++) {
        if (have_address()) {
            if (attempts_read() != 0) attempts_write(0);
            return 0;
        }
        sleep(1);
    }

    int tries = attempts_read();
    if (tries >= MAX_ATTEMPTS) {
        // LOUD, and then it stops. A machine that has rebooted twice
        // without getting an address has something this cannot fix, and
        // saying so once is more use than rebooting forever.
        fprintf(stderr, "netheal: still no address after %d reboot(s) -- "
                        "giving up; see docs/bugs.md for the USB NIC wedge\n",
                tries);
        return 1;
    }

    attempts_write(tries + 1);
    fprintf(stderr, "netheal: no address %ds after boot -- rebooting "
                    "(attempt %d of %d)\n", wait_s, tries + 1, MAX_ATTEMPTS);
    char *argv[] = { "reboot", 0 };
    execv("/bin/reboot", argv);
    fprintf(stderr, "netheal: could not run /bin/reboot\n");
    return 1;
}
