// A live memory readout in the top-right corner, for watching the two
// pools that actually run out on this machine while the system is doing
// something. Off unless `rammeter` is on the GRUB command line.
//
// **It reports TWO pools because they fail in completely different ways
// and one does not imply the other.** The physical frame allocator is
// what a leaking compositor or an unreaped process exhausts (window
// buffers are contiguous frames); the kernel heap is what fragments
// under kmalloc churn. A meter showing only one of them would routinely
// point at the wrong subsystem.
//
// It draws as an OVERLAY -- straight onto the visible framebuffer, on
// top of whatever frame was last finished, declaring no damage. That is
// deliberate and it is why this is debug-only: the compositor knows
// nothing about it, paints over it whenever it repaints that corner, and
// the meter simply reappears on its next tick. A control the user
// touches could not be built this way (see gfx.h's overlay section), but
// a passive readout that must work on the desktop AND at the physical
// console, without either one having to cooperate, can.
//
// The one-second cadence is not a rounding of "fast enough": redrawing
// costs framebuffer traffic, which on real hardware is the scarce thing
// this whole area is trying to conserve. A meter that measured the
// system by disturbing it would be its own worst observer.
#include "rammeter.h"
#include "gfx.h"
#include "pmm.h"
#include "heap.h"
#include "multiboot.h"
#include "string.h"
#include "kfmt.h"
#include "timer.h"
#include <stdint.h>

static int g_enabled;
static uint64_t g_last_tick;   // pit_ticks() when we last drew

// PIT runs at 100Hz (pit_init in kernel_main), so 100 ticks is a second.
#define REDRAW_TICKS 100

#define METER_BG    0x00101010u
#define METER_FG    0x00E0E0E0u
#define METER_DIM   0x00808080u
#define BAR_TRACK   0x00303030u
#define BAR_OK      0x0040C040u
#define BAR_WARN    0x00D0C040u
#define BAR_FULL    0x00D04040u
#define BAR_NEUTRAL 0x004080C0u

void rammeter_init(void) {
    const char *cmdline = multiboot_cmdline();
    g_enabled = (cmdline && k_strstr(cmdline, "rammeter")) ? 1 : 0;
}

int rammeter_enabled(void) { return g_enabled; }

void rammeter_set_enabled(int on) { g_enabled = on ? 1 : 0; }

// Fill colour for a usage bar. Three bands rather than a gradient
// because the point is to be readable at a glance -- and, on the machine
// this was written for, in a PHOTOGRAPH of the screen, since it has no
// serial port to report through.
static uint32_t bar_color(uint64_t used, uint64_t total) {
    if (total == 0) return BAR_TRACK;
    uint64_t pct = used * 100 / total;
    if (pct >= 90) return BAR_FULL;
    if (pct >= 70) return BAR_WARN;
    return BAR_OK;
}

// `warn` says whether a high percentage on this row is actually bad.
// It is not always: the heap's "total" is what it has CLAIMED from pmm
// so far, and it claims more on demand, so heap-used-against-claimed
// sits near full as a matter of course. Colouring that yellow would cry
// wolf on every boot and teach the reader to ignore the one row where
// the colour means something.
static void draw_row(int x, int y, int w, int ch, const char *label,
                     uint64_t used, uint64_t total, const char *unit, int warn) {
    char line[48];
    uint64_t pct = total ? (used * 100 / total) : 0;

    k_snprintf(line, sizeof(line), "%s %u/%u%s %u%%",
                label, (unsigned)used, (unsigned)total, unit, (unsigned)pct);
    gfx_overlay_string(x, y, line, METER_FG, METER_BG);

    // The bar sits under its own text and spans the panel width, so the
    // two readings line up vertically and can be compared at a glance.
    int bar_y = y + ch;
    int bar_h = 4;
    gfx_overlay_fill(x, bar_y, w, bar_h, BAR_TRACK);
    if (total > 0) {
        int filled = (int)((uint64_t)w * used / total);
        if (filled > w) filled = w;
        if (filled > 0) {
            gfx_overlay_fill(x, bar_y, filled, bar_h,
                             warn ? bar_color(used, total) : BAR_NEUTRAL);
        }
    }
}

void rammeter_tick(void) {
    if (!g_enabled) return;

    uint64_t now = pit_ticks();
    // Unsigned subtraction, so this stays correct across a wrap rather
    // than freezing the meter for the rest of the boot.
    if (g_last_tick != 0 && (now - g_last_tick) < REDRAW_TICKS) return;
    g_last_tick = now ? now : 1;

    rammeter_draw();
}

void rammeter_draw(void) {
    int sw = gfx_width(), sh = gfx_height();
    if (sw <= 0 || sh <= 0) return;

    int cw = gfx_char_w(), ch = gfx_char_h();

    // Font-derived, like everything else drawn here -- 26 columns is the
    // widest line this panel can produce ("heap 1234/5678KB 100%").
    int pad = cw;
    int w = 26 * cw + 2 * pad;
    int h = 2 * (ch + 4) + 2 * pad;
    if (w > sw || h > sh) return;      // nowhere sensible to put it

    int x = sw - w - pad;
    int y = pad;

    gfx_overlay_fill(x, y, w, h, METER_BG);
    // A one-pixel frame, so the panel stays legible over a light desktop
    // as well as a dark one.
    gfx_overlay_fill(x, y, w, 1, METER_DIM);
    gfx_overlay_fill(x, y + h - 1, w, 1, METER_DIM);
    gfx_overlay_fill(x, y, 1, h, METER_DIM);
    gfx_overlay_fill(x + w - 1, y, 1, h, METER_DIM);

    int ix = x + pad;
    int iy = y + pad;
    int iw = w - 2 * pad;

    // Frames reported in MiB: the raw frame count is a five-digit number
    // that says nothing at a glance, and MiB is the unit the rest of the
    // system (df, meminfo) already reports in.
    uint64_t total_f = pmm_total_frames();
    uint64_t free_f = pmm_free_frames();
    draw_row(ix, iy, iw, ch, "ram ",
             (total_f - free_f) * 4 / 1024, total_f * 4 / 1024, "MB", 1);

    // Not warn-coloured: see draw_row(). "heap 569/639KB" means the
    // allocator is about to claim another chunk from pmm, not that
    // anything is running out -- the row above is where exhaustion shows.
    draw_row(ix, iy + ch + 4, iw, ch, "heap",
             heap_used_bytes() / 1024, heap_total_bytes() / 1024, "KB", 0);
}
