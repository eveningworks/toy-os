// tone -- plays two seconds of A440 through the PCM stream, end to
// end: SYS_SND_OPEN's shared ring (abi/sound_abi.h), sine samples from
// the kernel's own fixed-point trig (fixed.h, compiled into ring 3),
// steady-state refills against hw_pos with zero syscalls, then a clean
// stop. tools/audio_test.py is the judge: QEMU's wav audiodev records
// what the DEVICE played to a host file, and the harness measures the
// frequency there -- an oracle that shares no code with any of this.
//
// The refill rule, from the ABI: the chunk at hw_pos is the hardware's;
// everything from the writer's cursor up to one chunk BEHIND hw_pos is
// the app's to (re)fill. The kernel zeroes consumed chunks, so a late
// refill plays silence, never a loop.
#include "rt/sys.h"
#include "sound_abi.h"
#include "fixed.h"
#include <stdint.h>
#include "tmppath.h"
#include "lib/utmppath.h"

#define TONE_HZ     440
#define TONE_FRAMES (2 * SND_RATE) // two seconds
#define AMPLITUDE   12000

static volatile struct snd_ctl_page *g_ctl;
static volatile int32_t *g_ring;   // the s32 ring (abi/sound_abi.h)

static uint32_t g_wr;      // byte cursor into the ring
static uint32_t g_frame;   // total frames generated, for phase

static void put_frame(void) {
    // Angle in TURNS (fixed.h): the cycle position of frame n at 440Hz.
    fx_t angle = (fx_t)(((uint64_t)g_frame * TONE_HZ % SND_RATE) * FX_ONE / SND_RATE);
    // Built as s16 and placed in the TOP half: the captured 16-bit
    // device output is then exactly this AMPLITUDE, which is what the
    // host's check reads.
    int32_t s = (int32_t)(int16_t)((fx_sin(angle) * AMPLITUDE) >> FX_SHIFT) * 65536;
    g_ring[g_wr / 4] = s;
    g_ring[g_wr / 4 + 1] = s;
    g_wr = (g_wr + SND_FRAME_BYTES) % SND_RING_BYTES;
    g_frame++;
}

// Fill from the cursor up to one chunk behind where the hardware is.
static void fill_to(uint32_t hw_pos) {
    uint32_t limit = (hw_pos + SND_RING_BYTES - SND_CHUNK_BYTES) % SND_RING_BYTES;
    while (g_wr != limit && g_frame < TONE_FRAMES) put_frame();
}

int main(void) {
    if (sys_snd_open() != 0) {
        // No hardware is a SKIP, not a failure -- the default boot has
        // no AC97 attached, and tools/audio_test.py is what boots one.
        sys_print(sys_errno() == ENODEV ? "tone: no sound device (skip)\n"
                                         : "tone: open failed\n");
        return sys_errno() == ENODEV ? 0 : 1;
    }
    g_ctl = (volatile struct snd_ctl_page *)(uintptr_t)SND_MAP_VADDR;
    g_ring = (volatile int32_t *)(uintptr_t)(SND_MAP_VADDR + 4096);
    if (g_ctl->magic != SND_CTL_MAGIC || g_ctl->rate != SND_RATE) {
        sys_print("tone: bad control page\n");
        return 1;
    }

    fill_to(0);            // prime the whole ring (minus the guard chunk)
    if (sys_snd_ctl(SND_CTL_START) != 0) {
        sys_print("tone: start failed\n");
        return 1;
    }
    while (g_frame < TONE_FRAMES) {
        fill_to(g_ctl->hw_pos);
        sys_sleep_ms(20);
    }
    // Let the ring drain (one lap is ~341ms) so the tail the host
    // records is the kernel's zeroed silence, not a cut.
    sys_sleep_ms(400);
    sys_snd_ctl(SND_CTL_STOP);
    sys_snd_ctl(SND_CTL_CLOSE);
    // THROUGH THE FILESYSTEM as well as the console: a spawned child's
    // console lines reach the serial capture unreliably, and bytes on
    // disk do not race anything (the keymap tests' idiom).
    int fd = sys_open(utest_path(TMP_VOLATILE, "tone_done"), SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd >= 0) {
        static const char msg[] = "played 440Hz for 2s\n";
        sys_write(fd, msg, sizeof msg - 1);
        sys_close(fd);
    }
    sys_print("tone: played 440Hz for 2s\n");
    return 0;
}
