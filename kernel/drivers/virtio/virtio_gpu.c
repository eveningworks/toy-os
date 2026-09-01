// virtio-gpu, 2D: the command protocol and the two queues.
//
// HOW A FRAME REACHES THE SCREEN, because it is not a framebuffer in
// the sense the rest of this kernel means by the word:
//
//   1. RESOURCE_CREATE_2D    -- ask the HOST for a surface of WxH.
//   2. RESOURCE_ATTACH_BACKING -- tell it which GUEST pages hold the
//                               pixels. This is the framebuffer gfx
//                               draws into; it is ordinary RAM.
//   3. SET_SCANOUT           -- point display 0 at that resource.
//   4. TRANSFER_TO_HOST_2D   -- copy a rectangle of guest pixels into
//                               the host's copy of the resource.
//   5. RESOURCE_FLUSH        -- show it.
//
// Steps 4 and 5 are what `flush()` does, and they are why this device
// advertises DISPLAY_CAP_NEEDS_FLUSH. Nothing scans guest memory here:
// pixels written and never transferred are invisible, exactly as on
// vmsvga, and the failure looks like a frozen screen rather than an
// error.
//
// TWO QUEUES, and the second one is the whole point of a cursor plane:
// controlq (0) carries everything above, cursorq (1) carries
// UPDATE_CURSOR/MOVE_CURSOR. A pointer that moves on the cursor queue
// costs one command; a pointer composited into the framebuffer costs a
// damage rectangle, a transfer and a flush every time it moves.
//
// POLLED, like every other virtio device here. Linux's drm/virtio
// completes on an interrupt with fences, and this kernel's transport
// deliberately does not have interrupts yet (see virtqueue.c) -- the
// used ring says the same thing, it just costs CPU to read it.
#include "virtio.h"
#include "virtio_gpu.h"
#include "display.h"
#include "pmm.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"

// driver-none: the virtio transport half; display_virtio.c declares the driver

// --- the protocol (spec 5.7.6) ---------------------------------------

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF        0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT           0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH        0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107

#define VIRTIO_GPU_CMD_UPDATE_CURSOR         0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR           0x0301

#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO      0x1101

#define VIRTIO_GPU_MAX_SCANOUTS 16

// The pixel format. This kernel's framebuffer word is 0x00RRGGBB, which
// in memory is B,G,R,X -- virtio's B8G8R8X8_UNORM, and the same mapping
// Linux makes for DRM_FORMAT_XRGB8888. Getting it wrong does not fail,
// it swaps red and blue, which is why the GUI check reads actual pixel
// values rather than looking at a screenshot.
#define VIRTIO_GPU_FORMAT_B8G8R8X8 2
// The cursor keeps its alpha channel: 0xAARRGGBB in memory is B,G,R,A.
#define VIRTIO_GPU_FORMAT_B8G8R8A8 1

struct gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct gpu_rect { uint32_t x, y, width, height; };

struct gpu_display_one {
    struct gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct gpu_resp_display_info {
    struct gpu_ctrl_hdr hdr;
    struct gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct gpu_resource_create_2d {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct gpu_resource_unref {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

struct gpu_mem_entry { uint64_t addr; uint32_t length; uint32_t padding; };

// One entry is enough because the backing is one contiguous allocation
// -- see the comment on g_fb_phys.
struct gpu_attach_backing {
    struct gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct gpu_mem_entry entry;
};

struct gpu_set_scanout {
    struct gpu_ctrl_hdr hdr;
    struct gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct gpu_transfer_to_host_2d {
    struct gpu_ctrl_hdr hdr;
    struct gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct gpu_resource_flush {
    struct gpu_ctrl_hdr hdr;
    struct gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

struct gpu_cursor_pos { uint32_t scanout_id, x, y, padding; };

struct gpu_update_cursor {
    struct gpu_ctrl_hdr hdr;
    struct gpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
};

// The wire layout is the protocol, so these are compile-time facts --
// the same reasoning as virtio.h's vring asserts. A struct that grew a
// padding byte would otherwise be a device that answers nothing.
_Static_assert(sizeof(struct gpu_ctrl_hdr) == 24, "gpu ctrl header is 24 bytes");
_Static_assert(sizeof(struct gpu_rect) == 16, "gpu rect is 16 bytes");
_Static_assert(sizeof(struct gpu_resp_display_info) == 24 + 24 * 16, "display info response");
_Static_assert(sizeof(struct gpu_transfer_to_host_2d) == 56, "transfer_to_host_2d is 56 bytes");
_Static_assert(sizeof(struct gpu_update_cursor) == 56, "update_cursor is 56 bytes");

// --- state ------------------------------------------------------------

#define GPU_CURSOR_DIM 64                 // virtio-gpu cursors are 64x64, always
#define GPU_CURSOR_BYTES (GPU_CURSOR_DIM * GPU_CURSOR_DIM * 4)

static struct virtio_device g_dev;
static struct virtqueue g_control;   // queue 0
static struct virtqueue g_cursorq;   // queue 1
static int g_present = 0;
static int g_cursor_ok = 0;
static uint32_t g_commands = 0;

static uint32_t g_pref_w = 0, g_pref_h = 0;

// The live scanout resource and the guest memory behind it.
//
// CONTIGUOUS, and that is a requirement rather than a convenience:
// ATTACH_BACKING would happily take a scatter-gather list of scattered
// frames, but `gfx` needs one linear mapping to draw into and this
// kernel's only kernel-side mapping is the identity map of the low
// 4 GiB. Physically contiguous there IS virtually contiguous, so one
// allocation and one memory entry.
static uint32_t g_res_id = 0;
static uint64_t g_fb_phys = 0;
static uint64_t g_fb_frames = 0;
static uint32_t g_fb_w = 0, g_fb_h = 0, g_fb_pitch = 0;

static uint32_t g_cursor_res = 0;
static uint64_t g_cursor_phys = 0;
static int g_cursor_defined = 0;
static int g_cursor_on = 0;
static int g_cursor_x = 0, g_cursor_y = 0;

// Request and response staging, in the kernel image (so below 4 GiB and
// its own physical address). One of each per QUEUE: the control and
// cursor paths are reached from different callers and must not share a
// buffer.
static union {
    struct gpu_ctrl_hdr hdr;
    struct gpu_resource_create_2d create;
    struct gpu_resource_unref unref;
    struct gpu_attach_backing attach;
    struct gpu_set_scanout scanout;
    struct gpu_transfer_to_host_2d xfer;
    struct gpu_resource_flush flush;
} g_req __attribute__((aligned(16)));

static struct gpu_resp_display_info g_resp __attribute__((aligned(16)));
static struct gpu_update_cursor g_cursor_req __attribute__((aligned(16)));

// Not locks -- there is no lock primitive here. They make a re-entrant
// call FAIL rather than corrupt the ring: a flush can arrive from any
// drawing context, and two requests interleaving through one staging
// buffer would send the device a half-overwritten command.
static int g_ctl_busy = 0;
static int g_cur_busy = 0;

// --- the control queue ------------------------------------------------

// Send whatever is in g_req (of `len` bytes) and wait for the response.
// Returns the response type, or 0 if the request never completed.
static uint32_t ctl_send(uint32_t len) {
    if (!g_dev.common) return 0;
    if (g_ctl_busy) {
        klog_write("virtio-gpu: re-entrant command refused\n");
        return 0;
    }
    g_ctl_busy = 1;

    k_memset(&g_resp, 0, sizeof g_resp);

    struct virtio_sg out = { .phys = (uint64_t)(uintptr_t)&g_req, .len = len };
    struct virtio_sg in  = { .phys = (uint64_t)(uintptr_t)&g_resp, .len = sizeof g_resp };

    int head = virtqueue_submit(&g_control, &out, 1, &in, 1);
    if (head < 0) {
        klog_write("virtio-gpu: no free descriptors\n");
        g_ctl_busy = 0;
        return 0;
    }
    virtqueue_kick(&g_control);

    uint32_t used = 0;
    int done = virtqueue_poll(&g_control, head, &used);
    g_ctl_busy = 0;
    if (!done) return 0;          // virtqueue_poll() logged it

    g_commands++;
    return g_resp.hdr.type;
}

// The common case: a command whose only answer is "fine".
static int ctl_ok(uint32_t len, const char *what) {
    uint32_t type = ctl_send(len);
    if (type == VIRTIO_GPU_RESP_OK_NODATA) return 1;
    klog_printf("virtio-gpu: %s failed (response 0x%x)\n", what, type);
    return 0;
}

static void hdr_init(struct gpu_ctrl_hdr *h, uint32_t type) {
    k_memset(h, 0, sizeof *h);
    h->type = type;
}

uint32_t virtio_gpu_commands(void) { return g_commands; }
int virtio_gpu_present(void) { return g_present; }

int virtio_gpu_preferred(uint32_t *out_w, uint32_t *out_h) {
    if (!g_pref_w || !g_pref_h) return 0;
    if (out_w) *out_w = g_pref_w;
    if (out_h) *out_h = g_pref_h;
    return 1;
}

// GET_DISPLAY_INFO: what the host would like scanout 0 to be. QEMU
// answers with the window size it is currently showing, which is how
// `video=` and this driver end up agreeing without either asking the
// other.
static void read_display_info(void) {
    hdr_init(&g_req.hdr, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
    if (ctl_send(sizeof g_req.hdr) != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        klog_write("virtio-gpu: GET_DISPLAY_INFO not answered -- falling back to the mode ladder\n");
        return;
    }
    if (!g_resp.pmodes[0].enabled) return;   // no preference, not a failure
    g_pref_w = g_resp.pmodes[0].r.width;
    g_pref_h = g_resp.pmodes[0].r.height;
}

// --- mode setting -----------------------------------------------------

static int create_resource(uint32_t id, uint32_t w, uint32_t h, uint32_t format) {
    hdr_init(&g_req.create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    g_req.create.resource_id = id;
    g_req.create.format = format;
    g_req.create.width = w;
    g_req.create.height = h;
    return ctl_ok(sizeof g_req.create, "RESOURCE_CREATE_2D");
}

static int attach_backing(uint32_t id, uint64_t phys, uint32_t bytes) {
    hdr_init(&g_req.attach.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    g_req.attach.resource_id = id;
    g_req.attach.nr_entries = 1;
    g_req.attach.entry.addr = phys;
    g_req.attach.entry.length = bytes;
    g_req.attach.entry.padding = 0;
    return ctl_ok(sizeof g_req.attach, "RESOURCE_ATTACH_BACKING");
}

static void unref_resource(uint32_t id) {
    if (!id) return;
    hdr_init(&g_req.unref.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    g_req.unref.resource_id = id;
    g_req.unref.padding = 0;
    ctl_ok(sizeof g_req.unref, "RESOURCE_UNREF");
}

int virtio_gpu_set_mode(uint32_t w, uint32_t h, struct display_surface *out) {
    if (!g_present || !w || !h) return 0;

    uint32_t pitch = w * 4;
    uint64_t bytes = (uint64_t)pitch * h;
    uint64_t frames = (bytes + 4095) / 4096;

    // Everything new is built BEFORE anything old is touched, because
    // display.h requires a failed set_mode to leave the previous mode
    // running. The cost is that both framebuffers are allocated at once
    // for the length of this call -- 16 MiB at 1920x1080, briefly.
    uint64_t phys = pmm_alloc_contiguous(frames);
    if (!phys) {
        klog_printf("virtio-gpu: %ux%u needs %u contiguous frames and none were free\n",
                    w, h, (unsigned)frames);
        return 0;
    }
    // Black, not whatever the last owner left: this memory is about to
    // be scanned out, and an unblanked frame of somebody else's data is
    // a real (and briefly visible) information leak.
    k_memset((void *)(uintptr_t)phys, 0, (unsigned)bytes);

    uint32_t new_id = g_res_id + 1;
    if (new_id == 0) new_id = 1;   // ids are 1-based; 0 means "none"

    if (!create_resource(new_id, w, h, VIRTIO_GPU_FORMAT_B8G8R8X8) ||
        !attach_backing(new_id, phys, (uint32_t)bytes)) {
        pmm_free_contiguous(phys, frames);
        return 0;
    }

    hdr_init(&g_req.scanout.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    g_req.scanout.r.x = 0;
    g_req.scanout.r.y = 0;
    g_req.scanout.r.width = w;
    g_req.scanout.r.height = h;
    g_req.scanout.scanout_id = 0;
    g_req.scanout.resource_id = new_id;
    if (!ctl_ok(sizeof g_req.scanout, "SET_SCANOUT")) {
        unref_resource(new_id);
        pmm_free_contiguous(phys, frames);
        return 0;
    }

    // The device has accepted the new scanout, so the old one is now
    // genuinely unreferenced and safe to release.
    uint32_t old_id = g_res_id;
    uint64_t old_phys = g_fb_phys, old_frames = g_fb_frames;

    g_res_id = new_id;
    g_fb_phys = phys;
    g_fb_frames = frames;
    g_fb_w = w;
    g_fb_h = h;
    g_fb_pitch = pitch;

    if (old_id) {
        unref_resource(old_id);
        pmm_free_contiguous(old_phys, old_frames);
    }

    if (out) {
        out->addr = phys;
        out->pitch = pitch;
        out->width = w;
        out->height = h;
        out->bpp = 32;
    }
    return 1;
}

// --- flush ------------------------------------------------------------

void virtio_gpu_flush(int x, int y, int w, int h) {
    if (!g_present || !g_res_id || w <= 0 || h <= 0) return;

    // Clip to the scanout. A rectangle past the edge is not a caller
    // bug worth refusing -- gfx damages generously -- but the device
    // rejects one, and a rejected transfer means the frame does not
    // appear at all.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)g_fb_w || y >= (int)g_fb_h) return;
    if (x + w > (int)g_fb_w) w = (int)g_fb_w - x;
    if (y + h > (int)g_fb_h) h = (int)g_fb_h - y;
    if (w <= 0 || h <= 0) return;

    hdr_init(&g_req.xfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    g_req.xfer.r.x = (uint32_t)x;
    g_req.xfer.r.y = (uint32_t)y;
    g_req.xfer.r.width = (uint32_t)w;
    g_req.xfer.r.height = (uint32_t)h;
    // The offset is where the rectangle STARTS in the backing, so the
    // device can read a sub-rectangle without being told the pitch --
    // it derives that from the resource's own width.
    g_req.xfer.offset = (uint64_t)y * g_fb_pitch + (uint64_t)x * 4;
    g_req.xfer.resource_id = g_res_id;
    g_req.xfer.padding = 0;
    if (!ctl_ok(sizeof g_req.xfer, "TRANSFER_TO_HOST_2D")) return;

    hdr_init(&g_req.flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    g_req.flush.r.x = (uint32_t)x;
    g_req.flush.r.y = (uint32_t)y;
    g_req.flush.r.width = (uint32_t)w;
    g_req.flush.r.height = (uint32_t)h;
    g_req.flush.resource_id = g_res_id;
    g_req.flush.padding = 0;
    ctl_ok(sizeof g_req.flush, "RESOURCE_FLUSH");
}

// --- the cursor queue -------------------------------------------------

// A cursor command carries no response: the device's answer is the used
// ring entry, which is also how the descriptor comes back. So this
// still polls -- not to read anything, but because leaving chains
// outstanding is how the queue silently runs down.
static int cursor_send(void) {
    if (!g_cursor_ok) return 0;
    if (g_cur_busy) return 0;
    g_cur_busy = 1;

    struct virtio_sg out = { .phys = (uint64_t)(uintptr_t)&g_cursor_req,
                             .len = sizeof g_cursor_req };
    int head = virtqueue_submit(&g_cursorq, &out, 1, 0, 0);
    if (head < 0) {
        g_cur_busy = 0;
        return 0;
    }
    virtqueue_kick(&g_cursorq);
    int done = virtqueue_poll(&g_cursorq, head, 0);
    g_cur_busy = 0;
    return done;
}

int virtio_gpu_cursor_available(void) { return g_present && g_cursor_ok; }

int virtio_gpu_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    if (!virtio_gpu_cursor_available() || !argb) return 0;
    if (w <= 0 || h <= 0 || w > GPU_CURSOR_DIM || h > GPU_CURSOR_DIM) return 0;

    // A virtio-gpu cursor resource is 64x64 and nothing else, so a
    // smaller image is CENTRED at the top-left and padded with
    // transparent pixels rather than refused -- the hotspot is what
    // decides where it points, and it is unchanged by the padding.
    uint32_t *dst = (uint32_t *)(uintptr_t)g_cursor_phys;
    k_memset(dst, 0, GPU_CURSOR_BYTES);
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            dst[row * GPU_CURSOR_DIM + col] = argb[row * w + col];
        }
    }

    // The pixels are in guest memory; the host needs its own copy.
    hdr_init(&g_req.xfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    g_req.xfer.r.x = 0;
    g_req.xfer.r.y = 0;
    g_req.xfer.r.width = GPU_CURSOR_DIM;
    g_req.xfer.r.height = GPU_CURSOR_DIM;
    g_req.xfer.offset = 0;
    g_req.xfer.resource_id = g_cursor_res;
    g_req.xfer.padding = 0;
    if (!ctl_ok(sizeof g_req.xfer, "TRANSFER_TO_HOST_2D (cursor)")) return 0;

    k_memset(&g_cursor_req, 0, sizeof g_cursor_req);
    hdr_init(&g_cursor_req.hdr, VIRTIO_GPU_CMD_UPDATE_CURSOR);
    g_cursor_req.pos.scanout_id = 0;
    g_cursor_req.pos.x = (uint32_t)g_cursor_x;
    g_cursor_req.pos.y = (uint32_t)g_cursor_y;
    g_cursor_req.resource_id = g_cursor_res;
    g_cursor_req.hot_x = (uint32_t)(hot_x < 0 ? 0 : hot_x);
    g_cursor_req.hot_y = (uint32_t)(hot_y < 0 ? 0 : hot_y);
    if (!cursor_send()) return 0;

    g_cursor_defined = 1;
    g_cursor_on = 1;
    return 1;
}

void virtio_gpu_cursor_move(int x, int y) {
    if (!virtio_gpu_cursor_available()) return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_cursor_x = x;
    g_cursor_y = y;
    if (!g_cursor_defined || !g_cursor_on) return;

    k_memset(&g_cursor_req, 0, sizeof g_cursor_req);
    hdr_init(&g_cursor_req.hdr, VIRTIO_GPU_CMD_MOVE_CURSOR);
    g_cursor_req.pos.scanout_id = 0;
    g_cursor_req.pos.x = (uint32_t)x;
    g_cursor_req.pos.y = (uint32_t)y;
    // MOVE_CURSOR carries the resource id too: the device treats a
    // move as an update whose image happens to be unchanged.
    g_cursor_req.resource_id = g_cursor_res;
    cursor_send();
}

void virtio_gpu_cursor_show(int on) {
    if (!virtio_gpu_cursor_available() || !g_cursor_defined) return;
    g_cursor_on = on ? 1 : 0;

    // There is no "hide" command. Hiding is UPDATE_CURSOR with resource
    // id 0, which is the spec's way of saying "no image on this
    // scanout" -- the same convention as SET_SCANOUT with id 0.
    k_memset(&g_cursor_req, 0, sizeof g_cursor_req);
    hdr_init(&g_cursor_req.hdr, VIRTIO_GPU_CMD_UPDATE_CURSOR);
    g_cursor_req.pos.scanout_id = 0;
    g_cursor_req.pos.x = (uint32_t)g_cursor_x;
    g_cursor_req.pos.y = (uint32_t)g_cursor_y;
    g_cursor_req.resource_id = g_cursor_on ? g_cursor_res : 0;
    cursor_send();
}

// --- bring-up ---------------------------------------------------------

static int cursor_init(void) {
    if (!virtqueue_setup(&g_dev, 1, &g_cursorq)) {
        klog_write("virtio-gpu: no cursor queue -- the pointer stays a software sprite\n");
        return 0;
    }
    g_cursor_phys = pmm_alloc_contiguous((GPU_CURSOR_BYTES + 4095) / 4096);
    if (!g_cursor_phys) {
        klog_write("virtio-gpu: no frames for a cursor resource\n");
        virtqueue_teardown(&g_cursorq);
        return 0;
    }
    k_memset((void *)(uintptr_t)g_cursor_phys, 0, GPU_CURSOR_BYTES);

    // Out of the scanout resources' counting range, which increments
    // from 1 on every mode change.
    g_cursor_res = 0xC0;
    if (!create_resource(g_cursor_res, GPU_CURSOR_DIM, GPU_CURSOR_DIM,
                         VIRTIO_GPU_FORMAT_B8G8R8A8) ||
        !attach_backing(g_cursor_res, g_cursor_phys, GPU_CURSOR_BYTES)) {
        pmm_free_contiguous(g_cursor_phys, (GPU_CURSOR_BYTES + 4095) / 4096);
        virtqueue_teardown(&g_cursorq);
        return 0;
    }
    g_cursor_ok = 1;
    return 1;
}

int virtio_gpu_init(void) {
    if (g_present) return 1;

    g_dev.name = "virtio-gpu";
    // No such device is the ordinary case: silent, no allocation, no
    // PCI writes.
    if (!virtio_pci_find(VIRTIO_ID_GPU, 0, &g_dev)) return 0;

    // Neither VIRGL nor EDID is asked for: this is a 2D driver, and a
    // feature negotiated but unimplemented is the kind of half-support
    // virtio_begin()'s modern-only refusal exists to avoid.
    if (!virtio_begin(&g_dev, 0)) return 0;

    if (!virtqueue_setup(&g_dev, 0, &g_control)) {
        klog_write("virtio-gpu: could not set up its control queue\n");
        virtio_fail(&g_dev);
        return 0;
    }
    // The cursor queue is optional; the control queue is not. The
    // device says how many it has -- asked rather than assumed, because
    // virtqueue_setup() on a queue the device does not have looks
    // exactly like a queue that failed to allocate.
    uint16_t num_queues = *(volatile uint16_t *)(g_dev.common + VIRTIO_COMMON_NUMQ);
    int want_cursor = num_queues >= 2;

    virtio_driver_ok(&g_dev);
    g_present = 1;

    read_display_info();
    if (want_cursor) cursor_init();

    klog_printf("virtio-gpu: claimed, preferred %ux%u, cursor plane %s\n",
                g_pref_w, g_pref_h, g_cursor_ok ? "yes" : "no");
    return 1;
}
