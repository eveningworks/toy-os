#ifndef KERNEL_SOUND_H
#define KERNEL_SOUND_H

#include <stdint.h>

// The sound-device class -- one PCM playback stream, N drivers, the
// same registry shape display_driver, block_device and clocksource
// use. The CORE (sound.c) owns the stream's policy: who has it open
// (EXCLUSIVE -- a second open is refused, and mixing stays out of the
// kernel the way every modern OS keeps it out), the shared ring's
// mapping into the owner, and zeroing consumed chunks. A DRIVER owns
// only its hardware: descriptors, the run bit, and the interrupt that
// reports progress.
//
// The ring the driver plays IS the ring the app writes (abi/
// sound_abi.h): the core hands the driver one physically-contiguous
// buffer at init and the driver points its descriptors into it --
// zero copies anywhere. A later card that cannot scatter-gather over
// one buffer copies in ITS half; the ABI does not move.

struct sound_device {
    const char *name;

    // Start/stop the engine over the ring `sound_register()` supplied.
    // start() begins at the ring's first chunk.
    int  (*start)(void);
    void (*stop)(void);

    // 0..100 into whatever the hardware's volume is. Optional (NULL).
    void (*set_volume)(int pct);
};

// A driver that found its hardware registers here, handing the core
// nothing -- the core hands IT the ring: `ring` is SND_RING_BYTES of
// physically contiguous, kernel-mapped memory the driver must point
// its descriptors at. First registration wins (one active device).
// Returns 1, or 0 when a device already holds the slot.
int sound_register(const struct sound_device *dev, void *ring, uint64_t ring_phys);

// The driver's completion interrupt calls this with the byte offset of
// the chunk the hardware is NOW playing. The core zeroes everything
// between the last reported position and this one (consumed samples
// must not loop -- abi/sound_abi.h) and publishes it to the app.
void sound_period_done(uint32_t hw_pos);

// Ask sound.c for the ring to register with -- allocated on first use,
// so a machine with no sound hardware never spends the frames.
void *sound_ring_alloc(uint64_t *out_phys);

// Is any device registered? (`lsdev`-style reporting, and the KTESTs'
// skip condition.)
int sound_present(void);

// The registered device's volume knob, 0..100 -- the audio.volume
// setting's apply path. A no-op with no device or no set_volume op.
void sound_set_volume(int pct);

// A dying process's cleanup (release_process_state): if it held the
// stream, stop the engine and release it. Keyed by pml4 like every
// other per-process release there.
void sound_process_gone(uint64_t pml4_phys);

// The AC'97 driver's boot probe (kernel/drivers/sound/ac97.c), called
// from kernel_main() beside the other PCI-scanning drivers. Finding no
// controller is the common case, not an error.
void ac97_init(void);

// The two syscall handlers (rows in syscall_table.c).
struct syscall_ctx;
int sys_snd_open(struct syscall_ctx *c);
int sys_snd_ctl(struct syscall_ctx *c);

#endif
