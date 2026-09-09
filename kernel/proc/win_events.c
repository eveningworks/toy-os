// Per-process windowing-event queues. See win_events.h for the API and
// abi/win_proto.h for the message format this delivers.
//
// One fixed-size ring per process slot, sized by WIN_EVENT_QUEUE_MAX
// and statically allocated rather than kmalloc'd: these are written
// from interrupt handlers (a keyboard or mouse IRQ delivering to a
// client), and allocating on that path would mean taking the heap's
// state in an interrupt. Static storage costs a few KB and removes the
// question entirely.
#include "win_events.h"
#include "futex.h"
#include "scheduler.h"
#include <stddef.h>

// Indexed by pid - 1, matching scheduler.c's 1-based pids. Kept here
// rather than inside struct sched_process so the scheduler doesn't grow
// a dependency on the windowing protocol -- it schedules processes, it
// has no business knowing what a window event is.
// Derived from SCHED_MAX_PROCS, never a literal -- api/scheduler.h says
// so in as many words, and this table is what happens when that is
// ignored: it read `4` long after MAX_PROCS became 64, with a comment
// still claiming the two matched. queue_for() then returned NULL for
// every pid above 4, so win_events_push() silently returned 0 and a
// client in slot 4 or beyond received NO window events of any kind --
// no keys, no mouse, no close, no resize, no ping. It draws perfectly
// and answers nothing, and the force-quit path reads it as wedged.
//
// Reachable with five processes alive at once (pids are slot + 1, first
// free slot reused). win_server.c's neighbouring table got a
// _Static_assert and therefore did not drift; this one had none, which
// is the whole reason to state the dependency twice.
#define WIN_EVENTS_MAX_PIDS SCHED_MAX_PROCS

_Static_assert(WIN_EVENTS_MAX_PIDS >= SCHED_MAX_PROCS,
               "a scheduled process can have no event queue");

struct event_queue {
    struct win_event ring[WIN_EVENT_QUEUE_MAX];
    int head;    // next slot to pop
    int count;   // how many are queued
    int dropped; // overflow drops since the last reset

    // **HAS THIS PROCESS EVER ASKED FOR A WINDOW EVENT?** Nothing else
    // calls SYS_WAIT_EVENT or SYS_POLL_EVENT, so this is "is it a
    // windowing client" -- the one question the kernel's window table
    // used to answer that is not window state. Two things need it and
    // neither cares about a window: the font broadcast, and asking
    // every client to close when the compositor dies.
    //
    // **THE COMPOSITOR IS NOT IN THIS SET.** It parks on its channel's
    // futex rather than on a window event, so both callers reach it
    // separately -- see win_server_broadcast(), where forgetting that
    // has now cost a font change twice.
    //
    // Cleared with the queue at spawn, so a reused slot never inherits
    // the last tenant's answer.
    int client;
};

static struct event_queue queues[WIN_EVENTS_MAX_PIDS];

static struct event_queue *queue_for(int pid) {
    if (pid < 1 || pid > WIN_EVENTS_MAX_PIDS) return NULL;
    return &queues[pid - 1];
}

void win_events_reset(int pid) {
    struct event_queue *q = queue_for(pid);
    if (!q) return;
    q->head = 0;
    q->count = 0;
    q->dropped = 0;
    q->client = 0;
}

// Input is what a full queue sheds -- the compositor's raw events and a
// client's delivered ones alike. Everything else, a notification
// (WIN_EV_SCREEN, WIN_EV_FONT, WIN_EV_CLOSE) or a client's request to
// the compositor, is a fact the receiver cannot re-derive by looking.
static int is_input(uint32_t type) {
    switch (type) {
    case WIN_EV_RAW_MOUSE: case WIN_EV_RAW_KEY: case WIN_EV_RAW_KEY_UP: case WIN_EV_RAW_WHEEL:
    case WIN_EV_MOUSE_MOVE: case WIN_EV_MOUSE_DOWN: case WIN_EV_MOUSE_UP:
    case WIN_EV_KEY: case WIN_EV_KEY_UP: case WIN_EV_WHEEL:
        return 1;
    default:
        return 0;
    }
}

static int is_motion(uint32_t type) {
    return type == WIN_EV_RAW_MOUSE || type == WIN_EV_MOUSE_MOVE;
}

int win_events_push(int pid, const struct win_event *ev) {
    struct event_queue *q = queue_for(pid);
    if (!q || !ev) return 0;

    // MOTION IS A STATE, NOT A BACKLOG (Windows holds one WM_MOUSEMOVE
    // per queue; X compresses MotionNotify). A move with the same
    // buttons as the NEWEST queued move replaces it, so motion never
    // holds more than one slot -- a mouse moving through one slow frame
    // evicted a WIN_EV_SCREEN and left the desktop painting the old
    // mode. Only the newest slot: a press in between keeps its place.
    if (is_motion(ev->type) && q->count > 0) {
        struct win_event *last = &q->ring[(q->head + q->count - 1) % WIN_EVENT_QUEUE_MAX];
        if (last->type == ev->type && last->window == ev->window && last->mods == ev->mods) {
            *last = *ev;
            scheduler_wake(q, 0);
            futex_note_ready(pid);
            return 1;
        }
    }

    if (q->count == WIN_EVENT_QUEUE_MAX) {
        // Full: drop the OLDEST INPUT event, and only when there is
        // none the oldest of all. See WIN_EVENT_QUEUE_MAX's comment for
        // why the old end rather than refusing the new event.
        int victim = 0;
        for (int i = 0; i < q->count; i++)
            if (is_input(q->ring[(q->head + i) % WIN_EVENT_QUEUE_MAX].type)) { victim = i; break; }
        for (int i = victim; i < q->count - 1; i++)
            q->ring[(q->head + i) % WIN_EVENT_QUEUE_MAX] = q->ring[(q->head + i + 1) % WIN_EVENT_QUEUE_MAX];
        q->count--;
        q->dropped++;
    }

    int tail = (q->head + q->count) % WIN_EVENT_QUEUE_MAX;
    q->ring[tail] = *ev;
    q->count++;

    // Wake THIS CLIENT, and only this client. The woken syscall returns
    // 0 ("try again"), NOT the event itself -- the event lives in
    // kernel memory and the client's buffer is in an address space that
    // is not current here (this may be running in an IRQ under some
    // other process's CR3), so the copy has to happen back inside the
    // client's own syscall. See syscall.c's SYS_WAIT_EVENT handler.
    //
    // The channel is this queue's address. It used to be the category
    // SCHED_WAIT_EVENT, which woke every process blocked in
    // SYS_WAIT_EVENT -- so one client's keystroke woke all of them, each
    // to pop an empty queue and park again. That is the thundering herd,
    // on the busiest path in the system: every mouse MOVE hit it.
    scheduler_wake(q, 0);
    // ...and the wakeword beside it, for a receiver waiting on SEVERAL
    // sources at once rather than on this queue alone (kernel/futex.h).
    futex_note_ready(pid);
    return 1;
}

// This client's wait channel -- see scheduler.h. The queue's own
// address, so a wake reaches exactly the process whose queue grew.
const void *win_events_wait_chan(int pid) { return queue_for(pid); }

void win_events_mark_client(int pid) {
    struct event_queue *q = queue_for(pid);
    if (q) q->client = 1;
}

int win_events_is_client(int pid) {
    struct event_queue *q = queue_for(pid);
    return q ? q->client : 0;
}

int win_events_pop(int pid, struct win_event *out) {
    struct event_queue *q = queue_for(pid);
    if (!q || !out || q->count == 0) return 0;

    *out = q->ring[q->head];
    q->head = (q->head + 1) % WIN_EVENT_QUEUE_MAX;
    q->count--;
    return 1;
}

int win_events_pending(int pid) {
    struct event_queue *q = queue_for(pid);
    return q ? q->count : 0;
}

int win_events_dropped(int pid) {
    struct event_queue *q = queue_for(pid);
    return q ? q->dropped : 0;
}
