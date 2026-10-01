#ifndef UUI_TRANSPORT_H
#define UUI_TRANSPORT_H

// uui_transport -- previous, play/pause, next: a media transport, the
// round play button in the middle (Windows 11 Media Player's, Amberol's).
// The Audio Player's stage and the Image Viewer's slideshow pill both
// draw one.
//
// It CLICKS LIKE A BUTTON -- armed on press, committed on a release over
// the same part -- and parks the part it committed for the app to take
// (uui_transport_take()), the shape uui_toolbar_take_code() has. The
// glyphs are drawn, not spelled: the font has no transport symbols.
//
// `dark` is for a dark or ambient ground (light glyphs, a light play
// disc); otherwise the play disc is the accent. Hover washes the
// ground under a side button, whatever colour that ground is.
#include <stdint.h>
#include "ui/uui_widget.h"

enum { UUI_TRANSPORT_NONE = 0, UUI_TRANSPORT_PREV, UUI_TRANSPORT_PLAY, UUI_TRANSPORT_NEXT };

struct uui_transport {
    int x, y, w, h;
    int playing;                 // the middle shows pause bars, not a triangle
    int no_prev, no_next;        // that side is greyed and does not click
    int disabled;                // nothing clicks (nothing to play)
    int dark;
    int hot;                     // OWNED: the part under the pointer
    int armed;                   // OWNED: the part a press landed on
    int committed;               // OWNED: the part a click completed, until taken
};

void uui_transport_init(struct uui_transport *t);
// The part a click completed (UUI_TRANSPORT_*), and clears it.
int  uui_transport_take(struct uui_transport *t);
// Where a part is, content-relative -- what `describe` reports.
void uui_transport_part(const struct uui_transport *t, int part,
                        int *x, int *y, int *w, int *h);

extern const struct uui_widget_ops uui_transport_ops;

#endif
