// The diagnostic registry: named ring-3 services, asked from either
// ring. See abi/diag_abi.h for the protocol and why this is the
// kernel's.
//
// **THIS IS THE WINDOW SERVER'S OLD `gui` RELAY, WITH THE ENDPOINT MADE
// A NAME.** Everything here except the name table was already generic --
// one command at a time, an owner that lapses, a reply chunked back to
// the caller -- and it sat in win_server.c only because the compositor
// was the first service anybody wanted to interrogate.
#include "diag.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf
#include "kerrno.h"
#include "timer.h"   // pit_ticks() -- the owner lapse and the console's wait
#include "futex.h"   // futex_note_ready() -- how a provider is woken
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "vmm.h"
#include "scheduler.h"
#include <stddef.h>

#define DIAG_MAX_PROVIDERS 8

struct provider {
    char name[DIAG_NAME_LEN];
    int  pid;
};
static struct provider g_prov[DIAG_MAX_PROVIDERS];

// --- the one channel --------------------------------------------------
//
// ONE DIAGNOSTIC AT A TIME, AND THE SECOND CALLER IS TOLD SO. The
// buffers below are a single slot, and two callers interleaving a
// command with somebody else's chunk drain would read each other's
// bytes -- so a command arriving while another is in flight is REFUSED
// with -EBUSY rather than served.
//
// THE CLAIM EXPIRES, and that is not belt-and-braces: a client killed
// between its command and its last chunk would otherwise hold the
// channel until reboot, and a diagnostic nobody can run is a worse
// failure than one that can be raced. Three seconds; a real drain is
// milliseconds.
static char g_reply[DIAG_REPLY_MAX];
static int  g_len;          // bytes of reply held
static int  g_sent;         // how many of them have gone out
static int  g_owner;        // 0 = free. DIAG_PID_KERNEL is the console
static uint64_t g_owner_until;
#define DIAG_OWNER_TICKS 300

// The command handed to a provider and not yet answered, and where its
// answer lands. Separate from g_reply, which is the CHUNKING buffer the
// caller drains -- writing straight into that would race the chunk being
// sent.
static char g_pending[DIAG_CMD_LEN];
static int  g_pending_valid;
static int  g_pending_pid;      // which provider owes us the answer
static char g_incoming[DIAG_REPLY_MAX];
static uint32_t g_incoming_len;
static int  g_reply_ready;
static unsigned g_reply_flags;
static int  g_awaiting;
static uint64_t g_await_until;

// How long a caller waits for a provider. Two seconds: long enough that
// a service busy with a slow frame still answers, short enough that a
// WEDGED one does not hang the console -- which would take the whole
// test harness down with it, since every GUI tool arrives this way.
//
// A timeout is an EMPTY reply, never an unknown command: those are
// different facts and the tools distinguish them.
#define DIAG_WAIT_TICKS 200u

static struct provider *find(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < DIAG_MAX_PROVIDERS; i++)
        if (g_prov[i].pid && k_strcmp(g_prov[i].name, name) == 0)
            return &g_prov[i];
    return NULL;
}

int diag_have_provider(const char *name) { return find(name) != NULL; }

int diag_list(char *out, int cap) {
    int n = 0, w = 0;
    for (int i = 0; i < DIAG_MAX_PROVIDERS; i++) {
        if (!g_prov[i].pid) continue;
        n++;
        if (!out || cap <= 1) continue;
        if (w && w < cap - 1) out[w++] = ' ';
        for (const char *p = g_prov[i].name; *p && w < cap - 1; p++) out[w++] = *p;
    }
    if (out && cap > 0) out[w < cap ? w : cap - 1] = '\0';
    return n;
}

void diag_provider_gone(int pid) {
    if (pid <= 0) return;
    for (int i = 0; i < DIAG_MAX_PROVIDERS; i++)
        if (g_prov[i].pid == pid) { g_prov[i].pid = 0; g_prov[i].name[0] = '\0'; }

    // A PROVIDER THAT DIES MID-ANSWER MUST NOT LEAVE A CALLER WAITING
    // for a reply that can never come. The wait would time out anyway;
    // releasing here turns a two-second stall into an immediate empty
    // answer, which is the honest one.
    if (g_awaiting && g_pending_pid == pid) {
        g_awaiting = 0;
        g_pending_valid = 0;
        g_reply_ready = 1;
        g_incoming_len = 0;
    }
    if (g_owner == pid) g_owner = 0;
}

// Fills `msg` with the next chunk of the held reply.
static void take_chunk(struct diag_msg *msg) {
    int left = g_len - g_sent;
    if (left < 0) left = 0;
    int n = left > DIAG_CHUNK ? DIAG_CHUNK : left;
    for (int i = 0; i < n; i++) msg->text[i] = g_reply[g_sent + i];
    msg->text[n] = '\0';

    g_sent += n;
    msg->type = DIAG_OUT;
    msg->len = (uint32_t)n;
    // What makes the reply self-delimiting -- a chunk that exactly fills
    // the buffer is otherwise indistinguishable from a truncated one.
    if (g_sent < g_len) msg->flags |= DIAG_F_MORE;
    else g_owner = 0;   // fully drained -- the channel is free again

    // WHAT THE PROVIDER SAID ABOUT THE COMMAND, not about this chunk.
    msg->flags |= (g_reply_flags & DIAG_F_UNKNOWN);
}

// Hands the command to `p` and returns at once.
static int post(struct provider *p, const char *line) {
    k_strlcpy(g_pending, line, sizeof g_pending);
    g_pending_valid = 1;
    g_pending_pid = p->pid;
    g_reply_ready = 0;
    g_reply_flags = 0;
    g_incoming_len = 0;

    // THE WAKEWORD, not a window event. That is the whole generalisation:
    // the old relay posted WIN_EV_CLIENT_DEBUG to the compositor's event
    // queue, which only a windowing client has. Every process has a
    // wakeword, so a service with no window is reachable the same way.
    futex_note_ready(p->pid);
    return 0;
}

static int collect(char *out, int cap) {
    if (!g_reply_ready) return -1;
    g_pending_valid = 0;
    int n = (int)g_incoming_len;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = g_incoming[i];
    return n;
}

// THE CONSOLE'S round trip, and ONLY the console's. It waits in place
// with `sti; hlt`, which is legal for one reason: the serial debug
// console is not a scheduled process, so there is no trapframe to
// corrupt and nothing to switch away from. A SYSCALL may not do this --
// api/scheduler.h says so in as many words, and handing a ring-3 caller
// this path faulted inside isr_common on the first try. Ring 3 gets
// PENDING and polls from its own side instead.
//
// Interrupts must be ON: the timer is what schedules the provider that
// owes us the answer, so waiting with them off deadlocks against the
// very process being waited for.
static int wait_here(struct provider *p, const char *line, char *out, int cap) {
    if (post(p, line) < 0) return -1;
    uint64_t deadline = pit_ticks() + DIAG_WAIT_TICKS;
    while (!g_reply_ready && pit_ticks() < deadline)
        __asm__ volatile ("sti; hlt");

    g_pending_valid = 0;
    if (!g_reply_ready) {
        klog_printf("diag: %s did not answer in time\n", p->name);
        return 0; // empty, and deliberately NOT "unknown" -- see above
    }
    return collect(out, cap);
}

int diag_request(int pid, struct diag_msg *msg) {
    if (!msg) return -1;

    unsigned in_flags = msg->flags;
    msg->flags = 0;
    msg->reserved = 0;
    msg->name[DIAG_NAME_LEN - 1] = '\0';   // caller data
    msg->text[DIAG_CHUNK] = '\0';

    switch (msg->type) {

    case DIAG_CLAIM: {
        if (pid <= 0) return 0;
        struct provider *held = find(msg->name);
        if (held) return held->pid == pid;   // idempotent, refused to others
        for (int i = 0; i < DIAG_MAX_PROVIDERS; i++) {
            if (g_prov[i].pid) continue;
            k_strlcpy(g_prov[i].name, msg->name, DIAG_NAME_LEN);
            g_prov[i].pid = pid;
            klog_printf("diag: %s claimed by pid %d\n", g_prov[i].name, pid);
            return 1;
        }
        return 0;
    }

    case DIAG_RELEASE: {
        struct provider *p = find(msg->name);
        if (!p || p->pid != pid) return 0;
        p->pid = 0; p->name[0] = '\0';
        return 1;
    }

    case DIAG_TAKE: {
        // A wake says only "look at your sources", so a provider asks
        // this on every one and usually gets nothing. Answering 0 is the
        // normal case, not a failure.
        if (!g_pending_valid || g_pending_pid != pid) {
            msg->len = 0; msg->text[0] = '\0';
            return 0;
        }
        k_strlcpy(msg->text, g_pending, DIAG_CMD_LEN);
        msg->len = (uint32_t)k_strlen(msg->text);
        // Cleared on TAKE, not on reply: a second TAKE must get nothing
        // rather than run the same command twice.
        g_pending_valid = 0;
        return 1;
    }

    case DIAG_REPLY: {
        if (g_pending_pid != pid) return 0;
        // APPENDED, not assigned: one message carries DIAG_CHUNK bytes
        // and a reply may be longer, so a provider sends several with
        // DIAG_F_MORE set on every piece but the last. Only that last
        // one releases the waiter -- otherwise a caller would print the
        // first 512 bytes of an answer and call it the whole thing.
        uint32_t n = msg->len;
        if (n > DIAG_CHUNK) n = DIAG_CHUNK;
        for (uint32_t i = 0; i < n && g_incoming_len < sizeof g_incoming; i++)
            g_incoming[g_incoming_len++] = msg->text[i];
        g_reply_flags |= (in_flags & DIAG_F_UNKNOWN);
        if (!(in_flags & DIAG_F_MORE)) g_reply_ready = 1;
        return 1;
    }

    case DIAG_MORE: {
        // A stranger asking for more gets the SAME empty final chunk a
        // caller one call too late gets -- not an error and not somebody
        // else's bytes, so their loop ends instead of consuming a reply
        // they never asked for.
        if (g_owner && pid != g_owner) {
            msg->type = DIAG_OUT; msg->len = 0; msg->text[0] = '\0';
            return 1;
        }
        if (g_awaiting) {
            int n = collect(g_reply, (int)sizeof g_reply);
            if (n < 0) {
                if (pit_ticks() >= g_await_until) {
                    klog_write("diag: provider did not answer in time\n");
                    g_awaiting = 0; g_pending_valid = 0; g_owner = 0;
                    msg->type = DIAG_OUT; msg->len = 0; msg->text[0] = '\0';
                    return 1;
                }
                // Still waiting: say so rather than handing back an empty
                // final chunk, which a caller cannot tell from a command
                // that legitimately printed nothing.
                msg->type = DIAG_OUT; msg->flags = DIAG_F_PENDING;
                msg->len = 0; msg->text[0] = '\0';
                return 1;
            }
            g_awaiting = 0;
            g_len = n;
            g_sent = 0;
        }
        take_chunk(msg);
        return 1;
    }

    case DIAG_CMD: {
        if (g_owner && g_owner != pid && pit_ticks() < g_owner_until)
            return -EBUSY;

        struct provider *p = find(msg->name);
        if (!p) {
            msg->type = DIAG_OUT; msg->len = 0; msg->text[0] = '\0';
            return 0;
        }

        g_owner = pid;
        g_owner_until = pit_ticks() + DIAG_OWNER_TICKS;
        g_len = 0; g_sent = 0; g_reply_flags = 0;

        if (pid > 0) {
            // A PROCESS. Post and hand back PENDING: the wait happens in
            // the caller's own ring, because a syscall may not block
            // here (see wait_here).
            if (post(p, msg->text) < 0) {
                g_owner = 0;
                msg->type = DIAG_OUT; msg->len = 0; msg->text[0] = '\0';
                return 0;
            }
            g_awaiting = 1;
            g_await_until = pit_ticks() + DIAG_WAIT_TICKS;
            msg->type = DIAG_OUT; msg->flags = DIAG_F_PENDING;
            msg->len = 0; msg->text[0] = '\0';
            return 1;
        }

        int n = wait_here(p, msg->text, g_reply, (int)sizeof g_reply);
        if (n < 0) {
            msg->type = DIAG_OUT; msg->len = 0; msg->text[0] = '\0';
            return 0;
        }
        if (n > (int)sizeof g_reply) n = (int)sizeof g_reply;
        g_len = n;
        take_chunk(msg);
        return 1;
    }

    default:
        return -1;
    }
}

// SYS_DIAG. Copy in, act, copy back: the message is handled against a
// KERNEL copy, never against the user page directly, which the caller
// shares and could change between validation and use.
int sys_diag(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    // THE PROCESS, not the calling thread: a provider is a program, so a
    // second thread of it must be able to answer for the same name.
    int pid = scheduler_current_tgid();

    struct diag_msg msg;
    if (!vmm_copy_from_user(pml4, &msg, c->a0, sizeof msg)) {
        klog_write(KLOG_ERR "syscall: diag() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (pid == 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else {
        int rc = diag_request(pid, &msg);
        if (!vmm_copy_to_user(pml4, c->a0, &msg, sizeof msg)) rc = -EFAULT;
        c->regs[14] = (uint64_t)(int64_t)rc;
    }
    return 0;
}
