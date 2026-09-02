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

// A device's `name` is its stable id -- what `audio_device` persists
// and what a test greps for. Its `label` is what a person reads.
#define SOUND_NAME_MAX 16

struct sound_device {
    const char *name;

    // Human-facing, e.g. "QEMU USB Audio". NULL falls back to `name`,
    // so a driver with nothing better to say need not invent one.
    const char *label;

    // The DRIVER behind it -- "ac97", "usb-audio". sound_register()
    // reports it to `lsdrv`, so a driver that fills this in cannot then
    // forget to say so.
    const char *driver;

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
// its descriptors at. Returns 1, or 0 when the table is full.
//
// SEVERAL DEVICES MAY BE REGISTERED; exactly one is ACTIVE, and only
// the active one is ever started. With no choice made the FIRST device
// discovered is the active one and a later plug does not disturb it; a
// choice (`audio_device`) outranks discovery order and is sticky --
// it survives that card being unplugged and takes effect again when it
// returns. See docs/decisions.md.
int sound_register(const struct sound_device *dev, void *ring, uint64_t ring_phys);

// The device is GONE -- a USB card unplugged. If it was the active
// one, the stream stops and `device_gone` is published to whoever holds
// it (abi/sound_abi.h); another registered device then becomes active
// for the next open. Unknown devices are ignored, so a driver may call
// this unconditionally on teardown.
void sound_unregister(const struct sound_device *dev);

// The registered devices, in registration order, and which of them is
// active. `sound_device_label()` never returns NULL -- it falls back to
// the name. These back `lsdev` and the `audio_device` setting's choice
// list -- which is how the desktop's volume popup gets its rows, since
// a setting's choices already travel to ring 3. Nothing else should be
// enumerating drivers.
int sound_device_count(void);
const char *sound_device_name(int index);
const char *sound_device_label(int index);
int sound_device_is_active(int index);

// The active device's name, or "" when there is none.
const char *sound_active_name(void);

// Choose the output device by name, or "auto" to let the newest
// registration win. Returns 1 when the preference was accepted --
// INCLUDING a name no device currently carries, which is not an error:
// the setting outlives the device, and unplugging a chosen card must
// not silently rewrite the choice. Returns 0 only for a NULL name.
int sound_select(const char *name);

// The current preference -- "auto", or the chosen device's name.
const char *sound_preference(void);

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

// The AC'97 (ac97.c) and Intel HDA (hda.c) drivers are PCI_DRIVERs,
// probed per matching device by pci_bind(); an HDA codec is registered
// only when it has an ANALOG output route -- a display-audio
// controller's HDMI-only codec is claimed and left silent.

// The kernel.hda_tone tunable: three seconds of a kernel-written tone
// through the registered HDA controller, no app and no zeroing -- the
// half of the path a crackle is diagnosed against.
void hda_diag_tone(void);

// The two syscall handlers (rows in syscall_table.c).
struct syscall_ctx;
int sys_snd_open(struct syscall_ctx *c);
int sys_snd_ctl(struct syscall_ctx *c);

#endif
