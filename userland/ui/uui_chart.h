#ifndef UUI_CHART_H
#define UUI_CHART_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// chart -- a value over TIME, which is the one thing uui_meter cannot
// show.
//
// A meter answers "how busy is it now"; this answers "and was it busy a
// minute ago", which is a different question and the reason Task
// Manager, GNOME System Monitor and KSysGuard all carry both.
//
// **RAW VALUES, NOT PERCENTAGES, AND A SCALE THAT SAYS WHICH.** A
// percentage-only chart cannot plot a RATE -- bytes per second has no
// natural 100 -- and rescaling a percentage series against a moving
// peak makes old samples lie, because they were scaled against an older
// one. So samples are stored as given and `scale_max` decides how they
// are read: 100 for a percentage, 0 to AUTOSCALE against the largest
// sample in view, which is what GNOME's network graph does.
//
// **THE RING IS INSIDE THE WIDGET.** Toykit has no allocator, and a
// caller-supplied array is one more thing for every caller to size
// wrongly.
//
// NEWEST ON THE RIGHT, which is every system monitor's direction and
// the one that keeps the eye where new data arrives.

#define UUI_CHART_MAX 180
#define UUI_CHART_MARKS 8
#define UUI_CHART_SERIES 4

// A labelled moment on the time axis -- "SEQ read" starting here. Its
// label sits in a strip ABOVE the plot, never under the trace.
struct uui_chart_mark { int at; const char *label; };

struct uui_chart {
    int x, y, w, h;
    const char *label;          // drawn top-left, or NULL for none
    // **THE READING, IN THE CALLER'S WORDS, drawn top-right.** Only the
    // caller knows what a sample MEANS -- "42%" for a processor,
    // "1.2 of 1.9 GiB" for memory, "3.4 MiB/s" for a disk. Formatting
    // it here would mean the widget choosing units for a number whose
    // meaning it does not have. Caller-owned and uncopied, like `label`.
    const char *value;

    uint32_t samples[UUI_CHART_MAX];
    // A SECOND SERIES THAT IS PART OF THE FIRST, not beside it: kernel
    // time inside total CPU time, which is the decomposition Windows
    // draws as a darker band. Drawn from the baseline UP TO its own
    // height, over the total, so the eye reads "this much of that".
    uint32_t parts[UUI_CHART_MAX];
    int has_parts;

    int count;                  // valid samples, capped at UUI_CHART_MAX
    int head;                   // where the next sample goes

    // 100 for a percentage; 0 autoscales to the largest sample in view.
    uint32_t scale_max;
    // Milliseconds between samples, for the span caption. 0 draws none
    // -- a width that means nothing is better left unlabelled than
    // labelled wrongly.
    int sample_ms;
    // Which column the pointer is over, counted from the OLDEST drawn,
    // or -1. The widget marks it; the APP formats what it says, because
    // the app owns the units.
    int hover;
    // **A SPARKLINE IS THE TRACE AND NOTHING ELSE** -- no gridlines, no
    // frame. At three rows tall the furniture is most of the ink, and a
    // small boxed grid reads as an empty table rather than as a series.
    // Tufte's point, and what every sparkline in a dashboard does.
    int compact;

    // UUI_COLOR_UNSET lets the theme answer at DRAW time -- see utheme.h
    // on why a widget must not resolve its colours when it is built.
    uint32_t bg, grid, line, fill, part;

    // **A RUN, NOT A MONITOR** (uui_chart_set_fit()): the whole history
    // spread across the width as a line, oldest at the left -- GNOME
    // Disks' benchmark graph. It never scrolls anything off: when full it
    // halves its resolution, averaging pairs, and from then on averages
    // `stride` pushes into each sample, so the time axis stays uniform.
    int fit;
    int stride, pend_n;
    uint64_t pend_sum;
    // Which series each sample belongs to, drawn in series_col[] and
    // never joined across a change -- a read and a write in one run.
    uint8_t series[UUI_CHART_MAX];
    uint32_t series_col[UUI_CHART_SERIES];
    int cur_series;
    struct uui_chart_mark marks[UUI_CHART_MARKS];
    int mark_n;
    // **A LABELLED VALUE AXIS** (uui_chart_set_axis(), FIT MODE ONLY --
    // a scrolling chart's gutter would change how many samples show): a left gutter of
    // tick values, the top rounded up to a 1-2-5 step so every gridline
    // sits on a printed number. A sample divided by `axis_div` is the
    // printed value -- 1000 for a milli-unit series. NULL draws none.
    const char *axis_unit;
    uint32_t axis_div;
};

void uui_chart_init(struct uui_chart *c, const char *label);

// 100 for a percentage, 0 to autoscale against the largest sample in
// view. Anything else fixes the top of the scale.
void uui_chart_set_scale(struct uui_chart *c, uint32_t max);

// How far apart samples are, so the span caption can say what the width
// covers. 0 (the default) draws no caption.
void uui_chart_set_interval(struct uui_chart *c, int ms);

void uui_chart_push(struct uui_chart *c, uint32_t value);

// The run mode above; clears the history. Then: which series the next
// pushes belong to (0..UUI_CHART_SERIES-1), and a labelled mark at the
// next sample. `label` is not copied.
void uui_chart_set_fit(struct uui_chart *c, int on);
void uui_chart_set_series(struct uui_chart *c, int series);
void uui_chart_add_mark(struct uui_chart *c, const char *label);
void uui_chart_clear(struct uui_chart *c);
// The value axis above: `unit` captions it ("MB/s"; not copied), a
// sample / `div` is what a tick prints. NULL turns it off.
void uui_chart_set_axis(struct uui_chart *c, const char *unit, uint32_t div);

// One sample where `part` is a component of `total` -- kernel time
// within CPU time. `part` is clamped to `total`: a component larger
// than its whole is a caller bug, and drawing it would paint outside
// the series it belongs to.
void uui_chart_push_split(struct uui_chart *c, uint32_t total, uint32_t part);

// The reading to print top-right, or NULL. NOT copied -- point it at
// storage that outlives the frame.
void uui_chart_set_value(struct uui_chart *c, const char *value);

uint32_t uui_chart_last(const struct uui_chart *c);

// Which sample the pointer is over, as an index into the drawn series
// (0 = oldest drawn), or -1. With uui_chart_sample() this is what lets
// an app print the hovered reading in its own units.
int uui_chart_hover_index(const struct uui_chart *c);
uint32_t uui_chart_sample(const struct uui_chart *c, int i);
// How many samples are actually DRAWN -- min(count, width).
int uui_chart_drawn(const struct uui_chart *c);
// The sample `back` pushes ago (0 = the newest), or 0 past the history.
// Independent of the chart's WIDTH, unlike uui_chart_sample(): a chart
// that is never laid out -- a trace drawn somewhere else, like Task
// Manager's device list -- has drawn nothing and still has its history.
uint32_t uui_chart_recent(const struct uui_chart *c, int back);

void uui_chart_natural_size(const struct uui_chart *c, int *out_w, int *out_h);
void uui_chart_set_geometry(struct uui_chart *c, int x, int y, int w, int h);
void uui_chart_draw(struct ugfx_surface *s, const struct uui_chart *c);

extern const struct uui_widget_ops uui_chart_ops;

#endif
