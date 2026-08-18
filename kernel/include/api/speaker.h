#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

// Drives the PC speaker via PIT channel 2 + port 0x61's gate/data bits
// (Milestone 25, docs/roadmap.md: "the simplest possible output").
// Blocks for duration_ms, busy-waiting on pit_ticks() -- there's no
// scheduler-aware sleep/delay primitive in this kernel yet (see
// docs/roadmap.md's Milestone 24 item), so this can't yield to
// anything else while the tone plays, same as every other "wait a
// while" spot in this codebase (kernel/drivers/ata.c's DMA wait,
// userland/wm/start_menu.c's flash timing, etc -- all the same
// capture-pit_ticks()-then-poll idiom). freq_hz is clamped to a sane
// audible range; see speaker.c for the exact bounds.
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

#endif
