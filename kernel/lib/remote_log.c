// The remote-activity ring. See kernel/include/kernel/remote_log.h for
// what a record is and why remoteness is derived rather than declared.
#include "remote_log.h"
#include "ktime.h"
#include "clocksource.h"
#include "string.h"
#include "klog.h"
#include "scheduler.h"
#include "vmm.h"
#include "syscalls.h"
#include "errno.h"
#include "proc_info.h"
#include "ktest.h"

// Enough to cover a whole flash (a few dozen commands and the transfers
// between them) without the ring being the reason something is missed,
// and small enough that a per-session buffer was never worth it. The
// tray flyout shows the last dozen; `sessions` explains the rest.
#define REMOTE_LOG_MAX 128

static struct query_remotelog g_ring[REMOTE_LOG_MAX];
static uint64_t g_next_seq = 1;   // the NEXT record's number; 0 is "none"
static int g_count;
static int g_head;                // where the oldest one is

// Open sessions, so the tray item can hide itself when nobody is on.
// A pid rather than a count alone: a session that dies without a close
// (the shell killed, the link dropped) must not leave the indicator up
// forever, and scheduler_exit() names the leader it is retiring.
#define REMOTE_SESSIONS_MAX 8
static struct {
    int leader;
    uint32_t ip;
    uint64_t opened_utc;
    char comm[24];
    char status[QUERY_REMOTESESS_STATUS_MAX];
} g_sessions[REMOTE_SESSIONS_MAX];

void remote_log_record(int kind, uint32_t remote_ip, int pid,
                       const char *comm, const char *text) {
    int slot = (g_head + g_count) % REMOTE_LOG_MAX;
    if (g_count == REMOTE_LOG_MAX) {
        slot = g_head;
        g_head = (g_head + 1) % REMOTE_LOG_MAX;
    } else {
        g_count++;
    }

    struct query_remotelog *r = &g_ring[slot];
    k_memset(r, 0, sizeof *r);
    r->seq = g_next_seq++;
    r->utc = ktime_now_sec();
    r->monotonic_ns = clocksource_now_ns();
    r->kind = (uint64_t)kind;
    r->remote_ip = remote_ip;
    r->pid = (uint64_t)pid;
    r->sessions = (uint64_t)remote_log_sessions();
    if (comm) k_strlcpy(r->comm, comm, sizeof r->comm);
    if (text) k_strlcpy(r->text, text, sizeof r->text);
}

uint64_t remote_log_total(void) { return g_next_seq - 1; }

uint64_t remote_log_oldest(void) {
    if (!g_count) return 0;
    return g_ring[g_head].seq;
}

int remote_log_get(uint64_t seq, struct query_remotelog *out) {
    if (!out || !g_count) return 0;
    uint64_t oldest = g_ring[g_head].seq;
    if (seq < oldest || seq > remote_log_total()) return 0;
    int slot = (g_head + (int)(seq - oldest)) % REMOTE_LOG_MAX;
    // The slot's own seq is re-checked for conn_log's reason: a record
    // written between the index arithmetic and the copy moves the ring
    // under a reader, and answering the WRONG record is worse than
    // answering none.
    if (g_ring[slot].seq != seq) return 0;
    *out = g_ring[slot];
    return 1;
}

int remote_log_sessions(void) {
    int n = 0;
    for (int i = 0; i < REMOTE_SESSIONS_MAX; i++)
        if (g_sessions[i].leader) n++;
    return n;
}

// IS THIS SESSION REMOTE, AND WHOSE? The spawn path asks per spawn, so
// it is a walk of eight slots rather than anything cleverer.
uint32_t remote_log_session_of(int leader_pid) {
    if (leader_pid <= 0) return 0;
    for (int i = 0; i < REMOTE_SESSIONS_MAX; i++)
        if (g_sessions[i].leader == leader_pid) return g_sessions[i].ip;
    return 0;
}

uint32_t remote_log_session_ip(void) {
    for (int i = REMOTE_SESSIONS_MAX - 1; i >= 0; i--)
        if (g_sessions[i].leader) return g_sessions[i].ip;
    return 0;
}

void remote_log_session_opened(int leader_pid, uint32_t ip, const char *creator) {
    if (leader_pid <= 0) return;
    if (!creator || !creator[0]) creator = "?";
    for (int i = 0; i < REMOTE_SESSIONS_MAX; i++) {
        if (g_sessions[i].leader) continue;
        g_sessions[i].leader = leader_pid;
        g_sessions[i].ip = ip;
        g_sessions[i].opened_utc = ktime_now_sec();
        k_strlcpy(g_sessions[i].comm, creator, sizeof g_sessions[i].comm);
        g_sessions[i].status[0] = 0;
        remote_log_record(QUERY_REMOTE_SESSION, ip, leader_pid, creator, "session opened");
        return;
    }
    // FULL MEANS THE INDICATOR STILL GOES UP, because the alternative
    // is a session nobody can see. The record is written; only the
    // per-session slot is lost, which costs the close line.
    remote_log_record(QUERY_REMOTE_SESSION, ip, leader_pid, creator,
                      "session opened (too many to track)");
}

int remote_log_session_count(void) { return remote_log_sessions(); }

int remote_log_session_get(int index, struct query_remotesess *out) {
    for (int i = 0; i < REMOTE_SESSIONS_MAX; i++) {
        if (!g_sessions[i].leader || index--) continue;
        k_memset(out, 0, sizeof *out);
        out->pid = (uint64_t)g_sessions[i].leader;
        out->remote_ip = g_sessions[i].ip;
        out->opened_utc = g_sessions[i].opened_utc;
        k_strlcpy(out->comm, g_sessions[i].comm, sizeof out->comm);
        k_strlcpy(out->status, g_sessions[i].status, sizeof out->status);
        return 1;
    }
    return 0;
}

void remote_log_session_closed(int leader_pid) {
    for (int i = 0; i < REMOTE_SESSIONS_MAX; i++) {
        if (g_sessions[i].leader != leader_pid) continue;
        // THE SLOT GOES FIRST, so the record this writes carries the
        // count AFTER the close -- a reader takes the newest record's
        // `sessions` as the current answer, and a close that still
        // counted itself would leave the indicator up forever.
        uint32_t ip = g_sessions[i].ip;
        char comm[sizeof g_sessions[i].comm];
        k_strlcpy(comm, g_sessions[i].comm, sizeof comm);
        g_sessions[i].leader = 0;
        g_sessions[i].ip = 0;
        remote_log_record(QUERY_REMOTE_SESSION, ip, leader_pid, comm, "session closed");
        return;
    }
}

// --- the syscall ------------------------------------------------------

int sys_remote_log(struct syscall_ctx *c) {
    int kind = (int)c->a0;
    uint32_t claimed_ip = (uint32_t)c->a1;

    char text[QUERY_REMOTELOG_TEXT_MAX];
    if (!vmm_copy_string_from_user(c->pml4, text, c->a2, sizeof text)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (kind < QUERY_REMOTE_SESSION || kind > QUERY_REMOTE_STATUS) {
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    int pid = scheduler_current_pid();
    // A STATUS IS THE SESSION LEADER'S OWN LINE about its session, and
    // only a leader of a live remote session has one to set -- anyone
    // else is told so, since there is nothing they could mean.
    if (kind == QUERY_REMOTE_STATUS) {
        for (int i = 0; i < REMOTE_SESSIONS_MAX; i++) {
            if (g_sessions[i].leader != pid) continue;
            k_strlcpy(g_sessions[i].status, text, sizeof g_sessions[i].status);
            c->regs[14] = 0;
            return 0;
        }
        c->regs[14] = (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    // THE SESSION'S PEER OUTRANKS THE CALLER'S CLAIM, which is what
    // makes a shell's record trustworthy: it says what was typed, and
    // the kernel says where from. A caller with no remote session may
    // still report one it is SERVING (tftpd naming its client), and a
    // caller with neither is dropped -- silently, and still 0, because
    // a program must not behave differently for being unwatched.
    uint32_t ip = remote_log_session_of(scheduler_sid(pid));
    if (!ip) ip = claimed_ip;
    if (!ip) {
        c->regs[14] = 0;
        return 0;
    }

    struct proc_info info;
    const char *comm = "";
    if (scheduler_proc_info_pid(pid, &info)) comm = info.name;
    remote_log_record(kind, ip, pid, comm, text);
    c->regs[14] = 0;
    return 0;
}

// --- KTESTs ---------------------------------------------------------------

KTEST("remote", "a session is listed with its creator and status until it closes") {
    int pid = 0x7ffff0;   // no process has it; the table only keeps numbers
    int before = remote_log_session_count();
    remote_log_session_opened(pid, 0xC0A80167, "remoted");
    struct query_remotesess q;
    int found = 0;
    for (int i = 0; remote_log_session_get(i, &q); i++)
        if ((int)q.pid == pid) { found = 1; break; }
    int k;
    for (k = 0; k < REMOTE_SESSIONS_MAX && g_sessions[k].leader != pid; k++) {}
    if (k < REMOTE_SESSIONS_MAX) k_strlcpy(g_sessions[k].status, "VNC, view only",
                                           sizeof g_sessions[k].status);
    struct query_remotesess q2;
    int status_ok = 0;
    for (int i = 0; remote_log_session_get(i, &q2); i++)
        if ((int)q2.pid == pid) status_ok = !k_strcmp(q2.status, "VNC, view only");
    remote_log_session_closed(pid);
    KTEST_ASSERT(found);
    KTEST_ASSERT_EQ(q.remote_ip, 0xC0A80167);
    KTEST_ASSERT(!k_strcmp(q.comm, "remoted"));
    KTEST_ASSERT(status_ok);
    KTEST_ASSERT_EQ(remote_log_session_count(), before);
}
