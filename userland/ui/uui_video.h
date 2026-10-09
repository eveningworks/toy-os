#ifndef UUI_VIDEO_H
#define UUI_VIDEO_H

// uui_video -- a playing video's picture: the frame lib/uvid_play.h says
// is due, scaled into the widget's rect, letterboxed (`fit`) or cropped
// to fill it (`cover`, a wallpaper's placement). The Video Player's
// stage and the video wallpaper both draw one.
//
// **A FRAME IS SCALED ONCE.** The result is kept and blitted again for
// every repaint until the player shows another frame -- a paused video
// repainted under a menu costs a copy, not a rescale, and comes out
// pixel-identical, which a scaler that changes quality between repaints
// (below) would not.
//
// IT DRAWS, IT DOES NOT PLAY: the app owns the uvid_play, ticks it and
// repaints when the tick says the frame changed; the widget only reads
// uvid_play_frame(). So the same player can be shown in two places (a
// preview beside a full view) and a widget never outlives a decision.
//
// A CLICK on the picture parks UUI_VIDEO_CLICK for uui_video_take(), a
// second within the double-click time UUI_VIDEO_DOUBLE -- play/pause and
// full screen in every desktop player. Armed on press, committed on a
// release over the picture.
//
// **SCALING ADAPTS TO THE MACHINE**: bilinear while its AVERAGE draw
// fits in a 60 Hz frame, nearest-neighbour when it does not -- what a
// slow CPU drawing a full screen of video needs, decided by measuring
// rather than by a size threshold that is wrong on every machine but
// one. An average, so one slow frame (the first, a cold cache) decides
// nothing; and a fast-mode widget tries bilinear again now and then, so
// a busy moment is not a verdict for the rest of the film.
#include <stdint.h>
#include "ui/uui_widget.h"

struct uvid_play;

enum { UUI_VIDEO_NONE = 0, UUI_VIDEO_CLICK, UUI_VIDEO_DOUBLE };

struct uui_video {
    int x, y, w, h;
    struct uvid_play *play;     // the app's; NULL draws only the ground
    uint32_t bg;                // the bars around a picture of another shape
    int cover;                  // fill the rect, cropping, rather than fit
    // OWNED
    int px, py, pw, ph;         // where the picture landed last draw
    int armed;
    int committed;
    uint64_t last_click_ns;
    int fast;                   // drawing nearest-neighbour now
    uint32_t smooth_us;         // a running average of bilinear draws
    uint32_t draws;             // since the last bilinear trial
    int last_w, last_h;         // the size those numbers are for
    uint32_t *cache;            // the scaled frame, last_w x last_h
    uint32_t cache_serial;      // which frame it is (uvid_play_serial())
    int cache_sx, cache_sy;     // ...cut from where
};

void uui_video_init(struct uui_video *v);
// Releases the scaled frame the widget keeps. The widget stays usable.
void uui_video_free(struct uui_video *v);
// The click a release completed (UUI_VIDEO_*), and clears it.
int  uui_video_take(struct uui_video *v);

extern const struct uui_widget_ops uui_video_ops;

#endif
