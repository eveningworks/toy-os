#ifndef ULIB_UVID_INTERNAL_H
#define ULIB_UVID_INTERNAL_H

// Shared between uvid.c, its containers and codecs, and usnd_vid.c (a
// video file's sound). Not for apps.
#include "lib/uvid.h"
#include <stddef.h>
#include <stdint.h>

// Records the sentence uvid_last_error() returns.
void uvid_fail(const char *msg);

enum { UVID_VIDEO, UVID_AUDIO };

// What a container found inside. NONE is "no such stream"; UNKNOWN is a
// stream this build cannot decode, with its name in the src's fourcc.
enum { UVID_VC_NONE, UVID_VC_UNKNOWN, UVID_VC_MJPEG, UVID_VC_MPEG1 };
enum { UVID_AC_NONE, UVID_AC_UNKNOWN, UVID_AC_PCM, UVID_AC_MP2 };

// One packet of one stream. The buffer is the CALLER's and is reused:
// a container grows it with uvid_pkt_reserve() and never frees it.
struct uvid_pkt {
    uint8_t *data;
    size_t len, cap;
    int64_t pts_ms;         // -1 when the container did not say
    int key;                // decodable without what came before
};

int  uvid_pkt_reserve(struct uvid_pkt *p, size_t n);
void uvid_pkt_free(struct uvid_pkt *p);

struct uvid_src;

struct uvid_container {
    const char *name;
    // Magic bytes only, as every probe here.
    int (*probe)(const uint8_t *d, size_t n);
    // Fills the src's description from the headers; allocates priv.
    int (*open)(struct uvid_src *c);
    // The next packet of stream `kind`, skipping the others: 1, 0 at the
    // end, or a negative errno.
    int (*read)(struct uvid_src *c, int kind, struct uvid_pkt *p);
    // Positions `kind` so its next packet starts at or before `ms` -- for
    // video, a packet a decoder can start from. Returns the time the
    // next packet starts at, or a negative errno.
    int64_t (*seek)(struct uvid_src *c, int kind, int64_t ms);
    void (*close)(struct uvid_src *c);
};

// A file opened by one container, for ONE reader: the video half and
// the sound half each open their own, so neither moves the other's
// position.
struct uvid_src {
    const struct uvid_container *ops;
    int fd;                 // borrowed: whoever opened the src closes it
    uint64_t size;
    void *priv;

    int vcodec;
    char vfourcc[8];        // what an UNKNOWN video stream said it was
    int w, h;
    uint32_t fps_num, fps_den;
    uint32_t ms;            // the longer stream's duration, 0 unknown
    uint32_t frames;

    int acodec;
    char afourcc[8];
    uint32_t a_rate;
    uint16_t a_channels, a_bits;
    uint32_t a_kbps;
    uint64_t a_frames;      // sample frames in the sound stream, 0 unknown
    // Set by a sound seek: the sample frame the next packet starts at,
    // which a seek trims from -- milliseconds are too coarse to land on
    // a sample. A container that cannot say derives it from the time.
    uint64_t a_next_frame;
};

// Probes and opens `fd`. 0, or a negative errno with uvid_fail() set.
int  uvid_src_open(int fd, struct uvid_src *c);
void uvid_src_close(struct uvid_src *c);

extern const struct uvid_container uvid_container_avi;
extern const struct uvid_container uvid_container_ps;

struct uvid_codec {
    int id;                 // UVID_VC_*
    const char *name;
    int  (*open)(struct uvid *v);
    // Hands over one packet; 0 or a negative errno. A codec that keeps
    // the bytes copies them -- the packet buffer is reused.
    int  (*feed)(struct uvid *v, const struct uvid_pkt *p);
    // A decoded frame, if one is ready: 1, else 0. `eof` asks for what a
    // reordering codec still holds back (MPEG's last anchor frame).
    int  (*frame)(struct uvid *v, int eof, const struct uvid_frame **out);
    // After a seek: forget references and partial input.
    void (*reset)(struct uvid *v);
    void (*close)(struct uvid *v);
};

extern const struct uvid_codec uvid_codec_mjpeg;
extern const struct uvid_codec uvid_codec_mpeg1;

// Every MPEG-1 code table a prefix code: 0, else the clashes found.
// Needs no video; /tests/uvid_test and tools/uvid_hostcheck.py run it.
int uvid_mpeg1_selftest(void);

struct uvid {
    int fd;
    struct uvid_src src;
    const struct uvid_codec *codec;
    void *cpriv;
    struct uvid_info info;
    struct uvid_pkt pkt;
    int at_end;             // the container has no more video packets
    // An exact seek decodes forward until it reaches the frame showing
    // at the target; that frame is handed out by the next uvid_next().
    const struct uvid_frame *held;
};

#endif
