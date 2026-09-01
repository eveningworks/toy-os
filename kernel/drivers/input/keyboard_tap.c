// The keyboard tap and its query provider. See kernel/keyboard_tap.h for
// the design and for why it records unconditionally.
//
// TWO RECORD SHAPES ON PURPOSE. What the ring holds is packed (20 bytes)
// and what QUERY_KBDTAP hands out is not (every named-field-capable
// value in that registry is 64 bits, api/query.h). Storing the wide form
// would put 18 KB of mostly padding in the kernel's BSS for a facility
// nobody runs most days; expanding one record at read time costs a
// function that runs once per line printed. The packed form is private
// to this file, so it can change without touching the ABI.
#include "keyboard_tap.h"
#include "query.h"
#include "timer.h"
#include "string.h"
#include <stddef.h>

// driver-none: a diagnostic tap on the key path

// 256 events -- about 128 keystrokes, since a key that types reports
// both edges. Enough that the command you TYPE to read the log does not
// evict what you were looking for, which is the failure a smaller ring
// would have (`kbd --last 20` is seven keys, i.e. fourteen records,
// before it prints anything).
//
// A power of two so the index arithmetic is a mask. 5 KB of BSS at 20
// bytes a record; that number is the honest cost of this file.
#define TAP_MAX 256

struct tap_rec {
    uint32_t seq;         // 1-based; 0 marks a slot never written
    uint32_t ticks;       // pit_ticks() truncated -- see the note in fill()
    uint16_t wire;        // PS/2 byte, | TAP_WIRE_EXT; 0 = not from PS/2
    uint16_t keycode;
    uint16_t produced[QUERY_KBDTAP_PRODUCED_MAX];
    uint8_t  mods;
    uint8_t  down;
    uint8_t  nproduced;
    uint8_t  reserved;
};

// The 0xE0 prefix flag, kept OUT of the byte's own eight bits so a wire
// value stays comparable against a scancode constant.
#define TAP_WIRE_EXT 0x100

static struct tap_rec g_ring[TAP_MAX];
static uint32_t g_seq;      // the last sequence number handed out
static uint32_t g_written;  // records retained, saturating at TAP_MAX

// OFF UNTIL ASKED. See keyboard_tap.h: this is a privacy default, not a
// performance one, and `kernel.kbdtap` is the switch.
static int g_enabled;

// Where the record kbdtap_key() opened most recently lives, so
// kbdtap_produced() can reach it without every one of ring_push()'s ~20
// call sites having to carry it. The same shape as keyboard.c's
// `emitting_keycode`, and safe for the same reason: both are set and
// read inside one non-preemptible interrupt handler.
static struct tap_rec *g_open;

int kbdtap_enabled(void) { return g_enabled; }

void kbdtap_set_enabled(int on) {
    on = on ? 1 : 0;

    // WIPED ON EVERY TRANSITION, in both directions. Off has to mean
    // "there are no keystrokes in here", or the switch is decorative;
    // on has to start from a clean ring, or a session opens with
    // whatever the previous one caught. `g_seq` deliberately survives --
    // it is a counter, not data, and restarting it would let a reader
    // see a sequence number it had already seen.
    k_memset(g_ring, 0, sizeof g_ring);
    g_written = 0;
    g_open = 0;
    g_enabled = on;
}

void kbdtap_key(uint16_t wire, int extended, uint16_t keycode, int down,
                uint8_t mods) {
    if (!g_enabled) return;
    struct tap_rec *r = &g_ring[g_seq & (TAP_MAX - 1)];
    r->seq       = ++g_seq;
    r->ticks     = (uint32_t)pit_ticks();
    r->wire      = (uint16_t)(wire ? (wire | (extended ? TAP_WIRE_EXT : 0)) : 0);
    r->keycode   = keycode;
    r->mods      = mods;
    r->down      = (uint8_t)(down ? 1 : 0);
    r->nproduced = 0;
    r->produced[0] = 0;
    r->produced[1] = 0;
    r->reserved  = 0;
    if (g_written < TAP_MAX) g_written++;
    g_open = r;
}

void kbdtap_produced(uint16_t code) {
    // No `g_enabled` test: g_open is only ever set by kbdtap_key(),
    // which already refuses while off, and clearing it is part of the
    // wipe. One gate, in the one place a record is created.
    if (!g_open) return;
    if (g_open->nproduced >= QUERY_KBDTAP_PRODUCED_MAX) return;
    g_open->produced[g_open->nproduced++] = code;
}

// --- the provider ----------------------------------------------------

static int kbdtap_count(void) { return (int)g_written; }

// Record `index`, OLDEST RETAINED FIRST. The ring can move between two
// of these calls -- it is fed by an interrupt -- so a reader that walks
// 0..count-1 may see a record twice or miss one under a burst. That is
// what `seq` is for: a repeat is recognisable and a gap is countable,
// which is strictly better than an interface that pretended to be
// atomic. Draining instead would make the log readable exactly once and
// break the second reader.
static int kbdtap_fill(int index, void *out) {
    if (index < 0 || (uint32_t)index >= g_written) return 0;

    // The oldest retained record is `g_written` back from the newest.
    uint32_t slot = (g_seq - g_written + (uint32_t)index) & (TAP_MAX - 1);
    const struct tap_rec *r = &g_ring[slot];

    struct query_kbdtap *q = out;
    k_memset(q, 0, sizeof *q);
    q->seq     = r->seq;
    // Widened here rather than stored wide. It is a TICK COUNT, not a
    // time: 10ms steps (PIT_HZ), truncated to 32 bits, so it wraps after
    // ~497 days of uptime. A caller uses it for the INTERVAL between two
    // adjacent events -- telling autorepeat from a second press -- which
    // a wrap breaks for exactly one pair of lines.
    q->ticks   = r->ticks;
    q->wire    = r->wire & 0xFF;
    q->keycode = r->keycode;
    q->mods    = r->mods;
    if (r->wire & TAP_WIRE_EXT) q->flags |= QUERY_KBDTAP_EXTENDED;
    if (r->down)                q->flags |= QUERY_KBDTAP_DOWN;
    q->produced = r->nproduced;
    for (int i = 0; i < QUERY_KBDTAP_PRODUCED_MAX; i++)
        q->produced_code[i] = r->produced[i];
    return 1;
}

static const struct query_provider kbdtap_provider = {
    .cls = QUERY_KBDTAP,
    .name = "kbdtap",
    .record_size = sizeof(struct query_kbdtap),
    .flags = QUERY_F_LIST,
    .count = kbdtap_count,
    .fill = kbdtap_fill,
    // NO NAMED FIELDS -- a list is not addressable as a single value
    // (api/query.h), and `kbdtap.keycode` would name a different
    // keypress on every read.
    .fields = NULL,
    .field_count = 0,
};

void kbdtap_query_init(void) {
    query_register(&kbdtap_provider);
}
