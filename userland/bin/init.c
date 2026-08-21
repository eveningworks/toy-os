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
// ORDERING IS LAUNCH ORDER, NOT AVAILABILITY. A service is "started"
// the instant sys_spawn() returns a pid; init cannot observe more than
// that, because nothing in this system can say "I am ready" yet. So
// `After=` guarantees the spawn happened first and nothing else --
// systemd's Type=simple, which is also its default, and the honest
// description of what a fire-and-forget spawn can promise. A readiness
// protocol (systemd's Type=notify / sd_notify) is a separate item on
// docs/roadmap.md.
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
//   - A restart pending: polls with waitpid_nohang so the backoff can
//     expire, since a blocking wait has no deadline.
#include "rt/sys.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "etc_config.h"
#include "setting_abi.h"
#include "syscall_abi.h" // struct sys_dirent, SYS_O_*

#define SERVICES_DIR   "/etc/services.d"
#define TARGET_SETTING "system.default_target"

// Sixteen, not eight: ordering only means anything with several
// services, and the table is static rather than on the stack, so the
// cap costs address space instead of the ring-3 guard page. Overflow
// is refused with a line naming the descriptor, never silently.
#define SVC_MAX        16
#define SVC_NAME_MAX   24
#define SVC_EXEC_MAX   64
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

struct service {
    char name[SVC_NAME_MAX];
    char exec[SVC_EXEC_MAX];
    int  restart;      // SVC_RESTART_*
    int  pid;          // 0 when not running
    unsigned long long started_ms;
    unsigned long long due_ms;  // when it may next be started
    int  fast_failures;
    int  started_once;
    char after[SVC_DEPS_MAX];   // names that must be spawned before this
    char before[SVC_DEPS_MAX];  // names this must be spawned before
    int  seen;         // survived the last scan
    int  stopped;      // exited cleanly and asked to stay down
    int  disabled;     // its descriptor is gone -- do not start it again
    int  gave_up;
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

static int read_file(const char *path, struct etc_config_buf *buf) {
    buf->valid = 0;
    buf->size = 0;
    buf->data[0] = '\0';

    int fd = sys_open(path, 0);
    if (fd < 0) return 0;

    int64_t n = sys_read(fd, buf->data, sizeof buf->data - 1);
    sys_close(fd);
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
    if (!etc_config_buf_get(&g_cfg, "Exec", s->exec, sizeof s->exec)) {
        logf1("init: %s has no Exec=, ignoring it\n", file);
        return;
    }

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

    int pid = sys_spawn(s->exec, 0, -1);
    if (pid > 0) {
        s->pid = pid;
        s->started_ms = now_ms();
        snprintf(g_msg, sizeof g_msg, "init: started %s as pid %d\n",
                 s->name, pid);
        sys_eprint(g_msg);
        return;
    }

    // A spawn that fails is a fast failure like any other -- it is the
    // shape a missing or unseeded binary takes, and without counting it
    // a bad Exec= path retries forever.
    s->fast_failures++;
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

        snprintf(g_msg, sizeof g_msg,
                 "init: %s (pid %d) exited with code %d after %u ms\n",
                 s->name, pid, code, (unsigned)ran);
        sys_eprint(g_msg);

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

// Starts everything that is down and due. Returns the number of
// services still waiting on a backoff, which is what decides whether
// the loop may block.
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
        if (s->restart == SVC_RESTART_NO && s->started_once) continue;
        if (s->stopped) continue; // it asked to stay down
        if (now < s->due_ms) { pending++; continue; }
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
    load_target();
    g_fs_gen = sys_fs_generation();
    load_services(1);

    for (;;) {
        // Rescan before deciding what to start, so a descriptor added or
        // removed since the last pass is honoured on this one rather
        // than one iteration late.
        if (services_changed()) load_services(0);

        int pending = start_due();

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
            pid = sys_waitpid(-1, &code);
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

        // -1: no children at all. Nothing to wait on, so idle.
        sys_sleep_ms(IDLE_SLEEP_MS);
    }
}
