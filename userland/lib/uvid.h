#ifndef ULIB_UVID_H
#define ULIB_UVID_H

// uvid -- decoding video files into frames, in RING 3.
//
// The video half of what lib/usnd.h is for sound and lib/uimg.h for
// pictures, and in the same place for the same reason: a parser of files
// anyone can download belongs in the process that opened them. FFmpeg's
// libavformat/libavcodec, GStreamer and Media Foundation are all
// user-mode; so is this.
//
// TWO TABLES, the shape both of those frameworks have:
//
//   CONTAINERS -- a file's layout: where each stream's packets are and
//   when each is shown (AVI; the MPEG-1 program stream). A row in
//   uvid.c, reading through uvid_internal.h's `struct uvid_src`.
//
//   CODECS -- what turns one stream's packets into pictures (Motion
//   JPEG, through lib/uimg.h's decoder; MPEG-1 video).
//
// **THE SOUND IS NOT HERE.** A video's audio track is a row in usnd's
// codec table (usnd_vid.c), so `usnd_play("clip.mpg")` plays it and
// usnd_position() is the clock a player shows frames against -- the
// AUDIO-MASTER sync every player uses (mpv, GStreamer's audio sink, Media
// Foundation's presentation clock), because a dropped or repeated video
// frame is invisible and a gap in the sound is not. The two halves open
// the file separately and each skips the other's packets.
//
// ERRORS are negative errnos with lib/uimg.h's three-way split: -EINVAL
// a broken file, -ENOTSUP a good file this build will not play (H.264 in
// an AVI), -ENOMEM. uvid_last_error() has the sentence.
#include <stddef.h>
#include <stdint.h>

struct uimg;

// What a frame's pixels are. YUV is what both codecs produce natively
// and what a scaler wants to read; the conversion to RGB happens once,
// at the destination's size, in uvid_frame_draw().
enum { UVID_YUV420 = 1, UVID_ARGB = 2 };

struct uvid_frame {
    int w, h;               // the displayed size
    int fmt;
    // UVID_YUV420: chroma planes are ((w+1)/2) x ((h+1)/2). `full_range`
    // is JFIF's 0..255 (Motion JPEG); MPEG video is 16..235.
    const uint8_t *y, *cb, *cr;
    int y_stride, c_stride;
    int full_range;
    // UVID_ARGB: 0xAARRGGBB, lib/uimg.h's layout.
    const uint32_t *argb;
    int argb_stride;
    int64_t pts_ms;         // when it is shown, from the start of the file
    char type;              // 'I', 'P', 'B' -- what the codec decoded it as
};

struct uvid_info {
    int w, h;
    uint32_t fps_num, fps_den;  // frames per second as a ratio; 0/0 unknown
    uint32_t ms;                // duration, 0 when unknown
    uint32_t frames;            // 0 when unknown
    uint32_t kbps;              // the whole file's, from size and duration
    const char *container;      // "avi", "mpeg-ps"
    const char *codec;          // "mjpeg", "mpeg1"
    char detail[48];            // "Motion JPEG", "MPEG-1, I/P/B"
    int has_audio;
    char audio[48];             // "PCM 16-bit, 22.05 kHz mono"; "" for none
};

struct uvid;

const char *uvid_last_error(void);

// Does a container claim these bytes? 16 bytes is enough for every probe.
int uvid_probe(const void *data, size_t n);

// Header only, from a path. Cheap: a container's index, never a frame.
int uvid_load_info(const char *path, struct uvid_info *out);

int  uvid_open(const char *path, struct uvid **out);
const struct uvid_info *uvid_info(const struct uvid *v);
void uvid_close(struct uvid *v);

// The next frame in DISPLAY order: 1 and `*out` set, 0 at the end, or a
// negative errno. The frame is the decoder's own memory and stays valid
// until the next uvid_next(), uvid_seek() or uvid_close().
int uvid_next(struct uvid *v, const struct uvid_frame **out);

// Repositions so the next frame is the one showing at `ms`: the decoder
// restarts at the keyframe before it and DISCARDS forward to it -- what
// "exact" seeking is in mpv. UVID_SEEK_KEY stops at the keyframe instead,
// which is what a seek bar being dragged wants: fast, approximate.
#define UVID_SEEK_EXACT 0
#define UVID_SEEK_KEY   1
int uvid_seek(struct uvid *v, uint32_t ms, int mode);

// --- drawing a frame ---------------------------------------------------
//
// Paints the source rectangle (sx, sy, sw, sh) of `f` into a dw x dh
// block of 0x00RRGGBB pixels at `dst`, `dst_stride` pixels per row --
// colour conversion and scaling in one pass, so a frame is read once.
// UVID_SMOOTH is bilinear; UVID_FAST nearest-neighbour, for when the
// time is worth more than the edges (a full-screen frame on a slow CPU).
#define UVID_SMOOTH 0
#define UVID_FAST   1
void uvid_frame_draw(const struct uvid_frame *f, int sx, int sy, int sw, int sh,
                     uint32_t *dst, int dst_stride, int dw, int dh, int quality);

// One still, for a thumbnail, a seek preview or "Save frame": the frame
// showing at `ms`, scaled (aspect kept) to fit max_w x max_h, or at its
// own size when either is 0. `out` is an opaque lib/uimg.h image the
// caller frees with uimg_free().
int uvid_frame_to_uimg(const struct uvid_frame *f, int max_w, int max_h, struct uimg *out);
int uvid_still(const char *path, uint32_t ms, int max_w, int max_h, struct uimg *out);

#endif
