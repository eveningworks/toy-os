// The terminals, as queryable FACTS: who owns each, what is in front of
// it, and whether a compositor is holding the keyboard away.
//
// **WHY IT IS ONE FACT AND NOT THREE.** Ownership and the foreground
// group (kernel/tty.h) and the keyboard stand-down (api/keyboard.h) live
// in two subsystems and were separately unreadable from ring 3, so the
// question a person actually asks -- "why is nothing responding to what
// I type?" -- had no answer at all. Read as ONE record because reading
// them one at a time would let them disagree: a terminal can change
// hands between two syscalls, and an owner from one moment beside a
// foreground group from another is a state the machine was never in.
// Same reasoning as QUERY_FSINFO carrying its usage numbers rather than
// leaving `df` to two reads.
//
// A LIST, one record per live terminal. The three flags are not
// redundant with each other: QUERY_TTY_SUSPENDED is EITHER reason
// (keyboard_blocking_suspended()), and the two below it are which one --
// both can hold at once, which is why api/keyboard.h keeps them as
// separate flags rather than a boolean.
//
// **THE KEYBOARD FLAGS ARE THE MACHINE'S, NOT THE TERMINAL'S**, and
// they are repeated on every record rather than reported once. There is
// one physical keyboard and one compositor, so "who holds the keyboard"
// has a single answer; putting it on each row is what lets a reader look
// at one terminal and understand it without cross-referencing another.
// QUERY_TTY_BYPASS is the per-terminal half of the same story.
#include "query.h"
#include "tty.h"
#include "keyboard.h"
#include "win_server.h"
#include "string.h"
#include <stddef.h>
#include "initcall.h"

static int tty_query_count(void) {
    int n = 0;
    for (int i = 0; i < tty_count(); i++)
        if (tty_at(i)) n++;
    return n;
}

// The `index`-th LIVE terminal, which is not the same as the terminal at
// slot `index`: a destroyed pty leaves a hole, and a caller walking
// 0..count-1 must not see an empty record in the middle of the list.
static struct tty *nth_live(int index) {
    if (index < 0) return NULL;
    for (int i = 0; i < tty_count(); i++) {
        struct tty *t = tty_at(i);
        if (!t) continue;
        if (index-- == 0) return t;
    }
    return NULL;
}

static int tty_fill(int index, void *out) {
    struct tty *t = nth_live(index);
    if (!t) return 0;

    struct query_tty *q = out;
    k_memset(q, 0, sizeof *q);
    q->index           = (uint64_t)(unsigned)tty_index(t);
    q->owner_pid       = (uint64_t)(unsigned)tty_owner(t);
    q->foreground_pgid = (uint64_t)(unsigned)tty_fg_pgid(t);
    q->compositor_pid  = (uint64_t)(unsigned)win_server_compositor_pid();

    struct tty_termios tio;
    tty_get_termios(t, &tio);
    q->lflag = tio.lflag;
    k_strlcpy(q->driver, tty_driver_name(t), sizeof q->driver);

    if (keyboard_compositor_owns())     q->flags |= QUERY_TTY_COMPOSITOR;
    if (keyboard_console_claimed())     q->flags |= QUERY_TTY_CLAIMED;
    if (keyboard_blocking_suspended())  q->flags |= QUERY_TTY_SUSPENDED;
    if (tty_bypassed(t))                q->flags |= QUERY_TTY_BYPASS;
    return 1;
}

static const struct query_provider tty_provider = {
    .cls = QUERY_TTY,
    .name = "tty",
    .record_size = sizeof(struct query_tty),
    .flags = QUERY_F_LIST,
    .count = tty_query_count,
    .fill = tty_fill,
    // NO NAMED FIELDS, deliberately -- see api/query.h: a list is not
    // addressable as a single value, because an index baked into a name
    // means a different record a second later.
    .fields = NULL,
    .field_count = 0,
};

void tty_query_init(void) {
    query_register(&tty_provider);
}
INITCALL(tty_query_init, INIT_QUERY);
