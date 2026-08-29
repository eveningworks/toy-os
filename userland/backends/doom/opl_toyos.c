// The OPL driver toy-os supplies to Chocolate Doom's OPL library.
//
// `opl/opl.c` dispatches every register write through an `opl_driver_t`
// so the same code can drive a real Yamaha chip or an emulator. Its
// driver list names `opl_sdl_driver` unconditionally, so THAT IS THE
// SYMBOL THIS FILE EXPORTS -- a link-time substitution, which is the
// only way to plug in without editing a vendored file (see that
// directory's README). The struct's own `name` says `toyos`, so
// anything that prints it tells the truth; only the C symbol is
// borrowed.
//
// The algorithm is `opl_sdl.c`'s, and deliberately so: render up to the
// next scheduled callback, emit that many samples from the emulator,
// advance the clock by exactly that many, then run whatever came due.
// **THE CLOCK IS SAMPLES, NOT WALL TIME.** That is what keeps the music
// in tempo on an emulator that runs at whatever speed it likes -- a
// timer-driven version would drift with the host's load, which is the
// whole class of bug TCG makes unavoidable here.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"
#include "dbopl.h"
#include "lib/usnd.h"
#include "rt/sys.h"
#include "ui/ulog.h"

#define MIXING_FREQ  USND_RATE

// One render pass. Bounded so the mono scratch below is a fixed size
// and `Chip__GenerateBlock2` is never asked for more than it holds.
#define RENDER_MAX   1024

static Chip g_chip;
static opl_callback_queue_t *g_queue;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cblock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t g_render;
static int g_render_on;
static int g_quit;

static uint64_t g_now;          // microseconds of emulated time
static uint64_t g_pause_offset; // time that passed while paused
static int g_paused;
static int g_started;
static unsigned g_reg;          // the register the last port write selected

// dbopl emits MONO 32-bit; the sink wants interleaved stereo s16.
static Bit32s g_mono[RENDER_MAX];
static int16_t g_stereo[RENDER_MAX * 2];

// The two OPL timers, emulated only as far as OPL_ReadPort needs: Doom
// polls the status byte to detect the chip, and gets the right answer.
struct opl_timer {
    unsigned value, enabled, rate;
    uint64_t expire;
};
static struct opl_timer g_timer1 = { 0, 0, 12500, 0 };
static struct opl_timer g_timer2 = { 0, 0, 3125, 0 };

static void timer_end_time(struct opl_timer *t) {
    if (!t->enabled) return;
    unsigned tics = 0x100 - t->value;
    t->expire = g_now + ((uint64_t)tics * OPL_SECOND) / t->rate;
}

// --- the driver ops ---------------------------------------------------

long opl_toyos_render(int16_t *dst, long frames);

// **THE RENDER THREAD IS NOT AN OPTIMISATION -- INIT DEADLOCKS WITHOUT
// IT.** `opl.c`'s InitDriver calls OPL_Detect(), which calls
// OPL_Delay(), which schedules a callback and BLOCKS on a condition
// variable until it fires -- and callbacks only fire from the render
// path. SDL got away with pumping from Doom's own loop because its
// audio thread was already running by then; here nothing is, so the
// clock has to be turning before init_func returns.
//
// It also decouples the music's tempo from Doom's frame rate, which is
// worth having on an emulator whose speed varies.
static void *render_main(void *arg) {
    (void)arg;
    while (!g_quit) {
        long space = usnd_push_space();
        if (space <= 0) { sys_sleep_ms(5); continue; }
        if (space > RENDER_MAX) space = RENDER_MAX;
        long got = opl_toyos_render(g_stereo, space);
        if (got > 0) usnd_push(g_stereo, got);
        else sys_sleep_ms(5);
    }
    return 0;
}

static int toyos_init(unsigned port_base) {
    (void)port_base;
    if (usnd_init() != 0) return 0;          // idempotent; -EBUSY/-ENODEV are ordinary
    if (usnd_push_open() != 0) return 0;

    DBOPL_InitTables();
    Chip__Chip(&g_chip);
    Chip__Setup(&g_chip, MIXING_FREQ);
    g_queue = OPL_Queue_Create();
    if (!g_queue) { usnd_push_close(); return 0; }
    g_now = g_pause_offset = 0;
    g_paused = 0;
    g_quit = 0;
    g_started = 1;

    if (pthread_create(&g_render, 0, render_main, 0) != 0) {
        g_started = 0;
        OPL_Queue_Destroy(g_queue); g_queue = 0;
        usnd_push_close();
        ulogf("doom: could not start the OPL render thread\n");
        return 0;
    }
    g_render_on = 1;
    return 1;
}

static void toyos_shutdown(void) {
    if (!g_started) return;
    g_quit = 1;
    if (g_render_on) { pthread_join(g_render, 0); g_render_on = 0; }
    g_started = 0;
    if (g_queue) { OPL_Queue_Destroy(g_queue); g_queue = 0; }
    usnd_push_close();
}

static unsigned toyos_read_port(opl_port_t port) {
    (void)port;
    unsigned result = 0;
    if (g_timer1.enabled && g_now > g_timer1.expire) result |= 0x80 | 0x40;
    if (g_timer2.enabled && g_now > g_timer2.expire) result |= 0x80 | 0x20;
    return result;
}

static void write_register(unsigned reg, unsigned value) {
    switch (reg) {
    case OPL_REG_TIMER1:
        g_timer1.value = value;
        timer_end_time(&g_timer1);
        break;
    case OPL_REG_TIMER2:
        g_timer2.value = value;
        timer_end_time(&g_timer2);
        break;
    case OPL_REG_TIMER_CTRL:
        if (value & 0x80) {
            g_timer1.enabled = g_timer2.enabled = 0;
        } else {
            if ((value & 0x40) == 0) {
                g_timer1.enabled = (value & 0x01) != 0;
                timer_end_time(&g_timer1);
            }
            if ((value & 0x20) == 0) {
                g_timer2.enabled = (value & 0x02) != 0;
                timer_end_time(&g_timer2);
            }
        }
        break;
    default:
        Chip__WriteReg(&g_chip, reg, (Bit8u)value);
        break;
    }
}

static void toyos_write_port(opl_port_t port, unsigned value) {
    if (port == OPL_REGISTER_PORT)  g_reg = value;
    else if (port == OPL_DATA_PORT) write_register(g_reg, value);
}

static void toyos_set_callback(uint64_t us, opl_callback_t cb, void *data) {
    pthread_mutex_lock(&g_qlock);
    if (g_queue) OPL_Queue_Push(g_queue, cb, data, g_now - g_pause_offset + us);
    pthread_mutex_unlock(&g_qlock);
}

static void toyos_clear_callbacks(void) {
    pthread_mutex_lock(&g_qlock);
    if (g_queue) OPL_Queue_Clear(g_queue);
    pthread_mutex_unlock(&g_qlock);
}

// OPL_Lock/Unlock are how the game thread stops a callback running
// while it changes the music's state. Held across the callback below.
static void toyos_lock(void)   { pthread_mutex_lock(&g_cblock); }
static void toyos_unlock(void) { pthread_mutex_unlock(&g_cblock); }

static void toyos_set_paused(int paused) { g_paused = paused; }

static void toyos_adjust_callbacks(float factor) {
    pthread_mutex_lock(&g_qlock);
    if (g_queue) OPL_Queue_AdjustCallbacks(g_queue, g_now, factor);
    pthread_mutex_unlock(&g_qlock);
}

// THE SYMBOL opl.c's DRIVER LIST NAMES. See the file header.
opl_driver_t opl_sdl_driver = {
    "toyos",
    toyos_init,
    toyos_shutdown,
    toyos_read_port,
    toyos_write_port,
    toyos_set_callback,
    toyos_clear_callbacks,
    toyos_lock,
    toyos_unlock,
    toyos_set_paused,
    toyos_adjust_callbacks,
};

// --- rendering --------------------------------------------------------

// Advance the clock by `n` samples and run everything that came due.
// The queue lock is RELEASED around each callback, because a callback
// schedules the next one and would otherwise deadlock on it -- while
// the separate callback lock is held, which is what OPL_Lock() blocks.
static void advance_time(unsigned n) {
    pthread_mutex_lock(&g_qlock);
    uint64_t us = ((uint64_t)n * OPL_SECOND) / MIXING_FREQ;
    g_now += us;
    if (g_paused) g_pause_offset += us;

    while (g_queue && !OPL_Queue_IsEmpty(g_queue) &&
           g_now >= OPL_Queue_Peek(g_queue) + g_pause_offset) {
        opl_callback_t cb;
        void *data;
        if (!OPL_Queue_Pop(g_queue, &cb, &data)) break;
        pthread_mutex_unlock(&g_qlock);
        pthread_mutex_lock(&g_cblock);
        cb(data);
        pthread_mutex_unlock(&g_cblock);
        pthread_mutex_lock(&g_qlock);
    }
    pthread_mutex_unlock(&g_qlock);
}

// Renders `frames` frames of interleaved stereo s16. dbopl is MONO, so
// each sample is written to both channels -- an OPL2 had no stereo.
long opl_toyos_render(int16_t *dst, long frames) {
    if (!g_started) return 0;
    long filled = 0;

    while (filled < frames) {
        uint64_t n;
        pthread_mutex_lock(&g_qlock);
        if (g_paused || !g_queue || OPL_Queue_IsEmpty(g_queue)) {
            n = (uint64_t)(frames - filled);
        } else {
            // Only up to the next event: rendering past it would play
            // the note change late by however far past it we went.
            uint64_t next = OPL_Queue_Peek(g_queue) + g_pause_offset;
            n = (next > g_now ? next - g_now : 0) * MIXING_FREQ;
            n = (n + OPL_SECOND - 1) / OPL_SECOND;
            if (n > (uint64_t)(frames - filled)) n = (uint64_t)(frames - filled);
        }
        pthread_mutex_unlock(&g_qlock);

        if (n > RENDER_MAX) n = RENDER_MAX;
        if (n > 0) {
            // UNDER THE CALLBACK LOCK, which is what OPL_Lock() takes.
            // The game thread writes registers through it (i_oplmusic's
            // StopSong keys off every voice that way), and generating
            // outside it would let that land in the middle of a block.
            // advance_time() takes the same lock only around callbacks,
            // and never while this is held, so there is no nesting.
            pthread_mutex_lock(&g_cblock);
            Chip__GenerateBlock2(&g_chip, (Bitu)n, g_mono);
            pthread_mutex_unlock(&g_cblock);
            for (uint64_t i = 0; i < n; i++) {
                int32_t v = g_mono[i];
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                dst[(filled + (long)i) * 2]     = (int16_t)v;
                dst[(filled + (long)i) * 2 + 1] = (int16_t)v;
            }
            filled += (long)n;
        }
        // ALWAYS advances, even when n is 0 -- a callback due at exactly
        // now yields zero samples, and returning here instead would spin
        // forever without ever running it.
        advance_time(n ? (unsigned)n : 1);
    }
    return filled;
}

int opl_toyos_started(void) { return g_started; }
