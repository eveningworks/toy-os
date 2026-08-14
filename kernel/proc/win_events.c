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
#include "scheduler.h"
#include <stddef.h>

// Indexed by pid - 1, matching scheduler.c's 1-based pids. Kept here
// rather than inside struct sched_process so the scheduler doesn't grow
// a dependency on the windowing protocol -- it schedules processes, it
// has no business knowing what a window event is.
#define WIN_EVENTS_MAX_PIDS 4 // MAX_PROCS (scheduler.c)

struct event_queue {
    struct win_event ring[WIN_EVENT_QUEUE_MAX];
    int head;    // next slot to pop
    int count;   // how many are queued
    int dropped; // overflow drops since the last reset
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
}

int win_events_push(int pid, const struct win_event *ev) {
    struct event_queue *q = queue_for(pid);
    if (!q || !ev) return 0;

    if (q->count == WIN_EVENT_QUEUE_MAX) {
        // Full: drop the OLDEST by advancing head, then write into the
        // slot it vacated. See WIN_EVENT_QUEUE_MAX's comment for why
        // this direction rather than refusing the new event.
        q->head = (q->head + 1) % WIN_EVENT_QUEUE_MAX;
        q->count--;
        q->dropped++;
    }

    int tail = (q->head + q->count) % WIN_EVENT_QUEUE_MAX;
    q->ring[tail] = *ev;
    q->count++;

    // Wake anything parked in SYS_WAIT_EVENT. The woken syscall returns
    // 0 ("try again"), NOT the event itself -- the event lives in
    // kernel memory and the client's buffer is in an address space that
    // is not current here (this may be running in an IRQ under some
    // other process's CR3), so the copy has to happen back inside the
    // client's own syscall. See syscall.c's SYS_WAIT_EVENT handler.
    scheduler_wake(SCHED_WAIT_EVENT, 0);
    return 1;
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
