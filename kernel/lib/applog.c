// The application log ring -- see api/applog.h for what it is and why
// it is not klog.
#include "applog.h"
#include "string.h"
#include "timer.h"

// 64 records is ~14 KiB of .bss and several seconds of ordinary service
// chatter at the rate this system produces it. It is a BUFFER between a
// writer and logd's one-second poll, not a history: the history is the
// file logd keeps, and sizing this to be one would be solving the
// problem twice in the place that cannot win.
#define APPLOG_RECS 64

static struct applog_rec g_rec[APPLOG_RECS];
static uint64_t g_next = 1;   // sequence of the NEXT record written

void applog_write(const char *tag, const char *text, uint32_t len) {
    if (!text || !len) return;
    // A trailing newline is the writer's line terminator, not part of
    // what it said. Stripped here so every record is a bare line and
    // whoever renders it decides the separator -- but REMEMBERED, since
    // it is what tells a reader the line is finished.
    int eol = 0;
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) { len--; eol = 1; }
    // A write of nothing but a newline still ENDS a line, and dropping it
    // would strand whatever fragments came before it. Only the SAME
    // writer's fragment may be closed this way: two processes interleave
    // freely here, so marking whatever happens to be last would hand one
    // program's terminator to another's half-line.
    if (!len) {
        if (eol && g_next > 1) {
            struct applog_rec *prev = &g_rec[(g_next - 1) % APPLOG_RECS];
            if (!k_strcmp(prev->tag, (tag && tag[0]) ? tag : "?")) prev->eol = 1;
        }
        return;
    }
    if (len > APPLOG_TEXT_MAX - 1) len = APPLOG_TEXT_MAX - 1;

    struct applog_rec *r = &g_rec[g_next % APPLOG_RECS];
    r->seq = g_next;
    r->cs = pit_ticks();
    k_strlcpy(r->tag, (tag && tag[0]) ? tag : "?", APPLOG_TAG_MAX);
    k_memcpy(r->text, text, len);
    r->text[len] = '\0';
    r->len = (uint16_t)len;
    r->eol = (uint8_t)eol;
    g_next++;
}

uint64_t applog_total(void) { return g_next - 1; }

uint64_t applog_oldest(void) {
    return g_next > APPLOG_RECS ? g_next - APPLOG_RECS : 1;
}

int applog_get(uint64_t seq, struct applog_rec *out) {
    if (!out || !seq || seq >= g_next) return 0;
    if (seq < applog_oldest()) return 0;         // aged out
    const struct applog_rec *r = &g_rec[seq % APPLOG_RECS];
    // The slot could have been overwritten between the bound check and
    // here on a preemptible kernel, so the SEQUENCE is checked against
    // the record itself -- the bound alone would hand back a newer
    // record wearing an older sequence number, which is worse than
    // reporting the gap.
    if (r->seq != seq) return 0;
    *out = *r;
    return 1;
}
