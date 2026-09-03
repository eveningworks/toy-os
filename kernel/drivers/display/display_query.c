// The screen, as a queryable FACT: the mode on it and the monitor's
// EDID. SCALAR, one record, everything re-read on each fill except the
// EDID, which was read once at probe (display.c) and cannot change.
#include "query.h"
#include "display.h"
#include "edid.h"
#include "string.h"
#include "initcall.h"

// driver-none: a query provider over the display class

static int display_q_count(void) { return 1; }

static void copy_str(char *dst, int cap, const char *src) {
    int n = 0;
    while (src && src[n] && n < cap - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

static int display_q_fill(int index, void *out) {
    if (index != 0) return 0;
    struct query_display *q = out;
    k_memset(q, 0, sizeof *q);
    const struct display_driver *d = display_active();
    if (!d) return 1;   // a machine with no display still has the fact: all zero
    struct display_surface s;
    display_get_surface(&s);
    q->width = s.width; q->height = s.height; q->bpp = s.bpp; q->pitch = s.pitch;
    q->caps = d->caps;
    q->scanouts = (uint64_t)display_scanout_count();
    copy_str(q->driver, sizeof q->driver, d->name);
    const struct display_edid *e = display_edid();
    if (e) {
        q->flags |= QUERY_DISPLAY_F_EDID;
        if (e->digital) q->flags |= QUERY_DISPLAY_F_DIGITAL;
        copy_str(q->panel, sizeof q->panel, e->name);
        copy_str(q->vendor, sizeof q->vendor, e->vendor);
        if (e->timing_count > 0) {
            const struct edid_timing *t = &e->timing[0];
            q->native_width = t->hactive;
            q->native_height = t->vactive;
            q->pixel_khz = t->pixel_khz;
            q->refresh_mhz = edid_refresh_mhz(t);
            q->width_mm = t->width_mm;
            q->height_mm = t->height_mm;
        }
    }
    return 1;
}

static const struct query_field display_q_fields[] = {
    QUERY_FIELD(struct query_display, width,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, height,        QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, bpp,           QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, pitch,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, caps,          QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, scanouts,      QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, flags,         QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, native_width,  QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, native_height, QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, pixel_khz,     QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, refresh_mhz,   QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, width_mm,      QUERY_TYPE_U64),
    QUERY_FIELD(struct query_display, height_mm,     QUERY_TYPE_U64),
};

static const struct query_provider display_provider = {
    .cls = QUERY_DISPLAY,
    .name = "display",
    .record_size = sizeof(struct query_display),
    .flags = 0, // scalar
    .count = display_q_count,
    .fill = display_q_fill,
    .fields = display_q_fields,
    .field_count = sizeof display_q_fields / sizeof display_q_fields[0],
};

static void display_query_init(void) { query_register(&display_provider); }
INITCALL(display_query_init, INIT_QUERY);
