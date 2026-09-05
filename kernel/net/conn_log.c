// The connection log's ring, its name cache, and the setting that says
// how much of it to keep. See kernel/include/kernel/conn_log.h for what
// a record is and why the hooks sit where they do.
#include "conn_log.h"
#include "net.h"
#include "net_abi.h"
#include "scheduler.h"
#include "setting.h"
#include "etc_config.h"
#include "ktime.h"
#include "clocksource.h"
#include "string.h"
#include "proc_info.h"

_Static_assert(NET_ABI_HOST_MAX == QUERY_CONNLOG_HOST_MAX,
               "a resolver's name and a log record's name must be the same "
               "size, or one truncates what the other accepted");

#define CONN_LOG_FILE "/etc/toyos.conf"

static struct query_connlog g_ring[CONN_LOG_MAX];
static uint64_t g_next_seq = 1;   // the NEXT record's number; 0 is "none"
static int g_count;               // records held, up to CONN_LOG_MAX
static int g_head;                // where the oldest one is

// The (address -> name) cache SYS_NET_RESOLVED feeds. Small on purpose:
// it exists to name the handful of hosts a boot actually talks to, and
// a resolver cache belongs in ring 3 (uresolv.h says why there isn't
// one yet).
#define NAME_CACHE_MAX 16

struct name_entry {
    uint32_t ip;
    char name[NET_ABI_HOST_MAX];
};

static struct name_entry g_names[NAME_CACHE_MAX];
static int g_name_next;   // round-robin; the oldest claim is the one lost

static int g_mode = CONN_LOG_ALL;

int conn_log_mode(void) { return g_mode; }

void conn_log_set_mode(int mode) {
    if (mode >= CONN_LOG_OFF && mode <= CONN_LOG_ALL) g_mode = mode;
}

void conn_log_name_hint(uint32_t ip, const char *name) {
    if (!ip || !name || !name[0]) return;

    // LAST CLAIM WINS for an address several names point at. A CDN
    // answers for many, and the useful one is whatever was asked for
    // most recently -- which is the one about to be connected to.
    for (int i = 0; i < NAME_CACHE_MAX; i++) {
        if (g_names[i].ip == ip) {
            k_strlcpy(g_names[i].name, name, sizeof g_names[i].name);
            return;
        }
    }
    g_names[g_name_next].ip = ip;
    k_strlcpy(g_names[g_name_next].name, name, sizeof g_names[g_name_next].name);
    g_name_next = (g_name_next + 1) % NAME_CACHE_MAX;
}

static const char *name_for(uint32_t ip) {
    for (int i = 0; i < NAME_CACHE_MAX; i++)
        if (g_names[i].ip == ip && g_names[i].name[0]) return g_names[i].name;
    return 0;
}

// The calling process's name. Walked rather than looked up because the
// scheduler indexes slots, not pids -- and it is walked HERE, at the
// connection, because the process may be gone by the time anybody reads
// the log.
static void comm_of(int pid, char *out, uint32_t cap) {
    out[0] = 0;
    if (pid <= 0) return;
    struct proc_info p;
    for (int i = 0; scheduler_proc_info(i, &p); i++)
        if (p.pid == pid) { k_strlcpy(out, p.name, cap); return; }
}

void conn_log_record(uint32_t direction, uint8_t proto,
                     uint32_t remote_ip, uint16_t remote_port,
                     uint16_t local_port) {
    if (g_mode == CONN_LOG_OFF) return;
    if (g_mode == CONN_LOG_TCP && proto != IP_PROTO_TCP) return;

    int slot = (g_head + g_count) % CONN_LOG_MAX;
    if (g_count == CONN_LOG_MAX) g_head = (g_head + 1) % CONN_LOG_MAX;
    else g_count++;

    struct query_connlog *r = &g_ring[slot];
    k_memset(r, 0, sizeof *r);
    r->seq = g_next_seq++;
    r->utc = ktime_now_sec();
    r->monotonic_ns = clocksource_now_ns();
    r->remote_ip = remote_ip;
    r->remote_port = remote_port;
    r->local_port = local_port;
    r->proto = proto;
    r->direction = direction;

    int pid = scheduler_current_pid();
    r->pid = (uint64_t)(pid > 0 ? pid : 0);
    comm_of(pid, r->comm, sizeof r->comm);

    const char *host = name_for(remote_ip);
    if (host) k_strlcpy(r->host, host, sizeof r->host);
}

int conn_log_count(void) { return g_count; }

int conn_log_at(int index, struct query_connlog *out) {
    if (index < 0 || index >= g_count || !out) return 0;
    *out = g_ring[(g_head + index) % CONN_LOG_MAX];
    return 1;
}

// ---- system.conn_log -------------------------------------------------

static const char *const MODE_NAMES[] = { "off", "tcp", "all" };

static int mode_choice(int index, char *out, uint32_t cap) {
    if (index < 0 || index > CONN_LOG_ALL) return 0;
    k_strlcpy(out, MODE_NAMES[index], cap);
    return 1;
}

static void mode_get(char *out, uint32_t cap) {
    k_strlcpy(out, MODE_NAMES[g_mode], cap);
}

static int mode_parse(const char *value) {
    for (int i = 0; i <= CONN_LOG_ALL; i++)
        if (k_strcmp(value, MODE_NAMES[i]) == 0) return i;
    return -1;
}

static int mode_apply(const char *value) {
    int m = mode_parse(value);
    if (m < 0) return SETTING_INVALID;
    conn_log_set_mode(m);
    return etc_config_set(CONN_LOG_FILE, "conn_log", value) ? SETTING_SAVED
                                                            : SETTING_UNSAVED;
}

static const struct setting g_mode_setting = {
    .name = "conn_log",
    .label = "Connection log",
    .type = SETTING_TYPE_ENUM,
    .file = CONN_LOG_FILE,
    .category = "Network",
    .choice = mode_choice,
    .get = mode_get,
    .apply = mode_apply,
};

void conn_log_setting_register(void) {
    // READ ONCE, HERE. conn_log_mode() is asked on every connection and
    // etc_config_get() re-reads the whole file per key -- the same trap
    // that made a nine-entry desktop reload 54 whole-file reads.
    char buf[16];
    if (etc_config_get(CONN_LOG_FILE, "conn_log", buf, sizeof buf)) {
        int m = mode_parse(buf);
        if (m >= 0) g_mode = m;
    }
    setting_register(&g_mode_setting);
}
