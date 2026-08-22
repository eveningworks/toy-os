// The physical console, as a queryable FACT: who owns it, what is in
// front of it, and whether a compositor is holding the keyboard.
//
// **WHY IT IS A FACT AND NOT THREE.** Console ownership (kernel/tty.h),
// the foreground group (kernel/tty.h) and the keyboard stand-down
// (api/keyboard.h) live in two subsystems and were separately
// unreadable from ring 3, so the question a person actually asks --
// "why is nothing responding to what I type?" -- had no answer at all.
// Read as ONE record because reading them one at a time would let the
// three disagree: a console can change hands between two syscalls, and
// an owner from one moment beside a foreground group from another is a
// state the machine was never in. Same reasoning as QUERY_FSINFO
// carrying its usage numbers rather than leaving `df` to two reads.
//
// The three flags are not redundant with each other. QUERY_TTY_
// SUSPENDED is EITHER reason (keyboard_blocking_suspended()), and the
// two below it are which one -- both can hold at once, which is why
// api/keyboard.h keeps them as separate flags rather than a boolean.
#include "query.h"
#include "tty.h"
#include "keyboard.h"
#include "win_server.h"
#include "string.h"
#include <stddef.h>

static int tty_count(void) { return 1; }

static int tty_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_tty *t = out;
    k_memset(t, 0, sizeof *t);
    t->owner_pid       = (uint64_t)(unsigned)tty_console_owner();
    t->foreground_pgid = (uint64_t)(unsigned)tty_foreground_pgid();
    t->compositor_pid  = (uint64_t)(unsigned)win_server_compositor_pid();
    if (keyboard_compositor_owns())     t->flags |= QUERY_TTY_COMPOSITOR;
    if (keyboard_console_claimed())     t->flags |= QUERY_TTY_CLAIMED;
    if (keyboard_blocking_suspended())  t->flags |= QUERY_TTY_SUSPENDED;
    return 1;
}

static const struct query_field tty_fields[] = {
    QUERY_FIELD(struct query_tty, owner_pid,       QUERY_TYPE_U64),
    QUERY_FIELD(struct query_tty, foreground_pgid, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_tty, compositor_pid,  QUERY_TYPE_U64),
    QUERY_FIELD(struct query_tty, flags,           QUERY_TYPE_U64),
};

static const struct query_provider tty_provider = {
    .cls = QUERY_TTY,
    .name = "tty",
    .record_size = sizeof(struct query_tty),
    .flags = 0,
    .count = tty_count,
    .fill = tty_fill,
    .fields = tty_fields,
    .field_count = sizeof tty_fields / sizeof tty_fields[0],
};

void tty_query_init(void) {
    query_register(&tty_provider);
}
