// Intel gen8 (Broadwell) integrated graphics -- the display engine as
// a display_driver. See intel_display.h for what it is and is not.
//
// THE INVARIANT: the probe never programs a mode. The firmware's GOP
// lit the panel, chose the pipe and pointed the primary plane at the
// framebuffer GRUB reports; the probe reads that back and refuses to
// claim unless every fact agrees (Linux's fastboot readout). A modeset
// happens only when asked for (set_mode, intel_modeset.c), and today
// it re-programs the native mode; nothing at boot can black the screen.
//
// THE TRAP: the display engine reads memory through the GGTT, not the
// CPU's page tables, and its reads do not snoop the CPU cache unless
// the PTE says so. A cursor image written to ordinary memory must be
// CLFLUSHed before the plane is pointed at it, or it shows stale RAM
// -- the same failure hda.c's NOSNOOP clear exists for.
//
// Register offsets are from Intel's public Broadwell PRM (Vol 2c) and
// agree with Linux's i915_reg.h; pipe B and C are +0x1000 and +0x2000
// from pipe A.
#include "ratelimit.h"   // the miss line, at most once a second
#include "intel_display.h"
#include "display.h"
#include "pci.h"
#include "pci_internal.h"
#include "paging.h"
#include "pmm.h"
#include "barrier.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"   // k_memset
#include "driver.h" // DRIVER_DECLARE -- `lsdrv -v` names THIS file
#include "intel_internal.h"
#include "edid.h"     // the mode list: the EDID's listed modes and DMT timings

DRIVER_DECLARE("intel-display", "display", "Intel gen8/gen9 display engine: modeset, cursor plane, page flip, backlight (gen8)");

// --- PCI ------------------------------------------------------------
#define INTEL_VENDOR 0x8086
// Host bridge (00:00.0) config space, Broadwell layout.
#define GMCH_CTRL   0x50 // bits 7:6 GGTT size (2^n MiB of PTEs), 15:8 stolen (x32 MiB)
#define GMCH_BSM    0x5C // base of stolen memory, bits 31:20

// --- state ----------------------------------------------------------
static const struct pci_device *g_dev;
static volatile uint8_t *g_mmio;
static volatile uint64_t *g_gtt;   // the GGTT PTE array, upper half of BAR0
static uint32_t g_gtt_entries;
static uint64_t g_aperture;        // BAR2: where the GGTT is visible to the CPU
static uint64_t g_aperture_size;
static uint64_t g_stolen_base, g_stolen_size;
static int g_pipe = -1;            // the pipe scanning GRUB's framebuffer
static uint32_t g_fb_ggtt;         // its DSPSURF
static struct display_surface g_surface;
static uint32_t g_native_w, g_native_h;   // what the firmware lit: the panel's size
static int g_active;
static uint64_t g_fb_pte_flags;    // low 12 bits of the firmware's framebuffer PTE, reused as-is

// The cursor plane: a 64x64 ARGB image in system memory, mapped into
// the GGTT just past the stolen region -- an offset the firmware has
// no mapping at, so nothing it set up is disturbed.
#define CURSOR_DIM   64
#define CURSOR_BYTES (CURSOR_DIM * CURSOR_DIM * 4)
#define CURSOR_PAGES (CURSOR_BYTES / 4096)
static uint64_t g_cursor_phys;
static uint32_t g_cursor_ggtt;
static int g_cursor_ok;
static int g_cursor_hot_x, g_cursor_hot_y;
static int g_cursor_on;

// Backlight: which PWM drives the pin (the firmware's choice, read
// back), and the period that means 100%.
static int g_bl_cpu_mode;
static uint32_t g_bl_max;

// Two more scanouts of the firmware's geometry, in system memory
// behind GGTT entries past the cursor's. A flip is one DSPSURF write,
// latched at vblank; DSPSURFLIVE says which buffer is being scanned.
// THREE, not two: with the flip never waiting, the buffer handed back
// to draw into must be neither the live one nor the one just asked
// for, and only a third buffer is always both.
#define SCANOUTS 3
static struct display_surface g_scanout[SCANOUTS];
static uint32_t g_scanout_ggtt[SCANOUTS];
static int g_scanouts = 1;
// The extra scanouts' frames, as allocated -- not derived from
// g_scanouts, which stays 1 when the SECOND extra fails after the first
// succeeded and its frames are kept unused.
static uint64_t g_scanout_ram_pages;

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(g_mmio + off); }
static struct display_driver intel_driver;

static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_mmio + off) = v; }

uint32_t intel_rd(uint32_t off) { return rd(off); }
void intel_wr(uint32_t off, uint32_t v) { wr(off, v); }
int intel_display_pipe(void) { return g_active ? g_pipe : -1; }

// Broadwell device ids, GT1..GT3, mobile and desktop; the machine this
// was written against is a GT1 (8086:161e).
static const uint16_t BDW_IDS[] = {
    0x1602, 0x1606, 0x160A, 0x160B, 0x160D, 0x160E,
    0x1612, 0x1616, 0x161A, 0x161B, 0x161D, 0x161E,
    0x1622, 0x1626, 0x162A, 0x162B, 0x162D, 0x162E,
    0x1632, 0x1636, 0x163A, 0x163B, 0x163D, 0x163E,
};

static const struct intel_gen_ops *const GENS[] = { &intel_gen8_ops, &intel_gen9_ops };
static const struct intel_gen_ops *g_ops;   // the claimed device's; set by find_gpu()

static const struct intel_gen_ops *gen_of(uint16_t id) {
    for (unsigned g = 0; g < sizeof GENS / sizeof GENS[0]; g++)
        for (int i = 0; i < GENS[g]->nids; i++)
            if (GENS[g]->ids[i] == id) return GENS[g];
    return 0;
}

// The firmware's plane, as each generation encodes it: gen8 has the
// format in 29:26 and the stride in bytes; gen9 the format in 27:24,
// tiling in 12:10 and the stride in 64-byte units.
int intel_display_plane_matches(int gen, uint32_t cntr, uint32_t stride, uint32_t pitch) {
    if (!(cntr & DSPCNTR_ENABLE)) return 0;
    if (gen == 8)
        return (cntr & DSPCNTR_FMT_MASK) == DSPCNTR_BGRX8888 && stride == pitch;
    if (gen == 9)
        return (cntr & PLANE_CTL_FMT_MASK) == PLANE_CTL_XRGB8888 &&
               !(cntr & (PLANE_CTL_TILED_MASK | PLANE_CTL_ORDER_RGBX | PLANE_CTL_ROTATE_MASK)) &&
               stride * 64 == pitch;
    return 0;
}

static const struct pci_device *find_gpu(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (!d || d->vendor_id != INTEL_VENDOR) continue;
        if (d->class_code != 0x03) continue;
        g_ops = gen_of(d->device_id);
        if (!g_ops) {
            klog_printf("intel-display: %04x:%04x is not gen8 or gen9 -- not claimed\n",
                        d->vendor_id, d->device_id);
            return 0;
        }
        return d;
    }
    return 0;
}

static const struct pci_device *find_host_bridge(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d && d->bus == 0 && d->device == 0 && d->function == 0 &&
            d->vendor_id == INTEL_VENDOR) return d;
    }
    return 0;
}

// Everything the driver will later write, read out once and logged: a
// probe that changes nothing is what makes the first boot on a new
// machine safe to look at.
static void log_readout(void) {
    for (int p = 0; p < 3; p++) {
        klog_printf("intel-display: pipe %c conf %#x src %#x plane cntr %#x stride %u surf %#x cursor cntr %#x base %#x frame %u\n",
                    'A' + p, rd(PIPECONF(p)), rd(PIPESRC(p)), rd(DSPCNTR(p)),
                    rd(DSPSTRIDE(p)), rd(DSPSURF(p)), rd(CURCNTR(p)), rd(CURBASE(p)),
                    rd(PIPEFRAME(p)));
    }
    klog_printf("intel-display: edp ddi func %#x pp status %#x control %#x power well bios %#x driver %#x\n",
                rd(TRANS_DDI_FUNC_CTL_EDP), rd(PCH_PP_STATUS), rd(PCH_PP_CONTROL),
                rd(HSW_PWR_WELL_CTL_BIOS), rd(HSW_PWR_WELL_CTL_DRIVER));
    klog_printf("intel-display: backlight pch ctl1 %#x ctl2 %#x cpu ctl2 %#x ctl %#x\n",
                rd(BLC_PWM_PCH_CTL1), rd(BLC_PWM_PCH_CTL2),
                rd(BLC_PWM_CPU_CTL2), rd(BLC_PWM_CPU_CTL));
}

// --- the display power well ------------------------------------------
// Pipes B/C, the DDI B-D ports and display audio hang off it. The
// firmware holds it through the BIOS request register; the driver takes
// its own request so the state no longer depends on that. A bounded
// spin: this runs before the timer exists.
// READ-MODIFY-WRITE: gen9's register holds a request bit per well (PW1,
// MISC IO, each DDI's IO); writing bit 31 alone would drop the rest.
static void setup_power_well(void) {
    uint32_t before = rd(HSW_PWR_WELL_CTL_DRIVER);
    wr(HSW_PWR_WELL_CTL_DRIVER, before | PWR_WELL_REQUEST);
    uint32_t v = 0;
    for (int i = 0; i < 100000; i++) {
        v = rd(HSW_PWR_WELL_CTL_DRIVER);
        if (v & PWR_WELL_STATE) break;
        cpu_relax();
    }
    klog_printf("intel-display: power well %s (driver ctl %#x -> %#x, bios %#x)\n",
                (v & PWR_WELL_STATE) ? "on" : "DID NOT COME UP",
                before, v, rd(HSW_PWR_WELL_CTL_BIOS));
}

int intel_display_power_well(int on) {
    if (!g_active) return 0;
    uint32_t was = rd(HSW_PWR_WELL_CTL_DRIVER);
    wr(HSW_PWR_WELL_CTL_DRIVER, on ? was | PWR_WELL_REQUEST : was & ~PWR_WELL_REQUEST);
    uint32_t v = 0;
    for (int i = 0; i < 100000; i++) {
        v = rd(HSW_PWR_WELL_CTL_DRIVER);
        if (((v & PWR_WELL_STATE) != 0) == (on != 0)) return 1;
        cpu_relax();
    }
    return 0;
}

// --- the cursor plane ------------------------------------------------
// The display engine's TLB over the GGTT: written PTEs are not seen
// until this is poked (Linux's gen8 ggtt invalidate).
#define GFX_FLSH_CNTL      0x101008
#define GFX_FLSH_CNTL_EN   (1u << 4)

static void ggtt_invalidate(void) {
    wr(GFX_FLSH_CNTL, GFX_FLSH_CNTL_EN);
    (void)rd(GFX_FLSH_CNTL);
}

static void setup_cursor(void) {
    g_cursor_ok = 0;
    if (g_pipe < 0 || !g_gtt_entries || !g_stolen_size) return;
    uint32_t idx = (uint32_t)(g_stolen_size >> 12);
    if (idx + CURSOR_PAGES > g_gtt_entries) return;
    uint64_t phys = pmm_alloc_contiguous(CURSOR_PAGES, PMM_ZONE_DMA32);
    if (!phys) return;
    uint64_t was = g_gtt[idx];
    for (int i = 0; i < CURSOR_PAGES; i++)
        g_gtt[idx + i] = (phys + (uint64_t)i * 4096) | g_fb_pte_flags;
    ggtt_invalidate();
    g_cursor_phys = phys;
    g_cursor_ggtt = idx << 12;
    if (g_ops->cursor_prepare && !g_ops->cursor_prepare(g_pipe)) {
        // No cursor plane after all: the slot back to what it held, the
        // frames back to the allocator.
        g_gtt[idx] = was;
        for (int i = 1; i < CURSOR_PAGES; i++) g_gtt[idx + i] = 0;
        ggtt_invalidate();
        pmm_free_contiguous(phys, CURSOR_PAGES);
        g_cursor_phys = 0;
        g_cursor_ggtt = 0;
        return;
    }
    // Plane off until something defines a shape.
    wr(CURCNTR(g_pipe), 0);
    wr(CURBASE(g_pipe), g_cursor_ggtt);
    g_cursor_ok = 1;
    intel_driver.caps |= DISPLAY_CAP_CURSOR;
    klog_printf("intel-display: cursor plane on pipe %c, %ux%u at ggtt %#x -> %#llx (slot held %#llx)\n",
                'A' + g_pipe, CURSOR_DIM, CURSOR_DIM, g_cursor_ggtt,
                (unsigned long long)phys, (unsigned long long)was);
}

static void clflush_range(volatile void *p, uint32_t len) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint32_t off = 0; off < len; off += 64)
        __asm__ volatile ("clflush (%0)" :: "r"(b + off) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}

static int intel_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    if (!g_cursor_ok || !argb) return 0;
    if (w <= 0 || h <= 0 || w > CURSOR_DIM || h > CURSOR_DIM) return 0;
    if (hot_x < 0 || hot_y < 0 || hot_x >= w || hot_y >= h) return 0;
    // The plane blends PREMULTIPLIED alpha (what a Wayland cursor
    // surface carries); the sprite arrives straight. Smaller shapes are
    // padded with transparent pixels, as virtio-gpu's cursor is.
    volatile uint32_t *img = (volatile uint32_t *)(uintptr_t)g_cursor_phys;
    for (int y = 0; y < CURSOR_DIM; y++) {
        for (int x = 0; x < CURSOR_DIM; x++) {
            uint32_t px = 0;
            if (x < w && y < h) {
                uint32_t v = argb[y * w + x];
                uint32_t a = v >> 24;
                uint32_t r = ((v >> 16) & 0xFF) * a / 255;
                uint32_t g = ((v >> 8) & 0xFF) * a / 255;
                uint32_t b = (v & 0xFF) * a / 255;
                px = (a << 24) | (r << 16) | (g << 8) | b;
            }
            img[y * CURSOR_DIM + x] = px;
        }
    }
    clflush_range(img, CURSOR_BYTES);
    g_cursor_hot_x = hot_x;
    g_cursor_hot_y = hot_y;
    wr(CURCNTR(g_pipe), g_cursor_on ? CURCNTR_64_ARGB : 0);
    wr(CURBASE(g_pipe), g_cursor_ggtt); // the arming write
    return 1;
}

uint32_t intel_display_curpos_field(int v) {
    uint32_t mag = (uint32_t)(v < 0 ? -v : v) & 0x1FFF;
    return v < 0 ? (mag | CURPOS_SIGN) : mag;
}

uint32_t intel_display_duty(uint32_t max, int percent) {
    uint32_t duty = (max * (uint32_t)percent + 50) / 100;
    if (percent > 0 && duty == 0) duty = 1;
    if (duty > max) duty = max;
    return duty;
}

int intel_display_percent(uint32_t max, uint32_t duty) {
    if (!max) return -1;
    if (duty > max) duty = max;
    return (int)((duty * 100u + max / 2) / max);
}

static void intel_cursor_move(int x, int y) {
    if (!g_cursor_ok) return;
    x -= g_cursor_hot_x;
    y -= g_cursor_hot_y;
    wr(CURPOS(g_pipe), (intel_display_curpos_field(y) << 16) | intel_display_curpos_field(x));
    wr(CURBASE(g_pipe), g_cursor_ggtt);
}

static void intel_cursor_show(int on) {
    if (!g_cursor_ok) return;
    g_cursor_on = on ? 1 : 0;
    wr(CURCNTR(g_pipe), g_cursor_on ? CURCNTR_64_ARGB : 0);
    wr(CURBASE(g_pipe), g_cursor_ggtt);
}

// --- the second scanout and the flip -----------------------------------

static void setup_scanouts(void) {
    g_scanout[0] = g_surface;
    g_scanout_ggtt[0] = g_fb_ggtt;
    g_scanouts = 1;
    if (!g_cursor_ok) return;   // no GGTT slot discipline without it
    uint64_t bytes = (uint64_t)g_surface.pitch * g_surface.height;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
    uint32_t align = g_ops->scanout_align_pages ? g_ops->scanout_align_pages : 1;
    uint32_t idx = (g_cursor_ggtt >> 12) + CURSOR_PAGES;
    for (int b = 1; b < SCANOUTS; b++) {
        idx = (idx + align - 1) / align * align;
        if (idx + pages > g_gtt_entries) return;
        uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_DMA32);
        if (!phys) {
            klog_printf("intel-display: no %u contiguous frames for scanout %d -- no flip\n",
                        pages, b);
            return;   // an earlier extra buffer is simply unused
        }
        k_memset((void *)(uintptr_t)phys, 0, (unsigned)bytes);
        for (uint32_t i = 0; i < pages; i++)
            g_gtt[idx + i] = (phys + (uint64_t)i * 4096) | g_fb_pte_flags;
        ggtt_invalidate();
        // Write-combining for the CPU side, exactly as display_probe()
        // does for the first surface; the ring-3 grant maps it WC.
        int wc = paging_set_write_combining(phys, bytes);
        g_scanout_ram_pages += pages;
        g_scanout[b] = g_surface;
        g_scanout[b].addr = phys;
        g_scanout_ggtt[b] = idx << 12;
        klog_printf("intel-display: scanout %d at ggtt %#x -> %#llx (%u pages, wc %s)\n",
                    b, g_scanout_ggtt[b], (unsigned long long)phys, pages, paging_wc_name(wc));
        idx += pages;
    }
    g_scanouts = SCANOUTS;
    intel_driver.caps |= DISPLAY_CAP_FLIP;
}

static int intel_scanout_count(void) { return g_scanouts; }

// Scanout 0 is the firmware's, in stolen memory the allocator never
// counted; the extras and the cursor image are RAM.
static uint64_t intel_ram_bytes(void) {
    // The cursor image by its allocation too: a gen9 claim with no DDB
    // slice keeps the frames and leaves g_cursor_ok 0.
    return ((g_cursor_phys ? CURSOR_PAGES : 0) + g_scanout_ram_pages) * 4096;
}

static void intel_scanout_at(int index, struct display_surface *out) {
    *out = g_scanout[(index >= 0 && index < g_scanouts) ? index : 0];
}

// Mailbox: the write is latched at the next vblank, and a second write
// before then replaces the first. Nothing waits -- this runs inside a
// syscall with interrupts off, where a wait of up to a frame would
// stall the keyboard, the mouse and the network a frame at a time.
static int intel_flip(int index) {
    if (g_pipe < 0 || index < 0 || index >= g_scanouts) return 0;
    wr(DSPSURF(g_pipe), g_scanout_ggtt[index]);
    return 1;
}

// WHEN THIS CANNOT TELL, IT SAYS 0, AND 0 MAY BE THE BUFFER ON SCREEN.
// free_back() (win_surface.c) excludes the answer from what it hands
// the compositor to draw into, so a wrong answer hands back a LIVE
// buffer and the next frame is painted where the panel can see it --
// a window drawn in front of you instead of appearing whole. The
// counter is how a session tells that from a present-path fault; the
// log line is rate-limited because a present runs at frame rate.
static unsigned long long g_live_miss, g_live_calls;
static struct ratelimit g_live_rl;
static uint32_t g_live_last_raw;

void intel_display_live_stats(unsigned long long *calls, unsigned long long *miss,
                              uint32_t *last_raw) {
    if (calls)    *calls    = g_live_calls;
    if (miss)     *miss     = g_live_miss;
    if (last_raw) *last_raw = g_live_last_raw;
}

static int intel_scanout_live(void) {
    // The address is 31:12; gen9 reads 0x20 in the low bits even on a
    // plane that is off. The diagnostic keeps the register as read.
    uint32_t raw = rd(DSPSURFLIVE(g_pipe));
    uint32_t live = raw & ~0xFFFu;
    g_live_calls++;
    g_live_last_raw = raw;
    for (int b = 0; b < g_scanouts; b++)
        if (g_scanout_ggtt[b] == live) return b;
    g_live_miss++;
    if (ratelimit_ok(&g_live_rl, 0)) {      // one line a second
        klog_printf("intel-display: DSPSURFLIVE %#x matches no scanout (%llu of %llu) -- "
                    "assuming 0; scanouts %#x %#x %#x\n",
                    live, g_live_miss, g_live_calls,
                    g_scanouts > 0 ? g_scanout_ggtt[0] : 0,
                    g_scanouts > 1 ? g_scanout_ggtt[1] : 0,
                    g_scanouts > 2 ? g_scanout_ggtt[2] : 0);
    }
    return 0;
}

int intel_display_scanout_count(void) { return g_active ? g_scanouts : 0; }

// --- the backlight ---------------------------------------------------
// Linux's lpt_setup_backlight, minus the mode switch: the duty register
// written is whichever PWM the firmware left driving the pin.
static void setup_backlight(void) {
    uint32_t pch1 = rd(BLC_PWM_PCH_CTL1);
    uint32_t pch2 = rd(BLC_PWM_PCH_CTL2);
    uint32_t cpu2 = rd(BLC_PWM_CPU_CTL2);
    g_bl_max = pch2 >> 16;
    if (!(pch1 & BLM_PWM_ENABLE) || !g_bl_max) {
        klog_printf("intel-display: backlight PWM not enabled by firmware (pch ctl1 %#x ctl2 %#x) -- no control\n",
                    pch1, pch2);
        return;
    }
    g_bl_cpu_mode = !(pch1 & BLM_PCH_OVERRIDE_ENABLE) && (cpu2 & BLM_PWM_ENABLE);
    intel_driver.caps |= DISPLAY_CAP_BACKLIGHT;
    klog_printf("intel-display: backlight via %s PWM, period %u, duty %u%s\n",
                g_bl_cpu_mode ? "CPU" : "PCH", g_bl_max,
                (g_bl_cpu_mode ? rd(BLC_PWM_CPU_CTL) : pch2) & 0xFFFF,
                (pch1 & BLM_PCH_POLARITY) ? " (active low)" : "");
}

static uint32_t backlight_duty(void) {
    return (g_bl_cpu_mode ? rd(BLC_PWM_CPU_CTL) : rd(BLC_PWM_PCH_CTL2)) & 0xFFFF;
}

static int intel_backlight_get(void) {
    if (!g_bl_max) return -1;
    return intel_display_percent(g_bl_max, backlight_duty());
}

static int intel_backlight_set(int percent) {
    if (!g_bl_max) return 0;
    uint32_t duty = intel_display_duty(g_bl_max, percent);
    if (g_bl_cpu_mode) {
        wr(BLC_PWM_CPU_CTL, (rd(BLC_PWM_CPU_CTL) & 0xFFFF0000u) | duty);
    } else {
        wr(BLC_PWM_PCH_CTL2, (g_bl_max << 16) | duty);
    }
    return backlight_duty() == duty;
}

static int intel_probe(void) {
    g_active = 0;
    g_dev = find_gpu();
    if (!g_dev) return 0;

    uint64_t bar0 = pci_bar_mem_addr(g_dev, 0);
    uint64_t bar0_size = pci_bar_mem_size(g_dev, 0);
    g_aperture = pci_bar_mem_addr(g_dev, 2);
    g_aperture_size = pci_bar_mem_size(g_dev, 2);
    if (!bar0 || bar0_size < (2u << 20) || !g_aperture) {
        klog_printf("intel-display: unexpected BARs (bar0 %#llx/%llu bar2 %#llx) -- not claimed\n",
                    (unsigned long long)bar0, (unsigned long long)bar0_size,
                    (unsigned long long)g_aperture);
        return 0;
    }
    g_mmio = (volatile uint8_t *)paging_map_device(bar0, bar0_size);
    if (!g_mmio) {
        klog_printf(KLOG_ERR "intel-display: BAR0 at %#llx could not be mapped\n", (unsigned long long)bar0);
        return 0;
    }
    // The GGTT occupies the upper half of BAR0 (gen8), one 64-bit PTE
    // per 4 KiB of GPU address; GMCH_CTRL says how much of it exists.
    g_gtt = (volatile uint64_t *)(g_mmio + bar0_size / 2);
    const struct pci_device *hb = find_host_bridge();
    uint32_t gmch = hb ? pci_config_read32(hb, GMCH_CTRL) : 0;
    uint32_t ggms = (gmch >> 6) & 3;
    uint64_t gtt_bytes = ggms ? ((uint64_t)1 << ggms) << 20 : 0;
    g_gtt_entries = (uint32_t)(gtt_bytes / 8);
    g_stolen_size = g_ops->stolen_bytes((gmch >> 8) & 0xFF);
    g_stolen_base = hb ? (pci_config_read32(hb, GMCH_BSM) & 0xFFF00000u) : 0;
    klog_printf("intel-display: %04x:%04x mmio %#llx/%lluM aperture %#llx/%lluM gtt %u entries stolen %#llx/%lluM cmd %#x\n",
                g_dev->vendor_id, g_dev->device_id,
                (unsigned long long)bar0, (unsigned long long)(bar0_size >> 20),
                (unsigned long long)g_aperture, (unsigned long long)(g_aperture_size >> 20),
                g_gtt_entries, (unsigned long long)g_stolen_base,
                (unsigned long long)(g_stolen_size >> 20),
                pci_config_read16(g_dev, 0x04));
    g_ops->readout_log();

    // GRUB's framebuffer must be inside the aperture: the plane's
    // DSPSURF is a GGTT offset, and the aperture is that offset seen
    // from the CPU. A framebuffer anywhere else is a firmware this
    // driver does not understand.
    struct display_surface cur;
    extern void vesafb_get_probe_surface(struct display_surface *out);
    vesafb_get_probe_surface(&cur);
    if (!cur.width || cur.addr < g_aperture || cur.addr >= g_aperture + g_aperture_size) {
        klog_printf("intel-display: GRUB framebuffer %#llx is not in the aperture -- not claimed\n",
                    (unsigned long long)cur.addr);
        return 0;
    }
    uint32_t want_surf = (uint32_t)(cur.addr - g_aperture);
    for (int p = 0; p < 3; p++) {
        if (rd(DSPSURF(p)) != want_surf) continue;
        if (!g_ops->plane_matches(rd(DSPCNTR(p)), rd(DSPSTRIDE(p)), cur.pitch))
            continue;
        g_pipe = p;
        break;
    }
    if (g_pipe < 0) {
        klog_printf("intel-display: no enabled plane scans surf %#x stride %u -- not claimed\n",
                    want_surf, cur.pitch);
        return 0;
    }
    g_fb_ggtt = want_surf;
    uint32_t idx = want_surf >> 12;
    uint64_t pte = idx < g_gtt_entries ? g_gtt[idx] : 0;
    klog_printf("intel-display: pipe %c scans GRUB's %ux%u at ggtt %#x (pte %#llx, expect stolen %#llx)\n",
                'A' + g_pipe, cur.width, cur.height, want_surf,
                (unsigned long long)pte, (unsigned long long)(g_stolen_base + want_surf));

    g_fb_pte_flags = pte & 0xFFF;

    g_surface = cur;
    g_active = 1;
    setup_power_well();
    setup_cursor();
    setup_scanouts();
    g_native_w = g_surface.width;
    g_native_h = g_surface.height;
    if (g_ops->claimed) g_ops->claimed();
    // MODESET and SCALING where the generation can: the native mode is
    // what the firmware lit, the smaller ones are scaled, and the buffer
    // never moves.
    intel_driver.caps |= g_ops->caps;
    return 1;
}

static void intel_get_surface(struct display_surface *out) { *out = g_surface; }

static int intel_read_edid(uint8_t *out, int cap) {
    return g_ops && g_ops->read_edid ? g_ops->read_edid(g_pipe, out, cap) : 0;
}

// Modes: the panel's native size (what the firmware lit, index 0) and
// every ladder entry smaller than it, shown through the panel fitter
// with the native timing kept. The framebuffer never moves: a smaller
// mode is the same buffer at the same stride, scanned w x h.
static int ladder_fits(int index, uint32_t *w, uint32_t *h) {
    int lw, lh;
    if (!display_ladder_mode(index, &lw, &lh)) return 0;
    *w = (uint32_t)lw; *h = (uint32_t)lh;
    return *w < g_native_w && *h <= g_native_h && !(*w == g_native_w && *h == g_native_h);
}

// THE MODE LIST, built once the EDID is known: native first; then the
// REAL modes -- a size the monitor lists whose DMT timing is known,
// set with its own pixel clock (gen9's set_timing); then the ladder's
// sizes no real mode covers, scaled from the native timing. A size
// appears once, real winning over scaled.
#define MODES_MAX 24
struct mode_entry { uint32_t w, h; int real; struct edid_timing t; };
static struct mode_entry g_modes[MODES_MAX];
static int g_nmodes;
static int g_on_native = 1;   // the transcoder runs the native timing (no real mode up)
static int g_cur;             // g_modes index on screen: 0, native, at the claim

static int has_mode(uint32_t w, uint32_t h) {
    for (int i = 0; i < g_nmodes; i++) if (g_modes[i].w == w && g_modes[i].h == h) return 1;
    return 0;
}

static void build_modes(void) {
    if (g_nmodes) return;
    const struct display_edid *e = display_edid();
    g_modes[g_nmodes++] = (struct mode_entry){ g_native_w, g_native_h, 0, { 0 } };
    if (g_ops->set_timing && g_ops->timing_ok && e && e->timing_count &&
        e->timing[0].hactive == g_native_w && e->timing[0].vactive == g_native_h) {
        g_modes[0].real = 1;
        g_modes[0].t = e->timing[0];
        for (int i = 0; i < e->mode_count && g_nmodes < MODES_MAX; i++) {
            struct edid_timing t;
            const struct edid_mode *m = &e->mode[i];
            if (m->w > g_native_w || m->h > g_native_h || has_mode(m->w, m->h)) continue;
            if (!edid_dmt_timing(m->w, m->h, m->hz, &t) || !g_ops->timing_ok(&t)) continue;
            g_modes[g_nmodes++] = (struct mode_entry){ m->w, m->h, 1, t };
        }
    }
    int lw, lh;
    uint32_t w, h;
    for (int i = 0; display_ladder_mode(i, &lw, &lh) && g_nmodes < MODES_MAX; i++)
        if (ladder_fits(i, &w, &h) && !has_mode(w, h))
            g_modes[g_nmodes++] = (struct mode_entry){ w, h, 0, { 0 } };
    // Largest first after native, as the ladder is: the list a person picks from.
    for (int i = 2; i < g_nmodes; i++)
        for (int j = i; j > 1 && g_modes[j].w * g_modes[j].h > g_modes[j - 1].w * g_modes[j - 1].h; j--) {
            struct mode_entry t;
            k_memcpy(&t, &g_modes[j], sizeof t);
            k_memcpy(&g_modes[j], &g_modes[j - 1], sizeof t);
            k_memcpy(&g_modes[j - 1], &t, sizeof t);
        }
    klog_printf("intel-display: %d modes, native %ux%u%s\n", g_nmodes, g_native_w, g_native_h,
                g_modes[0].real ? ", real modes from the EDID" : "");
}

static int intel_mode_count(void) {
    build_modes();
    return g_nmodes;
}

static void intel_mode_at(int index, struct display_mode *out) {
    build_modes();
    if (index < 0 || index >= g_nmodes) index = 0;
    out->width = g_modes[index].w; out->height = g_modes[index].h; out->bpp = 32;
}

// One axis of the fitter's window. THE INVARIANT (the PRM's, checked by
// i915's intel_pch_pfit_check_dst_window): the fitter's output must
// equal the pipe's active area, so panel = 2 * position + size. Rounding
// position and size to even INDEPENDENTLY left a 1366-wide window two
// pixels short, and the fitter walked every line out of step -- a
// skewed screen. So the size is rounded UP to even (as i915 does) and
// the position is exactly half the border, odd if it must be; the one
// forbidden position is 1, which becomes the whole axis instead.
static uint32_t fit_axis(uint32_t f, uint32_t p, uint32_t *pos) {
    if (f > p) f = p;
    if (f < p && ((p - f) & 1)) f++;
    if (p - f == 2) f = p;
    *pos = (p - f) / 2;
    return f;
}

// The fitter's window for a mode on the panel: exact for the limiting
// axis, so a same-aspect mode fills the panel to the pixel.
void intel_display_fit_window(int scaling, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph,
                              uint32_t *x, uint32_t *y, uint32_t *ww, uint32_t *wh) {
    uint32_t fw = pw, fh = ph;
    if (scaling == DISPLAY_SCALING_CENTER) {
        fw = w; fh = h;
    } else if (scaling == DISPLAY_SCALING_ASPECT) {
        if ((uint64_t)w * ph >= (uint64_t)h * pw) { fw = pw; fh = (uint32_t)((uint64_t)h * pw / w); }
        else                                       { fh = ph; fw = (uint32_t)((uint64_t)w * ph / h); }
    }
    *ww = fit_axis(fw, pw, x);
    *wh = fit_axis(fh, ph, y);
}

static void surface_size(uint32_t w, uint32_t h) {
    g_surface.width = w;
    g_surface.height = h;
    for (int b = 0; b < SCANOUTS; b++) { g_scanout[b].width = w; g_scanout[b].height = h; }
}

static int fit_current(uint32_t w, uint32_t h) {
    uint32_t x, y, ww, wh;
    intel_display_fit_window(display_scaling(), w, h, g_native_w, g_native_h, &x, &y, &ww, &wh);
    if (!g_ops->fit || !g_ops->fit(w, h, x, y, ww, wh)) return 0;
    surface_size(w, h);
    return 1;
}

// A real mode re-lights the pipe at its timing; a scaled one (and the
// native one) runs on the native timing, so a real mode is left first.
static int apply_mode(int i) {
    const struct mode_entry *e = &g_modes[i];
    if (e->real && i != 0) {
        if (!g_ops->set_timing(&e->t)) return 0;
        g_on_native = 0;
        surface_size(e->w, e->h);
        return 1;
    }
    if (!g_on_native) {
        if (!g_ops->set_timing(&g_modes[0].t)) return 0;
        g_on_native = 1;
        surface_size(g_native_w, g_native_h);
        if (i == 0) return 1;   // native is shown 1:1 already: no second fit
    }
    return fit_current(e->w, e->h);
}

// ON FAILURE THE PREVIOUS MODE IS PUT BACK: a cycle that stopped part
// way may have left the new timing on the transcoder, and the display
// layer keeps the old size when this returns 0 -- the two must agree.
static int intel_set_mode(const struct display_mode *m) {
    if (!m || m->bpp != 32) return 0;
    build_modes();
    int i = 0;
    while (i < g_nmodes && !(g_modes[i].w == m->width && g_modes[i].h == m->height)) i++;
    if (i == g_nmodes) return 0;
    if (apply_mode(i)) { g_cur = i; return 1; }
    klog_printf(KLOG_ERR "intel-display: %ux%u failed -- putting %ux%u back\n",
                g_modes[i].w, g_modes[i].h, g_modes[g_cur].w, g_modes[g_cur].h);
    if (!apply_mode(g_cur)) klog_printf(KLOG_ERR "intel-display: and that failed too\n");
    return 0;
}

static int intel_set_scaling(int mode) {
    (void)mode;   // read back through display_scaling() by fit_current
    if (!g_on_native) return 1;   // a real mode is shown 1:1: nothing to scale
    if (g_surface.width == g_native_w && g_surface.height == g_native_h) return 1;
    return fit_current(g_surface.width, g_surface.height);
}

// Caps are filled in at claim time: the cursor plane once its buffer
// has a GGTT slot, the backlight once the PWM has a readable period.
static struct display_driver intel_driver = {
    .name = "intel-display",
    .probe = intel_probe,
    .get_surface = intel_get_surface,
    .caps = 0,
    .cursor_define = intel_cursor_define,
    .cursor_move = intel_cursor_move,
    .cursor_show = intel_cursor_show,
    .backlight_get = intel_backlight_get,
    .backlight_set = intel_backlight_set,
    .scanout_count = intel_scanout_count,
    .scanout_at = intel_scanout_at,
    .flip = intel_flip,
    .scanout_live = intel_scanout_live,
    .read_edid = intel_read_edid,
    .mode_count = intel_mode_count,
    .mode_at = intel_mode_at,
    .set_mode = intel_set_mode,
    .set_scaling = intel_set_scaling,
    .ram_bytes = intel_ram_bytes,
};

void intel_display_register(void) {
    display_register(&intel_driver);
}

int intel_display_active(void) { return g_active; }
int intel_display_gen(void) { return g_active ? g_ops->gen : 0; }

// Each generation's own mechanisms, or refused where it has none.
int intel_display_pipe_cycle(void) { return g_active && g_ops->pipe_cycle ? g_ops->pipe_cycle() : 0; }
int intel_display_link_retrain(void) { return g_active && g_ops->link_retrain ? g_ops->link_retrain() : 0; }
int intel_display_native(void) { return g_active && g_ops->native ? g_ops->native() : 0; }

// --- Broadwell's table ---------------------------------------------------

static uint64_t gen8_stolen_bytes(uint32_t gms) { return (uint64_t)gms << 25; }   // 32 MiB units

static int gen8_plane_matches(uint32_t cntr, uint32_t stride, uint32_t pitch) {
    return intel_display_plane_matches(8, cntr, stride, pitch);
}

static void gen8_claimed(void) {
    setup_backlight();
    intel_aux_init();
}

// The eDP panel answers on DDI A's AUX channel, whatever the pipe.
static int gen8_read_edid(int pipe, uint8_t *out, int cap) {
    (void)pipe;
    return intel_aux_read_edid(out, cap);
}

const struct intel_gen_ops intel_gen8_ops = {
    .gen = 8,
    .ids = BDW_IDS,
    .nids = (int)(sizeof BDW_IDS / sizeof BDW_IDS[0]),
    .stolen_bytes = gen8_stolen_bytes,
    .readout_log = log_readout,
    .plane_matches = gen8_plane_matches,
    .scanout_align_pages = 1,   // gen8's layout, packed after the cursor
    .claimed = gen8_claimed,
    .read_edid = gen8_read_edid,
    .caps = DISPLAY_CAP_MODESET | DISPLAY_CAP_SCALING,
    .fit = intel_modeset_fit,
    .pipe_cycle = intel_modeset_pipe_cycle,
    .link_retrain = intel_modeset_link_retrain,
    .native = intel_modeset_native,
};
