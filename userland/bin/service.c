// service -- start, stop and inspect the services init supervises.
//
// Before this, the only lever on a running init was `rm`ing a
// descriptor from /etc/services.d and killing the pid by hand -- which
// says "never start this again", not "stop it for now", and which
// nothing could report on afterwards.
//
// THE READ HALF AND THE WRITE HALF REACH INIT DIFFERENTLY, and that is
// the design rather than an accident:
//
//   list/status read /run/init.status, which init rewrites whenever
//   anything changes. They ask init nothing. It is the only source that
//   can say a service is down ON PURPOSE -- the process table shows an
//   absence, and an absence cannot tell "stopped" from "crash-looping"
//   from "never declared".
//
//   start/stop append a line to /run/init.ctl and then send SIGHUP to
//   init, which is what wakes it out of waitpid(-1) to read the file.
//   runit's `supervise/control` plus SysV's `kill -HUP 1`; systemd's
//   D-Bus and /run/initctl's FIFO both need transports this system has
//   not got.
//
// IT FINDS INIT RATHER THAN ASSUMING PID 1. init holds pid 1 because it
// is spawned first, not because anything enforces the number, and the
// kernel itself asks scheduler_init_pid() rather than testing `pid ==
// 1` for exactly this reason. A boot with no init then produces a
// message saying so instead of a signal sent to whatever is in slot 0.
//
// A REQUEST IS NOT A RESULT, so this waits for one. init acts on the
// next pass of its loop, which the signal has just triggered; polling
// the status file until the state changes turns `service stop toywm`
// into something whose OUTCOME is on screen, rather than a program that
// prints "requested" and leaves the operator to run `ps`.
#include <stdint.h>
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include "etc_config.h"
#include "lib/uchan.h"
#include "lib/uinitctl.h"
#include "syscall_abi.h"
#include <fcntl.h>
#include <unistd.h>
#include "tmppath.h"

#define CONTROL_PATH TMP_RUNDIR "/init.ctl"
#define STATUS_PATH  TMP_RUNDIR "/init.status"

// How long to wait for init's answer over the channel. Generous: init
// acts on its next pass, and a machine mid-backoff answers late rather
// than never -- the same reason SETTLE_MS is what it is.
#define CHAN_REPLY_MS 4000
#define SERVICES_DIR "/etc/services.d"

// How long to wait for init to act on a request before reporting
// whatever the status file says anyway. Generous against a machine
// where init is blocked behind a slow spawn, and bounded because a
// request that init never sees must still end with this program
// exiting and saying what it found.
#define SETTLE_MS    3000
#define SETTLE_STEP  50

static char g_status[8192];   // init.c's SVC_STATUS_MAX: every row at SVC_MAX

// Defined below, beside init_pid(), which it needs: the status file,
// asking init to publish one if it has not.
static const char *read_status(void);

// The status file, or 0 if it is not there.
static const char *read_status_file(void) {
    int fd = open(STATUS_PATH, O_RDONLY);
    if (fd < 0) return 0;
    int64_t n = read(fd, g_status, sizeof g_status - 1);
    close(fd);
    if (n < 0) return 0;
    g_status[n] = '\0';
    return g_status;
}

// Copies the whitespace-separated field `want` (0-based) of `line` into
// `out`. Fields, not columns: init writes the file padded for a human
// reading it with `cat`, and depending on the padding would make the
// widths a second thing that has to stay true.
static int field(const char *line, int want, char *out, unsigned long cap) {
    int idx = 0;
    const char *p = line;
    out[0] = '\0';
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') return 0;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (idx == want) {
            unsigned long len = (unsigned long)(p - start);
            if (len >= cap) len = cap - 1;
            memcpy(out, start, len);
            out[len] = '\0';
            return 1;
        }
        idx++;
    }
}

// The status line for `name`, or 0. The caller owns nothing: the
// pointer is into g_status, which read_status() refills.
static const char *status_line(const char *status, const char *name) {
    char field0[64];
    const char *p = status;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) p++;
        if (*line == '#') continue;
        if (field(line, 0, field0, sizeof field0) && strcmp(field0, name) == 0)
            return line;
    }
    return 0;
}

// The state word (field 1) of a service, or "" when the status file has
// no line for it.
static void state_of(const char *name, char *out, unsigned long cap) {
    out[0] = '\0';
    const char *status = read_status();
    if (!status) return;
    const char *line = status_line(status, name);
    if (line) field(line, 1, out, cap);
}

static void print_line(const char *line) {
    char buf[160];
    unsigned long n = 0;
    while (line[n] && line[n] != '\n' && n < sizeof buf - 2) { buf[n] = line[n]; n++; }
    buf[n++] = '\n';
    buf[n] = '\0';
    sys_print(buf);
}

// init's pid, or 0. Identified by being the process the KERNEL spawned
// (ppid 0) that is called init -- not by its number, for the reason in
// this file's header.
static int init_pid(void) {
    struct proc_info info;
    for (int i = 0; sys_proc_info(i, &info) == 0; i++) {
        if (info.pid > 0 && info.ppid == 0 && strcmp(info.name, "init") == 0)
            return info.pid;
    }
    return 0;
}

// The status, asking init for it first if it has not published any.
//
// **init WRITES NOTHING UNTIL SOMEBODY ASKS**, because there is no
// tmpfs here and a status file nobody reads is a disk write nobody
// wanted -- so the first reader has to say so. A bare doorbell is that
// request; every later read finds the file already current, since init
// keeps it up to date once it knows there is a reader.
static const char *read_status(void) {
    const char *st = read_status_file();
    if (st) return st;

    int pid = init_pid();
    if (pid <= 0) return 0;
    if (sys_kill(pid, SIGHUP) < 0) return 0;

    // Bounded, and it has to be: init publishes on its next SETTLED
    // pass, so a machine with a service in a restart backoff answers
    // late rather than never.
    for (int waited = 0; waited < SETTLE_MS; waited += SETTLE_STEP) {
        sys_sleep_ms(SETTLE_STEP);
        st = read_status_file();
        if (st) return st;
    }
    return 0;
}

// THE CHANNEL, tried first. init publishes a beacon (lib/uinitctl.h)
// and answers on it, so this is one round trip with a RESULT rather
// than a file and a signal followed by a poll of the status file.
//
// Returns 1 if the channel carried it, 0 if there is no channel to
// carry it on -- which is not a failure: the caller falls through to
// the file and the doorbell, and that path stays exercised on any boot
// where init could not open its channel.
static int send_over_channel(const char *verb, const char *name, int *result) {
    struct uchan_client c;
    if (uchan_client_open(&c, INITCTL_SERVICE) < 0) return 0;

    struct initctl_msg m, reply;
    memset(&m, 0, sizeof m);
    m.verb = strcmp(verb, "start") == 0 ? INITCTL_START : INITCTL_STOP;
    snprintf(m.name, sizeof m.name, "%s", name);

    int ok = uchan_call(&c, &m, sizeof m, &reply, sizeof reply, CHAN_REPLY_MS) == 0;
    uchan_client_close(&c);
    if (!ok) return 0;          // init has the beacon up but did not answer
    *result = (int)reply.result;
    return 1;
}

// Appends one request and rings the doorbell. APPEND rather than
// truncate: two requests made in the same breath are both meant, and a
// second `service` overwriting the first would silently drop it.
static int send_request(const char *verb, const char *name) {
    int pid = init_pid();
    if (pid <= 0) {
        sys_print("service: init is not running -- nothing supervises anything\n");
        return 0;
    }

    int fd = open(CONTROL_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        sys_print("service: could not write " CONTROL_PATH "\n");
        return 0;
    }
    char line[128];
    int n = snprintf(line, sizeof line, "%s %s\n", verb, name);
    if (n <= 0 || write(fd, line, (size_t)n) != n) {
        close(fd);
        sys_print("service: could not write the request\n");
        return 0;
    }
    close(fd);

    // THE SIGNAL IS THE WAKE. Without it the request sits unread until
    // one of init's children happens to exit, which on a machine with a
    // desktop up can be never.
    if (sys_kill(pid, SIGHUP) < 0) {
        sys_print("service: could not signal init\n");
        return 0;
    }
    return 1;
}

// Waits until `name`'s state stops being `from`, then reports the line.
// Reports either way at the deadline: "it did not change" is the answer
// to a request that could not be honoured, and it is more useful than
// this program guessing why.
static void settle_and_report(const char *name, const char *from) {
    char now[32];
    for (int waited = 0; waited < SETTLE_MS; waited += SETTLE_STEP) {
        sys_sleep_ms(SETTLE_STEP);
        state_of(name, now, sizeof now);
        if (now[0] && strcmp(now, from) != 0) break;
    }

    const char *status = read_status();
    const char *line = status ? status_line(status, name) : 0;
    if (line) print_line(line);
    else sys_print("service: init published no status for it\n");
}

// WHY THE STATUS CAN BE ABSENT WITH INIT PERFECTLY HEALTHY, and why
// saying so matters. init publishes on demand and only when the machine
// has SETTLED (see init.c's write_status): nothing is written until
// somebody rings the doorbell, and nothing is written while a service is
// still starting or in a restart backoff. read_status() rings and waits,
// so reaching here means the wait expired -- which on a freshly booted
// machine is "not yet", not "init is broken".
//
// The old message asked "is init running?" for both cases. On a laptop
// booted from a fresh install that sent the maintainer looking for a
// dead init while init was pid 1 and the desktop was running.
static void explain_no_status(void) {
    if (init_pid() <= 0) {
        sys_print("service: init is not running -- nothing supervises "
                  "services on this machine\n");
        return;
    }
    sys_print("service: init is running but has not published a status "
              "yet.\nIt publishes when the machine has settled, so a "
              "service still starting\nor in a restart backoff holds it "
              "back -- try again in a moment.\n");
}

static int cmd_list(void) {
    const char *status = read_status();
    if (!status) {
        explain_no_status();
        return 1;
    }
    sys_print(status);
    return 0;
}

// The one thing the status file cannot answer: a descriptor's
// Description= is for a human and init has no reason to carry it in
// memory, so it is read here, from the file that declares it.
static struct etc_config_buf g_cfg;

static void print_description(const char *name) {
    char path[96];
    snprintf(path, sizeof path, SERVICES_DIR "/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;   // a service whose descriptor is gone, or whose
                          // Name= differs from its filename
    int64_t n = read(fd, g_cfg.data, sizeof g_cfg.data - 1);
    close(fd);
    if (n <= 0) return;
    g_cfg.data[n] = '\0';
    g_cfg.size = (uint32_t)n;
    g_cfg.valid = 1;

    char desc[128];
    if (etc_config_buf_get(&g_cfg, "Description", desc, sizeof desc)) {
        char line[160];
        snprintf(line, sizeof line, "  %s\n", desc);
        sys_print(line);
    }
}

static int cmd_status(const char *name) {
    const char *status = read_status();
    if (!status) {
        explain_no_status();
        return 1;
    }
    const char *line = status_line(status, name);
    if (!line) {
        char msg[128];
        snprintf(msg, sizeof msg, "service: init supervises no service called %s\n", name);
        sys_print(msg);
        return 1;
    }
    print_line(line);
    print_description(name);
    return 0;
}

// --- enable and disable: the descriptor, not the process ---------------
//
// ENABLE IS A COPY AND DISABLE IS A DELETE, because that is already what
// init means by them. init scans /etc/services.d and restarts what it
// finds there; /usr/share/services is a directory it never reads. So a
// descriptor's PRESENCE is the enabled state, and there is no third
// place for an `Enabled=` key to disagree with.
//
// This is systemd's split -- enable/disable act on the unit file,
// start/stop act on the process -- and the pairs are deliberately not
// interchangeable: `service stop` is undone by a reboot, `service
// disable` is not.
#define AVAIL_DIR "/usr/share/services/"
#define ENABLED_DIR "/etc/services.d/"

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return 0;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { close(in); return 0; }
    static char buf[512];
    int ok = 1;
    for (;;) {
        int64_t n = read(in, buf, sizeof buf);
        if (n < 0) { ok = 0; break; }
        if (n == 0) break;
        if (write(out, buf, (size_t)n) != n) { ok = 0; break; }
    }
    close(in);
    close(out);
    return ok;
}

static void ring_init(void) {
    int pid = init_pid();
    if (pid > 0) sys_kill(pid, SIGHUP);
}

static int cmd_enable(const char *name) {
    char from[96], to[96], tmp[96], msg[192];
    snprintf(from, sizeof from, "%s%s", AVAIL_DIR, name);
    snprintf(to, sizeof to, "%s%s", ENABLED_DIR, name);
    // WRITTEN ELSEWHERE AND MOVED IN, because init rescans
    // /etc/services.d on ANY filesystem change and a descriptor built up
    // by a write loop can be read half-finished. A partial one is not an
    // error it would report -- it is a service with no Exec=, or with an
    // ordering key it correctly ignores, so it starts wrongly and
    // nothing looks broken. data/etc/services.d/README.md says to do
    // exactly this; writing in place got the half-read on the first try.
    // /run, NOT /tmp, and this one is load-bearing rather than tidy:
    // the rename below crosses from here into /etc/services.d, and
    // kernel/mount.h's rule 4 REFUSES a rename that crosses a mount.
    // /tmp is a ramfs mount now, so leaving this here would have made
    // `service enable` fail with EXDEV every time.
    snprintf(tmp, sizeof tmp, TMP_RUNDIR "/.svc-%s", name);
    if (!copy_file(from, tmp) || rename(tmp, to) < 0) {
        unlink(tmp);
        snprintf(msg, sizeof msg,
                 "service: no available service called %s (look in %s)\n",
                 name, AVAIL_DIR);
        sys_print(msg);
        return 1;
    }
    // init rescans on its next WAKE, which on an idle machine is up to
    // 250 ms away and with a service running is whenever a child exits.
    // The doorbell is what makes `enable` take effect now rather than
    // eventually -- the same HUP `service reload` sends.
    ring_init();
    snprintf(msg, sizeof msg, "service: enabled %s\n", name);
    sys_print(msg);
    return 0;
}

static int cmd_disable(const char *name) {
    char to[96], msg[192];
    snprintf(to, sizeof to, "%s%s", ENABLED_DIR, name);
    if (unlink(to) < 0) {
        snprintf(msg, sizeof msg, "service: %s is not enabled\n", name);
        sys_print(msg);
        return 1;
    }
    ring_init();
    // DISABLE DOES NOT STOP IT, which is init's own rule: removing a
    // descriptor makes init stop RESTARTING the service and leaves the
    // running copy alone. Said here because the alternative reading is
    // the obvious one.
    snprintf(msg, sizeof msg,
             "service: disabled %s -- a running copy keeps running until "
             "it is stopped\n", name);
    sys_print(msg);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) return cmd_list();

    if (strcmp(argv[1], "reload") == 0) {
        int pid = init_pid();
        if (pid <= 0) {
            sys_print("service: init is not running\n");
            return 1;
        }
        // No request file: a bare doorbell makes init rescan
        // /etc/services.d, which is what a HUP means to every other init
        // there has ever been.
        if (sys_kill(pid, SIGHUP) < 0) {
            sys_print("service: could not signal init\n");
            return 1;
        }
        sys_print("service: init asked to reload\n");
        return 0;
    }

    if (argc < 3) {
        cmd_usage("service [list] | status <name> | start <name> | stop <name> | "
                  "enable <name> | disable <name> | reload");
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) return cmd_status(argv[2]);
    if (strcmp(argv[1], "enable") == 0)  return cmd_enable(argv[2]);
    if (strcmp(argv[1], "disable") == 0) return cmd_disable(argv[2]);

    int start = strcmp(argv[1], "start") == 0;
    if (!start && strcmp(argv[1], "stop") != 0) {
        cmd_usage("service [list] | status <name> | start <name> | stop <name> | "
                  "enable <name> | disable <name> | reload");
        return 1;
    }

    // READ THE STATE FIRST, because the wait below is for it to CHANGE.
    // A service already in the state being asked for has nothing to
    // wait for, and saying so beats three seconds of silence.
    char before[32];
    state_of(argv[2], before, sizeof before);
    if (!before[0]) {
        char msg[128];
        snprintf(msg, sizeof msg, "service: init supervises no service called %s\n", argv[2]);
        sys_print(msg);
        return 1;
    }
    if (start && strcmp(before, "running") == 0) {
        sys_print("service: already running\n");
        return cmd_status(argv[2]);
    }
    if (!start && strcmp(before, "running") != 0) {
        sys_print("service: not running\n");
        return cmd_status(argv[2]);
    }

    // THE CHANNEL FIRST, the file and the doorbell if there is none.
    // init's answer says whether the request was ACCEPTED; what the
    // service then does is still watched through the status file, since
    // "stop" means a SIGTERM the service obeys when it chooses to.
    const char *verb = start ? "start" : "stop";
    int result = INITCTL_OK;
    if (send_over_channel(verb, argv[2], &result)) {
        if (result == INITCTL_NO_SUCH) {
            sys_print("service: no service by that name\n");
            return 1;
        }
        if (result != INITCTL_OK) {
            sys_print("service: init refused the request\n");
            return 1;
        }
    } else if (!send_request(verb, argv[2])) {
        return 1;
    }
    settle_and_report(argv[2], before);
    return 0;
}
