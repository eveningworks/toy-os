// `lsdisplay` -- the screen and the monitor on it, over QUERY_DISPLAY.
//
// A sibling of lscpu/lspci/lsusb: the mode being shown, which driver
// shows it, and what the monitor said about itself in its EDID -- the
// panel's name and its native timing. Two numbers worth reading side
// by side: the mode on screen and the native one, because a driver that
// inherits the firmware's mode (the Intel one) can only tell you it is
// native by having read the EDID.
#include <stdint.h>
#include <stdio.h>
#include "rt/sys.h"
#include "lib/cmd.h"

#define USAGE "lsdisplay"

static const char *yn(uint64_t caps, uint64_t bit) { return (caps & bit) ? " yes" : " no"; }

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { cmd_usage(USAGE); return 1; }
    struct query_display d;
    if (sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d) < (int)sizeof d) {
        fprintf(stderr, "lsdisplay: no display fact from the kernel\n");
        return 1;
    }
    if (!d.driver[0]) {
        printf("no display driver claimed the hardware\n");
        return 0;
    }
    printf("Driver:        %s\n", d.driver);
    printf("Mode:          %llux%llu x%llu, pitch %llu, %llu scanout%s\n",
           (unsigned long long)d.width, (unsigned long long)d.height,
           (unsigned long long)d.bpp, (unsigned long long)d.pitch,
           (unsigned long long)d.scanouts, d.scanouts == 1 ? "" : "s");
    // DISPLAY_CAP_* bit order, kernel/display.h.
    printf("Capabilities:  flush%s cursor%s fill%s copy%s modeset%s backlight%s flip%s scaling%s\n",
           yn(d.caps, 1 << 0), yn(d.caps, 1 << 1), yn(d.caps, 1 << 2), yn(d.caps, 1 << 3),
           yn(d.caps, 1 << 4), yn(d.caps, 1 << 5), yn(d.caps, 1 << 6), yn(d.caps, 1 << 7));
    if (!(d.flags & QUERY_DISPLAY_F_EDID)) {
        printf("Monitor:       no EDID (the driver read none)\n");
        return 0;
    }
    printf("Monitor:       %s %s%s\n", d.vendor, d.panel[0] ? d.panel : "(unnamed)",
           (d.flags & QUERY_DISPLAY_F_DIGITAL) ? ", digital" : ", analog");
    if (d.native_width) {
        printf("Native timing: %llux%llu @ %llu.%02llu Hz, %llu.%02llu MHz pixel clock\n",
               (unsigned long long)d.native_width, (unsigned long long)d.native_height,
               (unsigned long long)(d.refresh_mhz / 1000), (unsigned long long)((d.refresh_mhz % 1000) / 10),
               (unsigned long long)(d.pixel_khz / 1000), (unsigned long long)((d.pixel_khz % 1000) / 10));
        if (d.width_mm)
            printf("Image size:    %llu x %llu mm\n",
                   (unsigned long long)d.width_mm, (unsigned long long)d.height_mm);
        printf("On screen:     %s\n",
               d.width == d.native_width && d.height == d.native_height ? "the native mode"
                                                                          : "not the native mode");
    }
    return 0;
}
