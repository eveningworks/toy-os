#ifndef UUI_STAGES_H
#define UUI_STAGES_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_progress.h"

// A JOB'S STAGES AND HOW FAR IT HAS GOT: "Stage 2 of 3: ...", a bar, a
// count under it, and every stage listed -- done, running, or still to
// come -- each with a line of its own. Windows' chkdsk and its "Scanning
// drive" dialog are the shape; Disks' Check and Repair the first caller.
// Usually a uui_dialog's body (uui_dialog_set_body()).
//
// Display only (no `hit`). The CALLER writes the words: `names` once,
// then `line` and each stage's `detail` as the job reports, and moves
// it on with uui_stages_set().

#define UUI_STAGES_MAX 6

struct uui_stages {
    int x, y, w, h;
    int count;
    char names[UUI_STAGES_MAX][32];
    char detail[UUI_STAGES_MAX][48];  // beside each stage, right-aligned; "" for none
    char line[64];                    // under the bar, left: "18 of 28 groups"
    int current;                      // the running stage; `count` once all are done
    struct uui_progress bar;
    uint32_t bg;                      // the ground it sits on: a dialog's, at init
};

void uui_stages_init(struct uui_stages *s);
// Copies the names; clears every detail and starts at stage 0.
void uui_stages_set_names(struct uui_stages *s, const char *const *names, int n);
// `per_mille` 0..1000 of the current stage, or negative for BUSY -- a
// stage whose size is not known yet (uui_progress's marquee).
void uui_stages_set(struct uui_stages *s, int current, int per_mille);
// Advances a busy bar; 1 when it needs a repaint (uui_progress_tick()).
int  uui_stages_tick(struct uui_stages *s);

void uui_stages_natural_size(const struct uui_stages *s, int *out_w, int *out_h);
void uui_stages_draw(struct ugfx_surface *surf, const struct uui_stages *s);

struct uui_widget_ops;
extern const struct uui_widget_ops uui_stages_ops;

#endif
