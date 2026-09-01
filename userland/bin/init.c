// init -- pid 1, the process every other ring-3 process ends up under.
//
// Stage 2 of docs/init-design.md. It does two things, and the second is
// what makes it an init rather than a reaper:
//
//   REAPS. A zombie is only ever cleared by somebody waiting for it, so
//   before init existed an orphan -- a process whose parent died first
//   -- held its slot for the rest of the boot with nothing in the
//   system able to free it. The kernel does the adopting
//   (scheduler.c's reparent_children()); this side is the loop that
//   then collects them.
//
//   SUPERVISES. It reads the boot TARGET (api/target.h) and starts
//   every service in /etc/services.d whose `Target=` matches, then
//   restarts one that dies. That is what makes the desktop init's
//   child rather than the shell's: its clients are init's
//   grandchildren, and killing the desktop reparents them here instead
//   of stranding them.
//
// ...and it starts them in a DECLARED ORDER. `After=` and `Before=`
// name other services; init topologically sorts them and spawns in that
// order. That is systemd's shape, and the reason to copy it rather than
// SysV's `S20foo` priority number is this repo's own rule against facts
// somebody else has to keep true -- a number needs renumbering the day a
// service is inserted between two others, while a name does not.
//
// ...and `After=` CAN NOW MEAN "usable", not merely "spawned". A
// service whose descriptor says `Ready=notify` is not considered
// started when sys_spawn() returns a pid -- it is started when it calls
// SYS_NOTIFY_READY, which init sees as a bit on its row in
// SYS_PROC_INFO. Anything ordered after it waits for that. That is
// systemd's Type=notify; the default stays `Ready=spawn`, which is
// Type=simple and the honest description of what a fire-and-forget
// spawn can promise.
//
// THE BARRIER ALWAYS EXPIRES, and that matters more than the barrier.
// `ReadyTimeout=` bounds the wait; when it runs out init logs a line
// naming the service and starts the dependents ANYWAY. Same reasoning
// as the crash-loop give-up below and as the ordering cycle: a machine
// that starts nothing has no console left to fix itself from, so no
// key in a descriptor may be able to reach that state. systemd fails
// the dependents instead, which it can afford because it has a rescue
// target and a journal to read afterwards.
//
// WHY A DIRECTORY OF FILES rather than a compiled-in list: it is the
// same call the Start menu already made when it stopped being a C table
// and became /usr/wm/desktop. Adding a service is dropping a file, and
// the format is the parser every other config file here uses. See
// data/etc/services.d/README.md.
//
// THE GIVE-UP MATTERS MORE THAN THE RESTART. `Restart=always` with no
// limit turns a binary that faults at its entry point into a machine
// that spins forever starting it, which is worse than a desktop that is
// simply down -- there is no console left to fix it from. So a service
// that keeps dying QUICKLY is declared a crash loop and left down with
// a line saying so, which is systemd's StartLimitBurst. The backoff
// between attempts is the other half: it doubles, so a service that is
// merely slow to become healthy is not hammered.
//
// THREE STATES, and only the last one polls:
//
//   - Something running, nothing due to restart: blocks in waitpid(-1)
//     and consumes NOTHING until a child dies.
//   - No children at all (a `text` boot with no services): sleeps.
//     waitpid(-1) answers -1, which is PERMANENT -- see its ABI
//     comment -- so there is nothing to block on. A yield loop here
//     would burn a core forever and make every CPU figure meaningless.
//   - A restart pending, or a readiness barrier outstanding: polls with
//     waitpid_nohang, since a blocking wait has no deadline and neither
//     a backoff expiring nor a service calling SYS_NOTIFY_READY wakes a
//     waiter. Both are bounded -- a backoff by its table, a barrier by
//     ReadyTimeout= -- so this state ends on its own.
#include "rt/sys.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "etc_config.h"
#include "setting_abi.h"
#include "syscall_abi.h" // struct sys_dirent
#include <fcntl.h>
#include <unistd.h>

#define SERVICES_DIR   "/etc/services.d"
#define TARGET_SETTING "system.default_target"

// THE CONTROL CHANNEL: a request file plus a doorbell.
//
// /bin/service appends `<verb> <name>` to CONTROL_PATH and sends SIGHUP;
// init reads every line, acts, and deletes the file. That is runit's
// `supervise/control` object plus SysV's `kill -HUP 1`, and it is the
// shape that ports: there are no unix sockets here for systemd's D-Bus
// and no named pipes for /run/initctl.
//
// THE SIGNAL IS THE WAKE, NOT THE MESSAGE, and it has to be: with a
// service running init BLOCKS in waitpid(-1), so a file written while
// the desktop is up is not noticed until something dies. The handler
// carries no payload (a signal cannot), which is why the verb is in a
// file and the file is read by the loop rather than by the handler.
//
// STATUS_PATH is the answer coming back -- one line per service,
// rewritten whenever anything changes, because init is the only thing
// that knows a service is down ON PURPOSE rather than merely absent
// from the process table. runit writes the same file per service; this
// one is text and greppable, so `cat` is a working `service list`.
//
// BOTH LIVE IN /tmp because they are runtime state, which is what /run
// is for on a real system and /tmp is the only such directory here.
// /tmp is NOT emptied at boot (docs/filesystem-layout.md), so a request
// left by a machine that lost power would otherwise be obeyed by the
// next boot: init deletes the control file at startup for that reason.
#define CONTROL_PATH   "/tmp/init.ctl"
#define STATUS_PATH    "/tmp/init.status"

// Sixteen, not eight: ordering only means anything with several
// services, and the table is static rather than on the stack, so the
// cap costs address space instead of the ring-3 guard page. Overflow
// is refused with a line naming the descriptor, never silently.
#define SVC_MAX        16
#define SVC_NAME_MAX   24
#define SVC_EXEC_MAX   64
#define SVC_ARGS_MAX   64
// After= and Before= are stored as the raw space-separated text and
// resolved to indices only once the WHOLE directory has been scanned --
// a descriptor may name a service whose file has not been read yet, so
// resolving as we parse would depend on listdir order, which is exactly
// what this feature exists to stop depending on.
#define SVC_DEPS_MAX   64

// A service that dies within this of starting counts as a FAST failure,
// which is what a crash loop is made of. Anything longer ran, did
// something, and stopped -- that is a restart, not a loop, so the
// attempt counter goes back to zero.
#define SVC_FAST_MS    2000
#define SVC_MAX_FAST   5    // consecutive fast failures before giving up

// Backoff between attempts, indexed by consecutive fast failures. The
// first restart is immediate on purpose: a desktop the user just killed
// should come straight back, and only a service that is actually
// failing pays the wait.
static const int SVC_BACKOFF_MS[] = { 0, 250, 500, 1000, 2000 };

// Long enough that an idle machine wakes ~4 times a second, short
// enough that a freshly adopted orphan is reaped promptly even if the
// adoption's wake is missed. The kernel wakes child-waiters on adoption
// (reparent_children()), so this is a backstop rather than the
// mechanism -- which is the right way round: a poll that is load-
// bearing is a poll whose interval is a correctness constant.
#define IDLE_SLEEP_MS  250

// How finely a pending backoff is checked. Only ever used while a
// restart is actually due, never on an idle machine.
#define POLL_SLEEP_MS  50

// Restart= -- what a service's EXIT means.
//
// The default is ON_FAILURE, and the reason is a real feature: the Start
// menu's "Exit to shell" makes the desktop return 0, i.e. it ASKED to
// stop. Under `always` init restarted it immediately and the menu item
// silently did nothing. A process that chose to leave stays gone; one
// that crashed or was killed comes back. That is systemd's
// `on-failure`, and it is the sensible default here for the same reason
// it is there.
#define SVC_RESTART_NO         0
#define SVC_RESTART_ON_FAILURE 1
#define SVC_RESTART_ALWAYS     2

// Ready= -- what "started" MEANS for this service.
//
// SPAWN is the default and is what every service did before this
// existed: started is sys_spawn() returning a pid. NOTIFY is systemd's
// Type=notify -- started is the process calling SYS_NOTIFY_READY, and
// anything ordered after it waits.
//
// The default has to be SPAWN, and not only for compatibility: a
// service that never calls the syscall would otherwise hold up
// everything after it for a whole timeout on every boot, so opting IN
// is the only default under which a descriptor that says nothing about
// readiness behaves sensibly.
#define SVC_READY_SPAWN  0
#define SVC_READY_NOTIFY 1

// How long init waits for a `Ready=notify` service to announce itself
// before starting its dependents anyway. Five seconds, against a
// desktop that reaches its first composited frame in well under one on
// every machine this has been run on -- long enough to absorb a slow
// boot, short enough that a service which will never report does not
// look like a hang.
//
// systemd's equivalent (TimeoutStartSec) is 90s and KILLS the unit when
// it expires. Both differ here deliberately: 90s of a black screen is
// not a diagnosis anyone waits for on a machine with one console, and
// killing the service would take a desktop that is merely slow and
// remove it, which is the opposite of what the timeout is protecting.
#define SVC_READY_TIMEOUT_MS 5000

struct service {
    char name[SVC_NAME_MAX];
    char exec[SVC_EXEC_MAX];
    // Whitespace-separated arguments, as SYS_SPAWN has always taken --
    // the descriptor format simply had no way to say them, so a service
    // that needed one had to be a program with the arguments baked in.
    // `inetd -p 23 /bin/telnetd` is the first that genuinely does.
    char args[SVC_ARGS_MAX];
    int  restart;      // SVC_RESTART_*
    int  pid;          // 0 when not running
    unsigned long long started_ms;
    unsigned long long due_ms;  // when it may next be started
    int  fast_failures;
    int  started_once;
    // What the last run RETURNED, which is the only thing that tells a
    // one-shot that did its job from one that could not: both are down
    // for good, and `Restart=no` never records a failure anywhere else.
    int  last_exit;
    char after[SVC_DEPS_MAX];   // names that must be spawned before this
    char before[SVC_DEPS_MAX];  // names this must be spawned before
    int  seen;         // survived the last scan
    int  stopped;      // exited cleanly and asked to stay down
    int  disabled;     // its descriptor is gone -- do not start it again
    // An operator asked for this service to be DOWN (`service stop`).
    // Separate from `stopped` because the two are cleared by different
    // things and one of them must survive a non-zero exit: a service
    // stopped by hand is SIGTERMed, so it dies with 128 + SIGTERM, and
    // reusing `stopped` would leave the restart policy looking at a
    // failure and putting it straight back.
    int  admin_stopped;
    int  gave_up;

    int  ready_mode;   // SVC_READY_*
    // Set when this service called SYS_NOTIFY_READY. Cleared at every
    // start: a restarted service is a new process that has announced
    // nothing yet.
    int  ready;
    // Set when the wait for that announcement ran out. STICKY ACROSS
    // RESTARTS, unlike `ready`, so a service that keeps failing to
    // report cannot re-arm the barrier once per restart and hold its
    // dependents down for the whole crash-loop budget.
    int  ready_timed_out;
    unsigned long long ready_timeout_ms;
    unsigned long long ready_deadline_ms;
};

// Static, not local: USERLAND_CFLAGS carries -Wframe-larger-than=2048
// and this table plus a 1 KiB config buffer is several times that. A
// big local array in ring 3 does not merely overflow the 16 KiB stack,
// it steps over the single guard page into unmapped memory.
static struct service g_svc[SVC_MAX];
static int g_svc_count;
static char g_target[SETTING_ABI_VALUE_MAX];
static struct etc_config_buf g_cfg;
static struct sys_dirent g_ents[SVC_MAX * 2];
static char g_msg[128];
static unsigned long long g_fs_gen;

static unsigned long long now_ms(void) {
    return sys_monotonic_ns() / 1000000ull;
}

static void logf1(const char *fmt, const char *a) {
    snprintf(g_msg, sizeof g_msg, fmt, a);
    sys_eprint(g_msg);
}

// --- the boot target -------------------------------------------------

// Asks the kernel's settings registry rather than reading
// /etc/toyos.conf directly, so a `target=text` word on the GRUB line --
// which the kernel applies as a LIVE value without writing the file --
// is what init sees. Reading the file here would silently ignore the
// override, which is precisely the escape hatch a desktop that faults
// on boot depends on.
static void load_target(void) {
    struct setting_msg msg;
    k_memset(&msg, 0, sizeof msg);
    msg.op = SETTING_OP_GET;
    k_strlcpy(msg.name, TARGET_SETTING, sizeof msg.name);

    if (sys_setting(&msg) == 0 && msg.value[0]) {
        k_strlcpy(g_target, msg.value, sizeof g_target);
    } else {
        // No registry answer is not a reason to start nothing: a kernel
        // that cannot say what it is for still has a desktop, and a
        // machine that boots to a blank screen because a syscall failed
        // is a worse outcome than one that boots to the wrong target.
        k_strlcpy(g_target, "graphical", sizeof g_target);
        sys_eprint("init: could not read " TARGET_SETTING
                   ", assuming graphical\n");
    }
    logf1("init: target %s\n", g_target);
}

// --- service descriptors ---------------------------------------------

// A DESCRIPTOR LONGER THAN THE PARSER'S BUFFER LOSES ITS LAST KEYS, and
// silently, which is how `Ready=notify` on the end of a well-commented
// file can simply not exist. The read is capped at ETC_CONFIG_BUF_MAX
// (api/etc_config.h) and nothing above it could tell a file that fit
// from one that was cut in half.
//
// Detected by asking for ONE more byte after a full read: a file that
// has one is over the cap. Reported and then USED ANYWAY rather than
// refused, which is the same call this file makes for an unknown
// Restart= and for an ordering cycle -- no key, and no comment, may be
// able to leave the machine with nothing started. The line is what
// makes it findable; the alternative was a service quietly running
// under half its own configuration.
static int read_file(const char *path, struct etc_config_buf *buf) {
    buf->valid = 0;
    buf->size = 0;
    buf->data[0] = '\0';

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    int64_t n = read(fd, buf->data, sizeof buf->data - 1);
    if (n == (int64_t)(sizeof buf->data - 1)) {
        char extra;
        if (read(fd, &extra, 1) > 0)
            logf1("init: %s is longer than the config parser's buffer -- "
                  "its last keys are being ignored\n", path);
    }
    close(fd);
    if (n <= 0) return 0;

    buf->data[n] = '\0';
    buf->size = (uint32_t)n;
    buf->valid = 1;
    return 1;
}

static void load_service(const char *file) {
    char path[96];
    snprintf(path, sizeof path, SERVICES_DIR "/%s", file);

    if (!read_file(path, &g_cfg)) {
        logf1("init: could not read %s\n", path);
        return;
    }

    char want[SVC_NAME_MAX];
    // No Target= means EVERY target. That is the useful default: a
    // service with no opinion should run on a text boot as well as a
    // graphical one, and requiring the key would make the common case
    // the verbose one.
    if (etc_config_buf_get(&g_cfg, "Target", want, sizeof want)
        && k_strcmp(want, g_target) != 0)
        return;

    char name[SVC_NAME_MAX];
    if (!etc_config_buf_get(&g_cfg, "Name", name, sizeof name))
        k_strlcpy(name, file, sizeof name);

    // A RESCAN UPDATES IN PLACE. Matching on Name means a service keeps
    // its pid, its failure count and its backoff across a reload -- a
    // fresh entry would look "not running" and be started a second time,
    // which is how a live-reload feature turns into a fork bomb.
    struct service *s = 0;
    for (int i = 0; i < g_svc_count; i++)
        if (k_strcmp(g_svc[i].name, name) == 0) { s = &g_svc[i]; break; }

    if (!s) {
        if (g_svc_count >= SVC_MAX) {
            logf1("init: too many services, ignoring %s\n", file);
            return;
        }
        s = &g_svc[g_svc_count++];
        k_memset(s, 0, sizeof *s);
        k_strlcpy(s->name, name, sizeof s->name);
    }
    s->seen = 1;
    s->disabled = 0;

    // Exec is the one key with no sensible default: a service with no
    // command is not a service. Refused loudly -- a descriptor that
    // silently does nothing is indistinguishable from one that is not
    // being read at all.
    //
    // AN EMPTY exec IS THE "cannot be started" STATE, and returning
    // without setting it is what made the refusal a lie: the entry is
    // already in g_svc[] by this point, so start_due() spawned the
    // empty path five times over while the log claimed it was ignored.
    // It is deliberately NOT the `disabled` flag, which is sticky
    // across rescans -- a descriptor that gains an Exec= later is
    // re-read here and starts, with no separate un-disable step.
    if (!etc_config_buf_get(&g_cfg, "Exec", s->exec, sizeof s->exec)) {
        s->exec[0] = '\0';
        logf1("init: %s has no Exec=, will not start it\n", file);
        return;
    }

    // Absent is an empty argument string, not a missing one: a service
    // with no arguments is the common case and says nothing.
    if (!etc_config_buf_get(&g_cfg, "Args", s->args, sizeof s->args))
        s->args[0] = '\0';

    // Absent means unconstrained, which is the common case and must
    // stay the terse one. Cleared rather than left alone, so removing
    // the key from a descriptor and letting init rescan actually drops
    // the constraint.
    if (!etc_config_buf_get(&g_cfg, "After", s->after, sizeof s->after))
        s->after[0] = '\0';
    if (!etc_config_buf_get(&g_cfg, "Before", s->before, sizeof s->before))
        s->before[0] = '\0';

    char restart[16];
    s->restart = SVC_RESTART_ON_FAILURE;
    if (etc_config_buf_get(&g_cfg, "Restart", restart, sizeof restart)) {
        if (k_strcmp(restart, "no") == 0)          s->restart = SVC_RESTART_NO;
        else if (k_strcmp(restart, "always") == 0) s->restart = SVC_RESTART_ALWAYS;
        else if (k_strcmp(restart, "on-failure") != 0)
            // An unrecognised value keeps the default rather than
            // failing the boot, like every other /etc reader here -- but
            // it SAYS so, because a typo that silently changes a restart
            // policy is found the day the service dies and stays dead.
            logf1("init: %s has an unknown Restart=, using on-failure\n",
                  s->name);
    }

    char ready[16];
    s->ready_mode = SVC_READY_SPAWN;
    if (etc_config_buf_get(&g_cfg, "Ready", ready, sizeof ready)) {
        if (k_strcmp(ready, "notify") == 0) s->ready_mode = SVC_READY_NOTIFY;
        else if (k_strcmp(ready, "spawn") != 0)
            // Same call as Restart= above: keep the default rather than
            // fail the boot, but SAY so. A typo here would otherwise
            // read as "readiness silently does not work", which is the
            // hardest kind of not-working to find.
            logf1("init: %s has an unknown Ready=, using spawn\n", s->name);
    }

    char timeout[16];
    s->ready_timeout_ms = SVC_READY_TIMEOUT_MS;
    if (etc_config_buf_get(&g_cfg, "ReadyTimeout", timeout, sizeof timeout)) {
        int ms = atoi(timeout);
        // Zero or negative is not a shorter wait, it is a request for a
        // barrier that expires before the service can possibly answer --
        // so it is refused rather than honoured. There is no way to
        // spell "wait forever" on purpose, and that is the point: see
        // this file's header on why no key may be able to leave the
        // machine with nothing started.
        if (ms > 0) s->ready_timeout_ms = (unsigned long long)ms;
        else logf1("init: %s has a non-positive ReadyTimeout=, using the default\n",
                   s->name);
    }
}

// --- start order -----------------------------------------------------
//
// A topological sort over the After=/Before= edges, recomputed after
// every scan and read by start_due(). Three properties, each a choice
// rather than a consequence:
//
//   IT IS STABLE. Ties are broken by a service's index in g_svc[], i.e.
//   the order its descriptor was read, so services with no ordering
//   constraints between them keep exactly the behaviour they had before
//   this existed. An unstable sort would let an unrelated edit reshuffle
//   unrelated services, which is the failure mode that makes an
//   ordering feature untrustworthy.
//
//   BOTH KEYS PRODUCE THE SAME EDGE. `After=B` on A and `Before=A` on B
//   are the same constraint written from either end, exactly as in
//   systemd -- which matters because the two ends are usually owned by
//   different people: a service can order itself against one it must not
//   have to edit.
//
//   A MALFORMED GRAPH NEVER FAILS A BOOT. A cycle logs a line and has
//   one edge dropped; a name that matches no loaded service logs a line
//   and is ignored. systemd does the former; the reason to do it here is
//   harsher than tidiness -- a machine that starts nothing has no
//   console left to fix itself from, so a typo in an ordering key must
//   never be able to reach that state.

static int g_order[SVC_MAX];
static unsigned char g_edge[SVC_MAX][SVC_MAX]; // g_edge[a][b]: a spawns before b

// The loaded service called `tok` (a token of `len` bytes, not
// NUL-terminated), or -1.
static int find_service_n(const char *tok, int len) {
    for (int i = 0; i < g_svc_count; i++) {
        if (k_strncmp(g_svc[i].name, tok, (size_t)len) == 0
            && g_svc[i].name[len] == '\0')
            return i;
    }
    return -1;
}

// Walks a space-separated name list and records one edge per name.
// `self_first` is what the key means: Before= puts self first, After=
// puts the named service first.
static void add_edges(int self, const char *list, int self_first,
                      const char *key) {
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
        int len = (int)(p - start);
        if (len <= 0) continue;

        int other = find_service_n(start, len);
        if (other < 0) {
            // Not necessarily a typo: a service on the OTHER boot target
            // was never loaded, and naming it is a perfectly reasonable
            // thing for a descriptor to do. init cannot tell the two
            // apart, so it says what it saw rather than guessing which.
            char nm[SVC_NAME_MAX];
            int n = len < (int)sizeof nm - 1 ? len : (int)sizeof nm - 1;
            k_memcpy(nm, start, (size_t)n);
            nm[n] = '\0';
            snprintf(g_msg, sizeof g_msg,
                     "init: %s: %s=%s names no loaded service, ignoring it\n",
                     g_svc[self].name, key, nm);
            sys_eprint(g_msg);
            continue;
        }
        if (other == self) continue; // ordering against yourself is a no-op

        if (self_first) g_edge[self][other] = 1;
        else            g_edge[other][self] = 1;
    }
}

// Recomputes g_order[]. Called once per scan, never per pass -- the
// graph can only change when a descriptor does.
static void build_order(void) {
    k_memset(g_edge, 0, sizeof g_edge);
    for (int i = 0; i < g_svc_count; i++) {
        if (g_svc[i].after[0])  add_edges(i, g_svc[i].after, 0, "After");
        if (g_svc[i].before[0]) add_edges(i, g_svc[i].before, 1, "Before");
    }

    // Kahn's algorithm, scanning from the front each round so the first
    // eligible service always wins -- that is what makes it stable.
    unsigned char done[SVC_MAX];
    k_memset(done, 0, sizeof done);

    for (int out = 0; out < g_svc_count; out++) {
        int pick = -1;
        for (int i = 0; i < g_svc_count && pick < 0; i++) {
            if (done[i]) continue;
            int blocked = 0;
            for (int j = 0; j < g_svc_count; j++)
                if (!done[j] && g_edge[j][i]) { blocked = 1; break; }
            if (!blocked) pick = i;
        }

        if (pick < 0) {
            // Everything left is inside a cycle. Take the lowest index,
            // which drops its incoming edges -- systemd's "breaking
            // ordering cycle by deleting job". Named loudly: an ordering
            // that is silently not honoured is worse than none, because
            // the descriptor still says it was asked for.
            for (int i = 0; i < g_svc_count; i++)
                if (!done[i]) { pick = i; break; }
            logf1("init: ordering cycle -- starting %s anyway\n",
                  g_svc[pick].name);
            // AND THE EDGES ARE REALLY DROPPED, not merely stepped over.
            // The sort alone could leave them in g_edge[][] because it
            // only ever reads them once; the readiness barrier reads
            // them EVERY pass, so a surviving cycle edge would be a
            // wait for something that is itself waiting -- the exact
            // "machine with nothing started" this init refuses to be
            // able to reach.
            for (int j = 0; j < g_svc_count; j++) g_edge[j][pick] = 0;
        }

        done[pick] = 1;
        g_order[out] = pick;
    }
}

// Scans /etc/services.d and reconciles it against what is running.
// `announce` keeps the boot message off every subsequent rescan.
static void load_services(int announce) {
    for (int i = 0; i < g_svc_count; i++) g_svc[i].seen = 0;

    int n = sys_listdir(SERVICES_DIR, g_ents,
                        (int)(sizeof g_ents / sizeof g_ents[0]));
    if (n <= 0) {
        // Not an error. A machine with no service files is a machine
        // that starts nothing, which is what a bare disk image gives
        // you -- and is recoverable, since the kernel's own shell only
        // stands down for a console service that exists.
        if (announce) sys_eprint("init: no services in " SERVICES_DIR "\n");
    }

    for (int i = 0; i < n; i++) {
        if (g_ents[i].is_dir) continue;
        load_service(g_ents[i].name);
    }

    // A DESCRIPTOR THAT HAS GONE AWAY DISABLES ITS SERVICE, and does not
    // stop it. That is systemd's `disable`, not `stop`, and the split is
    // deliberate: killing a running process because someone edited a
    // file in /etc is a surprise, while quietly refusing to restart it
    // is what "I no longer want this service" actually means. It is also
    // the only way to take a supervised service out of init's hands
    // without a reboot -- `delete /etc/services.d/toywm` then `kill` the
    // pid, which is exactly what a test needing the compositor role has
    // to do.
    for (int i = 0; i < g_svc_count; i++) {
        struct service *sv = &g_svc[i];
        if (sv->seen || sv->disabled) continue;
        sv->disabled = 1;
        logf1("init: %s -- descriptor gone, will not restart it\n", sv->name);
    }

    build_order();
}

// Has anything on the filesystem changed since the last look? One
// integer compare, no I/O -- the same trick the desktop uses to notice a
// new `.desktop` file, and the reason SYS_FS_GENERATION exists. It says
// SOMETHING changed, never what, so a move means rescan.
static int services_changed(void) {
    unsigned long long gen = sys_fs_generation();
    if (gen == g_fs_gen) return 0;
    g_fs_gen = gen;
    return 1;
}

// --- supervision -----------------------------------------------------

// The backoff for the Nth consecutive fast failure. Clamped rather than
// wrapped: past the end of the table the wait simply stops growing,
// which is only ever reached on the attempt before giving up anyway.
static unsigned long long backoff_ms(int fast_failures) {
    int max = (int)(sizeof SVC_BACKOFF_MS / sizeof SVC_BACKOFF_MS[0]) - 1;
    int idx = fast_failures < 0 ? 0 : fast_failures;
    if (idx > max) idx = max;
    return (unsigned long long)SVC_BACKOFF_MS[idx];
}

// Records a failure and either schedules the next attempt or gives up.
static void service_failed(struct service *s) {
    if (s->fast_failures >= SVC_MAX_FAST) {
        s->gave_up = 1;
        logf1("init: %s is crash-looping, giving up\n", s->name);
        return;
    }
    s->due_ms = now_ms() + backoff_ms(s->fast_failures);
}

static void start_service(struct service *s) {
    s->started_once = 1;
    s->last_exit = 0;

    int pid = sys_spawn(s->exec, s->args[0] ? s->args : 0, -1);
    if (pid > 0) {
        s->pid = pid;
        s->started_ms = now_ms();
        // A NEW PROCESS HAS ANNOUNCED NOTHING. Cleared on every start,
        // not only the first, so a restarted service is waited for
        // again -- while ready_timed_out deliberately is NOT (see the
        // field). The deadline is armed from the spawn rather than from
        // the boot, so a service restarted an hour in gets its full
        // timeout and not a deadline that expired long ago.
        s->ready = 0;
        s->ready_deadline_ms = s->started_ms + s->ready_timeout_ms;
        snprintf(g_msg, sizeof g_msg, "init: started %s as pid %d\n",
                 s->name, pid);
        sys_eprint(g_msg);
        return;
    }

    // A spawn that fails is a fast failure like any other -- it is the
    // shape a missing or unseeded binary takes, and without counting it
    // a bad Exec= path retries forever. It is also an EXIT for status
    // purposes: a one-shot that never ran is not `done`, and nothing
    // else would set the code, since no child exists to report one.
    s->fast_failures++;
    s->last_exit = -1;
    snprintf(g_msg, sizeof g_msg, "init: %s failed to start (%s)\n",
             s->name, s->exec);
    sys_eprint(g_msg);
    service_failed(s);
}

// Records an exit and decides what happens next. Returns 1 if `pid`
// was one of ours.
static int service_exited(int pid, int code) {
    for (int i = 0; i < g_svc_count; i++) {
        struct service *s = &g_svc[i];
        if (s->pid != pid) continue;

        unsigned long long ran = now_ms() - s->started_ms;
        s->pid = 0;
        s->last_exit = code;

        snprintf(g_msg, sizeof g_msg,
                 "init: %s (pid %d) exited with code %d after %u ms\n",
                 s->name, pid, code, (unsigned)ran);
        sys_eprint(g_msg);

        // AN ADMIN STOP OUTRANKS THE RESTART POLICY, including
        // `Restart=always`. It is checked before the policy for the same
        // reason the flag exists at all: the exit code here is 128 +
        // SIGTERM, which every policy below reads as a failure.
        if (s->admin_stopped) {
            logf1("init: %s stopped\n", s->name);
            return 1;
        }

        if (s->restart == SVC_RESTART_NO) return 1;

        // A CLEAN exit is a request to stop, not a failure. Only
        // `Restart=always` overrides that -- see the enum's comment.
        if (code == 0 && s->restart != SVC_RESTART_ALWAYS) {
            s->stopped = 1;
            logf1("init: %s exited cleanly -- not restarting it\n", s->name);
            return 1;
        }

        // A service that ran for a while and then stopped is a restart;
        // one that died immediately is a loop forming. Only the second
        // kind accumulates, which is what lets a desktop be killed
        // repeatedly by hand without ever being given up on.
        if (ran < SVC_FAST_MS) s->fast_failures++;
        else s->fast_failures = 0;

        service_failed(s);
        return 1;
    }
    return 0;
}

// --- readiness -------------------------------------------------------
//
// Does this service still hold up anything ordered after it?
//
// EVERY "it will never answer" CASE RELEASES THE BARRIER, and that list
// is the whole safety argument: a `Ready=notify` service that was given
// up on, disabled, asked to stay down, or is a one-shot that has
// already run cannot ever call SYS_NOTIFY_READY, so waiting for it
// would be waiting forever. The timeout covers the remaining case --
// a service that is alive and simply never says anything.
static int svc_gates_dependents(const struct service *s) {
    if (s->ready_mode != SVC_READY_NOTIFY) return 0; // launch order only
    if (s->ready || s->ready_timed_out) return 0;
    if (s->gave_up || s->disabled || s->stopped || s->admin_stopped) return 0;
    if (!s->exec[0]) return 0;                       // nothing to run
    if (s->restart == SVC_RESTART_NO && s->started_once && !s->pid) return 0;
    return 1;
}

// Whether anything `idx` is ordered after has yet to become available.
// Reads g_edge[][] rather than the After= text, so `Before=` on the
// other end gates exactly the same way -- the two keys were made one
// edge for precisely this reason.
static int svc_waiting_on_deps(int idx) {
    for (int j = 0; j < g_svc_count; j++)
        if (g_edge[j][idx] && svc_gates_dependents(&g_svc[j])) return 1;
    return 0;
}

// How many services have been started and have yet to resolve -- either
// by announcing themselves or by running out of time.
//
// **COUNTED AFTER start_due(), NEVER BEFORE IT.** A service started on
// this pass is exactly the one whose announcement is about to arrive,
// and counting before the start left init blocking in waitpid(-1) with
// nothing that could wake it: the readiness bit sets no channel, and a
// desktop reporting ready produces no event at all. It cost a boot
// where the desktop came up perfectly and init never noticed.
static int count_awaiting_ready(void) {
    int n = 0;
    for (int i = 0; i < g_svc_count; i++)
        if (g_svc[i].started_once && svc_gates_dependents(&g_svc[i])) n++;
    return n;
}

// Collects readiness announcements and expires the ones that never
// came.
//
// **IT READS THE PROCESS TABLE, WHICH IS THE ONLY CHANNEL THERE IS.**
// SYS_NOTIFY_READY sets a bit on the caller's row and does not wake
// anybody -- deliberately, so the kernel holds no service-manager
// policy -- so this is a poll, and it is bounded twice over: it runs
// only while some notify service has yet to resolve, and each such
// service resolves within its ReadyTimeout. An idle machine never
// reaches this function's second line.
//
// The table is walked by SLOT and matched by PID, not indexed: pid ==
// slot + 1 is a scheduler internal, and SYS_PROC_INFO's contract is
// that a caller enumerates.
static void poll_readiness(void) {
    unsigned long long now = now_ms();
    if (!count_awaiting_ready()) return;

    struct proc_info pi;
    for (int slot = 0; slot < SYS_PROC_MAX; slot++) {
        if (sys_proc_info(slot, &pi) != 0 || pi.pid == 0 || !pi.ready) continue;
        for (int i = 0; i < g_svc_count; i++) {
            struct service *s = &g_svc[i];
            if (s->pid != pi.pid || s->ready) continue;
            if (s->ready_mode != SVC_READY_NOTIFY) continue;
            s->ready = 1;
            // The INTERVAL rather than a timestamp, because that is the
            // number a person reads: "how long was this service up
            // before it was usable" is the question readiness exists to
            // make answerable at all.
            snprintf(g_msg, sizeof g_msg, "init: %s is ready after %u ms\n",
                     s->name, (unsigned)(now - s->started_ms));
            sys_eprint(g_msg);
        }
    }

    for (int i = 0; i < g_svc_count; i++) {
        struct service *s = &g_svc[i];
        if (!s->started_once || !svc_gates_dependents(s)) continue;
        if (now < s->ready_deadline_ms) continue;
        s->ready_timed_out = 1;
        snprintf(g_msg, sizeof g_msg,
                 "init: %s did not report ready in %u ms -- starting the rest anyway\n",
                 s->name, (unsigned)s->ready_timeout_ms);
        sys_eprint(g_msg);
    }
}

// --- the control channel ---------------------------------------------

// Set by the SIGHUP handler and read by the loop. A FLAG IS ALL A
// HANDLER MAY DO: it interrupts the loop between any two instructions,
// so reading a file, spawning or logging from inside it would run in
// the middle of whatever the loop was halfway through -- and `malloc`
// here is not async-signal-safe (its lock is not recursive).
static volatile int g_hup;

static void on_hup(int sig) { (void)sig; g_hup = 1; }

// NO SA_RESTART, and that is the entire point of using sigaction()
// here rather than sys_signal(): a restarted waitpid(-1) goes straight
// back to blocking and the loop never gets a chance to read the request.
// The doorbell IS the -EINTR.
static void install_hup_handler(void) {
    struct k_sigaction act = {
        .handler  = (uint64_t)(uintptr_t)on_hup,
        .restorer = (uint64_t)(uintptr_t)__sigrestore,
        .flags    = 0,
    };
    if (sys_sigaction(SIGHUP, &act, 0) < 0)
        sys_eprint("init: could not install the SIGHUP handler -- "
                   "`service` will not be able to reach me\n");
}

// Finds a service by name, or NULL. Names are what a descriptor's
// `Name=` says, which is also what After=/Before= resolve against.
static struct service *find_service(const char *name) {
    for (int i = 0; i < g_svc_count; i++)
        if (k_strcmp(g_svc[i].name, name) == 0) return &g_svc[i];
    return 0;
}

static void control_start(const char *name) {
    struct service *s = find_service(name);
    if (!s) { logf1("init: start: no service named %s\n", name); return; }
    if (s->disabled) {
        logf1("init: start: %s has no descriptor -- put one back in "
              SERVICES_DIR "\n", s->name);
        return;
    }
    if (s->pid) { logf1("init: start: %s is already running\n", s->name); return; }

    // EVERY reason it is down is cleared, which is what makes `start`
    // mean "start it" rather than "start it unless something earlier
    // decided otherwise". The crash-loop counter goes with them: an
    // operator asking again is explicitly overruling the give-up, and
    // leaving the count would give the service one attempt before it
    // gave up again.
    s->admin_stopped = 0;
    s->stopped       = 0;
    s->gave_up       = 0;
    s->fast_failures = 0;
    s->due_ms        = 0;
    // A `Restart=no` service has already had its one run; asking for it
    // again is a second run, not a restart, so the flag that would skip
    // it is cleared too.
    s->started_once  = 0;
    logf1("init: start: %s\n", s->name);
}

static void control_stop(const char *name) {
    struct service *s = find_service(name);
    if (!s) { logf1("init: stop: no service named %s\n", name); return; }

    // SET BEFORE THE SIGNAL, never after: the child's death is what
    // wakes the loop, and a flag set after sending it races the exit
    // handling that would otherwise restart the service.
    s->admin_stopped = 1;
    if (!s->pid) { logf1("init: stop: %s is not running\n", s->name); return; }

    // SIGTERM, and no escalation to SIGKILL after a timeout the way
    // systemd does. A service that ignores it stays up and says so in
    // the status file, which is a state an operator can see and reach
    // with `kill -9`; a timeout that force-kills would be a policy this
    // has no second caller for yet.
    snprintf(g_msg, sizeof g_msg, "init: stop: sending SIGTERM to %s (pid %d)\n",
             s->name, s->pid);
    sys_eprint(g_msg);
    sys_kill(s->pid, SIGTERM);
}

// Reads and obeys the request file. Deleted afterwards WHETHER OR NOT
// every line made sense -- a request that could not be parsed has been
// reported, and leaving it would make init re-run it on every later
// doorbell.
static char g_ctl[512];

static void apply_control(void) {
    int fd = open(CONTROL_PATH, O_RDONLY);
    if (fd < 0) return;                       // a bare `reload`: no file
    int64_t n = read(fd, g_ctl, sizeof g_ctl - 1);
    close(fd);
    unlink(CONTROL_PATH);
    if (n <= 0) return;
    g_ctl[n] = '\0';

    char *p = g_ctl;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';

        char *sp = line;
        while (*sp && *sp != ' ') sp++;
        if (!*sp) continue;                   // no name: nothing to act on
        *sp++ = '\0';
        while (*sp == ' ') sp++;
        if (!*sp) continue;

        if (k_strcmp(line, "start") == 0)      control_start(sp);
        else if (k_strcmp(line, "stop") == 0)  control_stop(sp);
        else logf1("init: unknown control request %s\n", line);
    }
}

// --- the status file -------------------------------------------------

// One word for why a service is where it is. Ordered by how much it
// overrides: a running service is running whatever else is set, and
// `disabled` outranks the rest because a service with no descriptor is
// no longer declared at all.
static const char *svc_state(const struct service *s) {
    if (s->pid)                                       return "running";
    if (s->disabled)                                  return "disabled";
    if (s->admin_stopped)                             return "stopped";
    if (s->gave_up)                                   return "crash-loop";
    if (!s->exec[0])                                  return "no-exec";
    if (s->stopped)                                   return "exited";
    if (s->restart == SVC_RESTART_NO && s->started_once)
        return s->last_exit == 0 ? "done" : "failed";
    if (s->due_ms > now_ms())                         return "waiting";
    return "starting";
}

// `-` for a service that never announces anything, so the column does
// not claim a spawn-mode service is "not ready" -- it has nothing to be.
static const char *svc_ready(const struct service *s) {
    if (s->ready_mode != SVC_READY_NOTIFY) return "-";
    // The column is about the process that is RUNNING. `ready` is left
    // set after an exit (it is cleared at the next start, so that the
    // readiness barrier is not re-armed on the way down), which would
    // otherwise show a stopped service as ready.
    if (!s->pid)            return "-";
    if (s->ready)           return "yes";
    if (s->ready_timed_out) return "timeout";
    return "no";
}

// PUBLISHED ON DEMAND, AND ONLY WHEN THE MACHINE IS SETTLED. Two gates,
// and each one is load-bearing:
//
//   g_publish -- nothing is written until somebody rings the doorbell.
//   There is no tmpfs here, so every write is a real disk transaction,
//   and a status nobody has asked for is one nobody reads.
//
//   `settled` -- never while a service is in a restart backoff or has
//   yet to announce itself. That is the honest description of the file:
//   it says where things CAME TO REST. It was ALSO believed to dodge a
//   compositor that a startup write wedged; there was no such bug --
//   the stall was serial_putc() waiting on a stalled COM1 consumer.
//
// WRITTEN ONLY WHEN IT CHANGED, once both gates are open: the loop runs
// on every child exit and every doorbell, and a service manager that
// rewrites a file each pass writes to the disk forever on an idle
// machine.
static char g_status[2048];
static char g_status_prev[2048];
static int g_publish;

static void write_status(int settled) {
    if (!g_publish || !settled) return;

    int n = snprintf(g_status, sizeof g_status,
                     "# NAME             STATE       PID  FAILS  READY    EXEC\n");
    for (int i = 0; i < g_svc_count && n > 0 && n < (int)sizeof g_status; i++) {
        struct service *s = &g_svc[i];
        n += snprintf(g_status + n, sizeof g_status - (unsigned)n,
                      "%-18s %-10s %4d %6d  %-7s  %s\n",
                      s->name, svc_state(s), s->pid, s->fast_failures,
                      svc_ready(s), s->exec);
    }
    if (n <= 0 || n >= (int)sizeof g_status) return;   // a formatter that
                                                       // does not fit writes
                                                       // nothing (kfmt.h)
    if (k_strcmp(g_status, g_status_prev) == 0) return;

    int fd = open(STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;                // no disk, or a full one: not fatal
    write(fd, g_status, (size_t)n);
    close(fd);
    k_strlcpy(g_status_prev, g_status, sizeof g_status_prev);
}

// Starts everything that is down and due. Returns the number of
// services still waiting on a backoff or on a dependency, which is what
// decides whether the loop may block.
static int start_due(void) {
    unsigned long long now = now_ms();
    int pending = 0;

    for (int oi = 0; oi < g_svc_count; oi++) {
        // g_order[] is the topological order, not g_svc[]'s. A service
        // waiting on a backoff does NOT hold up the ones after it: this
        // is launch ORDER, not a barrier, and blocking here would let a
        // crash-looping service keep the rest of the machine down.
        struct service *s = &g_svc[g_order[oi]];
        // `Restart=no` still gets its ONE start -- the key says what
        // happens when it EXITS, not whether it runs at all. Reading it
        // as "do not start" is the obvious misreading and would leave a
        // one-shot service silently never running.
        if (s->pid || s->gave_up || s->disabled) continue;
        // A descriptor with no Exec= (see load_service). Skipped rather
        // than spawned-and-failed, so the log's refusal is the truth.
        if (!s->exec[0]) continue;
        if (s->restart == SVC_RESTART_NO && s->started_once) continue;
        if (s->stopped) continue;       // it asked to stay down
        if (s->admin_stopped) continue; // somebody else asked it to
        if (now < s->due_ms) { pending++; continue; }
        // THE BARRIER, and the one place launch order stops being the
        // whole story. Counted as pending rather than skipped, so the
        // loop keeps polling instead of blocking on a wake that a
        // readiness announcement will not produce.
        //
        // It holds up only the services ordered after this one. A
        // notify service that is slow does not delay an unrelated one,
        // for the same reason a backoff does not: this is an ordering
        // constraint, not a boot phase.
        if (svc_waiting_on_deps(g_order[oi])) { pending++; continue; }
        start_service(s);
        if (!s->pid && !s->gave_up) pending++;
    }
    return pending;
}

// THE ENVIRONMENT EVERY PROCESS ON THIS SYSTEM INHERITS.
//
// init is pid 1 and the ancestor of everything, and inheritance here is
// a library convention rather than a kernel one (sys_spawn passes
// `environ`) -- so whatever init sets is what the whole tree gets, and
// this is the one place it comes from. That is how a real system does
// it too: on Linux the environment a login shell has was put there by
// pid 1 and passed down, not stored anywhere.
//
// Deliberately SMALL. Two variables that are true about this system,
// rather than a list copied from a Unix that has daemons, locales and
// terminals to describe. Anything that cannot be answered honestly
// (TERM, USER, SHELL) is better absent: a program reading TERM=xterm
// here would be told a lie it then acts on.
static void seed_environment(void) {
    // /bin is where every program lives (docs/filesystem-layout.md) and
    // is what tosh already searches; saying so in PATH means a ported
    // program that builds its own search list agrees with the shell.
    setenv("PATH", "/bin", 1);
    // There is one user and no home directories, so HOME is the root.
    // It exists because ported code reaches for it constantly and
    // handles it being unset far less often than it should.
    setenv("HOME", "/", 1);
}

int main(void) {
    // No pid of its own to report -- there is no getpid() in this ABI,
    // and the kernel names the pid it spawned init as anyway.
    sys_eprint("init: starting\n");

    seed_environment();
    install_hup_handler();
    // NEITHER FILE SURVIVES A BOOT. /tmp is not emptied here
    // (docs/filesystem-layout.md), which is the one way it differs from
    // /run being a tmpfs: a request left behind by a machine that lost
    // power would otherwise be obeyed by the next one, and last boot's
    // status would be read as this boot's.
    //
    // BEFORE ANYTHING IS SPAWNED, deliberately. These are the only
    // writes init makes during startup, and doing them here puts them
    // ahead of the desktop existing at all -- see write_status() on why
    // a write once it is starting up is not harmless.
    unlink(CONTROL_PATH);
    unlink(STATUS_PATH);
    load_target();
    g_fs_gen = sys_fs_generation();
    load_services(1);

    for (;;) {
        // A DOORBELL IS BOTH A REQUEST AND A RESCAN. Read before
        // deciding what to start, so a `service start` takes effect on
        // this pass rather than the next one.
        if (g_hup) {
            g_hup = 0;
            // FROM HERE ON THERE IS A READER. Everything before the
            // first doorbell is a boot nobody was watching, and init
            // writes nothing during it -- see write_status().
            g_publish = 1;
            apply_control();
            // Resync the generation our own read-and-delete moved, so
            // the branch below does not rescan a second time for it.
            services_changed();
            load_services(0);
        } else if (services_changed()) {
            // Rescan before deciding what to start, so a descriptor
            // added or removed since the last pass is honoured on this
            // one rather than one iteration late.
            load_services(0);
        }

        // BEFORE start_due(), so a service that became ready since the
        // last pass releases its dependents in THIS one rather than a
        // poll interval later -- and the count comes AFTER it, because
        // a service started on this very pass is one whose announcement
        // has not arrived yet. See count_awaiting_ready().
        poll_readiness();
        int pending = start_due() + count_awaiting_ready();

        // AFTER the pass, not before: what an operator wants to read is
        // where this pass left things, and writing first would publish
        // the previous state with a fresh timestamp on it. `!pending` is
        // the settled test -- see write_status().
        write_status(!pending);

        int code = 0;
        int pid;

        if (pending) {
            // A backoff is running, so the wait needs a deadline and
            // waitpid has none. This is the only polling state.
            pid = sys_waitpid_nohang(-1, &code);
            if (pid == SYS_RETRY || pid < 0) {
                sys_sleep_ms(POLL_SLEEP_MS);
                continue;
            }
        } else {
            // INTERRUPTIBLE, because a control request is not a child
            // exiting: sys_waitpid() retries -EINTR and would park init
            // again with the doorbell unanswered.
            pid = sys_waitpid_intr(-1, &code);
        }

        if (pid > 0) {
            if (!service_exited(pid, code)) {
                // Not one of ours: an orphan the kernel handed us.
                // Report every reap on stderr, which reaches the kernel
                // log and `dmesg` -- an init that silently absorbs
                // corpses is one nobody can tell apart from an init
                // that has died.
                snprintf(g_msg, sizeof g_msg,
                         "init: reaped orphan pid %d (code %d)\n", pid, code);
                sys_eprint(g_msg);
            }
            continue;
        }

        // A DOORBELL LOOKS EXACTLY LIKE "no children" HERE -- both
        // land as a negative return. Sleeping on it would delay every
        // request by the idle interval for no reason, so the flag is
        // asked before the sleep rather than after it.
        if (g_hup) continue;

        // -1: no children at all. Nothing to wait on, so idle.
        sys_sleep_ms(IDLE_SLEEP_MS);
    }
}
