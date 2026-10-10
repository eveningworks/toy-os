#ifndef UUI_SNDTEST_H
#define UUI_SNDTEST_H

// uui_sndtest -- a SPEAKER TEST as a panel: a button per side, Both
// between them, a level meter beside each side, and a line saying what
// plays. A button starts a chime on that side, once a second, until it is
// pressed again. GNOME's speaker test and Windows' Test, in one card;
// System Settings shows it for the card in use (Sound > Output).
//
// **IT HOLDS THE SINK ONLY WHILE A TEST RUNS** (usnd_init() on the first
// press, usnd_shutdown() on stop): without soundd the stream is
// exclusive, and an open Settings window holding it would silence the
// Audio Player.
//
// **THE METERS SHOW WHAT IS SENT, NOT WHAT THE CARD PLAYS** -- the peak of
// the chime's own samples where the clock says playback has reached,
// before the master volume. Nothing reads a level back from the card.
//
// A PANEL OF STANDARD CONTROLS, like uui_sndformat: the app embeds
// uui_sndtest_item(), adds the focusables to its ring, offers each
// on_action code to uui_sndtest_on_action() first, and calls
// uui_sndtest_tick() often while uui_sndtest_playing() -- a meter at
// the app's idle tick would step twice a second.

#include <stdint.h>
#include "ui/uui_widget.h"
#include "ui/uui_button.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_focus.h"
#include "lib/usnd.h"

// The codes it reports under: code_base .. code_base + UUI_SNDTEST_IDS - 1.
#define UUI_SNDTEST_IDS 3
enum { UUI_SNDTEST_LEFT = 0, UUI_SNDTEST_BOTH = 1, UUI_SNDTEST_RIGHT = 2 };

#define UUI_SNDTEST_SEGS 8       // a meter's segments, 6 dB each
#define UUI_SNDTEST_SOUND "/usr/share/sounds/chime.wav"

struct uui_sndtest {
    int code_base;
    int playing;                 // UUI_SNDTEST_*, or -1
    int sink, have_clip;         // usnd_init() succeeded; the chime is loaded
    struct usnd_clip clip;
    usnd_voice_t voice;
    uint64_t started_ms;         // this chime's start
    int level[2];                // segments lit, left and right
    char status[96];

    struct uui_button btn[UUI_SNDTEST_IDS];
    struct uui_custom meter[2];
    struct uui_label status_l;
    struct uui_item row_it[5], col_it[2];
    struct uui_layout row, col;
};

void uui_sndtest_init(struct uui_sndtest *t, int code_base);

// The panel, for the app's layout.
struct uui_item uui_sndtest_item(struct uui_sndtest *t);

// Its buttons, in tab order, appended to `out`; returns how many.
int uui_sndtest_focusables(struct uui_sndtest *t, struct uui_focusable *out, int max);

// The app's on_action, offered first. 1 when `code` was the panel's: a
// test started, moved to another side or stopped -- redraw.
int uui_sndtest_on_action(struct uui_sndtest *t, int code);

// 1 while a test runs.
int uui_sndtest_playing(const struct uui_sndtest *t);

// Moves the meters and starts the next chime when one is due. 1 when
// something changed -- redraw.
int uui_sndtest_tick(struct uui_sndtest *t);

// Stops a test and gives the card back; harmless when none runs.
void uui_sndtest_stop(struct uui_sndtest *t);

#endif
