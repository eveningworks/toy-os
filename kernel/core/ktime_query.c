// The clock, as a queryable fact. See abi/query_abi.h's QUERY_CLOCK for
// why ring 3 needs this at all: SYS_GETTIME's answer is local civil
// time, so nothing in ring 3 could read UTC before it.
#include "query.h"
#include "ktime.h"
#include "clocksource.h"
#include "clockevent.h"
#include "string.h"
#include "initcall.h"
#include <stddef.h>

static int clock_count(void) { return 1; } // scalar

static int clock_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_clock *c = out;
    k_memset(c, 0, sizeof *c);
    // ONE READ, then both units derived from it. Calling ktime_now_ns()
    // and ktime_now_sec() separately would let a second boundary fall
    // between them, and a record whose two halves disagree is worse than
    // either alone.
    uint64_t ns = ktime_now_ns();
    c->utc_ns = ns;
    c->utc = ns / 1000000000ull;
    c->monotonic_ns = clocksource_now_ns();
    c->last_step = (uint64_t)ktime_last_step();
    c->steps = ktime_step_count();
    const struct clocksource *cs = clocksource_current();
    k_strlcpy(c->source, cs ? cs->name : "none", sizeof c->source);
    struct clockevent_stats st;
    clockevent_get_stats(&st);
    c->tick_hz = clockevent_hz();
    c->tick_mode = st.oneshot ? (st.nohz ? 2 : 1) : 0;
    c->tick_events = st.events;
    c->tick_ticks = st.ticks;
    c->tick_idle_stops = st.idle_stops;
    c->tick_stopped_ns = st.stopped_ns;
    return 1;
}

static const struct query_field clock_fields[] = {
    QUERY_FIELD(struct query_clock, utc, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, utc_ns, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, monotonic_ns, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, last_step, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, steps, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, tick_hz, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, tick_mode, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, tick_events, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, tick_ticks, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, tick_idle_stops, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_clock, tick_stopped_ns, QUERY_TYPE_U64),
};

static const struct query_provider clock_provider = {
    .cls = QUERY_CLOCK,
    .name = "clock",
    .record_size = sizeof(struct query_clock),
    .flags = 0, // scalar
    .count = clock_count,
    .fill = clock_fill,
    .fields = clock_fields,
    .field_count = sizeof clock_fields / sizeof clock_fields[0],
};

static void ktime_query_init(void) { query_register(&clock_provider); }
INITCALL(ktime_query_init, INIT_QUERY);
