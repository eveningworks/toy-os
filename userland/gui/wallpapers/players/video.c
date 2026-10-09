// A VIDEO AS THE DESKTOP BACKGROUND. The compositor runs this as its
// background client when the chosen live wallpaper is a video in
// LIVEWALL_ANIMATED_DIR (wm_background.c), its name as argv[1] --
// gif.c's arrangement, for .mpg and .avi.
//
// Silent and looping (lib/uvid_play.h's MUTE and LOOP), cropped to fill
// the screen as "Fill the screen" places a still (ui/uui_video.h's
// `cover`). The decoder runs HERE, in a process that can die, rather
// than in the compositor: it parses a file anyone can drop into the
// directory, for as long as the desktop is up.
#include "lib/uvid_play.h"
#include "lib/ulivewall.h"
#include "ui/uapp.h"
#include "ui/uui_video.h"

#include <stdio.h>
#include <string.h>

static struct uvid_play *g_play;
static struct uui_video g_vid;

// Drawn from on_draw, NOT as a routed widget: uapp clears a window that
// has widgets before painting them, and the compositor reads a
// background client's buffer whenever it composes -- so a frame caught
// mid-clear showed a band of the clear colour. Here every pixel is
// written once, by one copy of the scaled frame.
static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    uui_video_ops.draw(d->surface, &g_vid);
}

static int on_tick(struct uapp *a) {
    (void)a;
    return uvid_play_tick(g_play);
}

static void on_resize(struct uapp *a, int w, int h) {
    (void)a;
    uui_video_ops.set_geometry(&g_vid, 0, 0, w, h);
}

static void on_open(struct uapp *a) {
    on_resize(a, uapp_width(a), uapp_height(a));
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "";
    char path[128] = "";
    // A NAME, as the compositor passes it: never a path out of the directory.
    if (name[0] && !strchr(name, '/'))
        for (int i = 0; LIVEWALL_VIDEO_EXT[i]; i++) {
            snprintf(path, sizeof path, "%s/%s.%s", LIVEWALL_ANIMATED_DIR, name, LIVEWALL_VIDEO_EXT[i]);
            if (uvid_play_open(&g_play, path, UVID_PLAY_MUTE | UVID_PLAY_LOOP) == 0) break;
            g_play = 0;
        }
    if (!g_play) {
        fprintf(stderr, "video wallpaper: cannot play %s\n", name);
        return 1;
    }
    uui_video_init(&g_vid);
    g_vid.play = g_play;
    g_vid.cover = 1;
    g_vid.bg = 0x183c5a;              // desktop.c's plain colour, until the first frame
    const struct uvid_info *in = uvid_play_info(g_play);
    // A tick a little faster than the frames, so none waits a whole one.
    unsigned tick = in->fps_num ? (unsigned)(1000ull * in->fps_den / in->fps_num / 2) : 20;
    struct uapp_desc desc = {
        .title = "Video wallpaper",
        .app_id = "wallpaper",
        .flags = UAPP_RESIZABLE,
        .w = 320, .h = 180,
        .tick_ms = tick ? tick : 10,
        .on_draw = on_draw,
        .on_open = on_open,
        .on_resize = on_resize,
        .on_tick = on_tick,
    };
    int rc = uapp_run(&desc);
    uvid_play_close(g_play);
    uui_video_free(&g_vid);
    return rc;
}
