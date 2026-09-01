// See speaker.h. PIT channel 2 (ports 0x42 data / 0x43 command) drives
// the speaker's tone frequency, the same square-wave mode channel 0
// already uses for the system timer (kernel/core/timer.c's pit_init())
// -- just a different channel, so this doesn't touch channel 0's own
// 100Hz tick at all. Port 0x61 bits 0-1 gate the PIT's channel-2 output
// through to the physical speaker (bit 0) and enable the speaker data
// line (bit 1); both have to be set for a tone to actually sound, and
// both get cleared again afterward rather than left however they were
// found, so a beep never leaves the speaker stuck on if something else
// touches that port later.
#include "speaker.h"
#include "io.h"
#include "timer.h"

// driver-none: the PC speaker: a beep, on no device class

#define PIT_CHANNEL2 0x42
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQ 1193182
#define SPEAKER_PORT 0x61

// Divisor is a 16-bit PIT register (kernel/core/timer.c's channel-0
// init has the same constraint) -- freq_hz below PIT_BASE_FREQ/65536
// (~18Hz) would need a divisor that doesn't fit, and 0 would divide by
// zero, so both ends of the audible-ish range are clamped rather than
// trusted blindly.
#define SPEAKER_MIN_HZ 20
#define SPEAKER_MAX_HZ 20000

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz < SPEAKER_MIN_HZ) freq_hz = SPEAKER_MIN_HZ;
    if (freq_hz > SPEAKER_MAX_HZ) freq_hz = SPEAKER_MAX_HZ;

    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    outb(PIT_COMMAND, 0xB6); // channel 2, lobyte/hibyte, mode 3 (square wave)
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t prior = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, prior | 0x03); // gate channel 2 through + enable speaker data

    // pit_ticks() is channel 0's 100Hz counter (kernel/core/timer.c),
    // i.e. 10ms resolution -- the same busy-wait-until-deadline idiom
    // used everywhere else in this codebase that needs to wait without
    // a real sleep primitive (see speaker.h's top comment).
    uint64_t deadline = pit_ticks() + (duration_ms + 9) / 10; // round up, not down
    while (pit_ticks() < deadline) { }

    outb(SPEAKER_PORT, prior); // restore exactly what was there before, not just clear bits 0-1
}
