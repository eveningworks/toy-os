#ifndef ABI_SOUND_ABI_H
#define ABI_SOUND_ABI_H

#include <stdint.h>

// The PCM stream's shared-memory ABI -- what SYS_SND_OPEN maps at
// UADDR_SND_BASE (kernel/uaddr.h): one CONTROL page, then the sample
// ring. ALSA's mmap mode in miniature: the app writes samples AHEAD of
// the hardware's read position, the kernel writes that position back
// here on every completion interrupt, and steady-state playback costs
// ZERO syscalls.
//
// **A CHUNK THE HARDWARE FINISHED IS ZEROED BY THE KERNEL** before
// `hw_pos` moves past it. That one rule is what makes the ring safe to
// abandon: the device loops forever once started, so without it a
// stalled (or dead) app would loop its last second of audio -- with it,
// an underrun plays silence (zero IS silence in signed PCM), which is
// also why there is no underrun error path to get wrong.
//
// The app's contract: keep its own write cursor, write only between
// `hw_pos + SND_CHUNK_BYTES` (the chunk being played is the hardware's)
// and `hw_pos + SND_RING_BYTES` (all the way around), and never touch
// the control page except to read it.

// 16-bit signed little-endian stereo at 48 kHz -- AC'97's native
// format, and the one every card this registry will hold can do.
// Rate/format negotiation is deliberately NOT in the v1 ABI: the field
// exists so a reader can check it, not so a caller can choose.
#define SND_RATE      48000
#define SND_CHANNELS  2
#define SND_FRAME_BYTES 4 // 2 channels x 16-bit

// The ring: 32 chunks of 2 KiB = 64 KiB, ~341 ms. A chunk is one
// completion interrupt (~47/s) and one hardware descriptor.
#define SND_CHUNKS      32
#define SND_CHUNK_BYTES 2048
#define SND_RING_BYTES  (SND_CHUNKS * SND_CHUNK_BYTES)

// Where SYS_SND_OPEN maps the control page (the ring follows on the
// next page) -- fixed, like GUI_FB_VADDR and the window buffers, so an
// app computes addresses instead of being told them.
#define SND_MAP_VADDR 0x8F00000000ULL

#define SND_CTL_MAGIC 0x534e4431 // "SND1"

struct snd_ctl_page {
    uint32_t magic;      // SND_CTL_MAGIC -- an app can sanity-check the map
    uint32_t rate;       // SND_RATE
    uint32_t channels;   // SND_CHANNELS
    uint32_t ring_bytes; // SND_RING_BYTES
    // Byte offset INTO THE RING of the chunk the hardware is playing
    // now. Written by the kernel on every completion interrupt; only
    // ever a multiple of SND_CHUNK_BYTES. Volatile to the app.
    uint32_t hw_pos;
    uint32_t running;    // 1 between SND_CTL_START and _STOP
    // THE DEVICE WENT AWAY UNDER THE STREAM -- a USB card unplugged
    // mid-playback. `running` drops to 0 at the same moment, so an app
    // that only watches that degrades to the stalled case (silence);
    // this is how it tells "somebody stopped me" from "the hardware is
    // gone", which is the difference between resuming and reopening.
    // Cleared by the next SYS_SND_OPEN.
    uint32_t device_gone;
};

// SYS_SND_CTL ops (RDI).
#define SND_CTL_START 1 // begin playback from the ring's start
#define SND_CTL_STOP  2 // stop the engine; the ring stays mapped
#define SND_CTL_CLOSE 3 // stop, unmap, release the (exclusive) stream

#endif
