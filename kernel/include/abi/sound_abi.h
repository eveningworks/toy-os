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
// --- WHAT A CARD CAN DO, as opposed to what this stack asks of it ----
//
// SND_RATE and SND_CHANNELS below are what the shared ring IS -- one
// format the whole stack is compiled around. These masks are a
// different thing: what the HARDWARE reports it could do, read from
// the card and reported unchanged.
//
// THEY DRIVE NOTHING YET, deliberately. Knowing what the cards support
// is the fact that decides whether a per-device format is worth
// building, and this project reads the fact before acting on it -- the
// EDID readout took the same shape. A mask of 0 means the driver does
// not report; it does not mean "nothing".
//
// A COMMON ENCODING, not the hardware's. HDA has its own bit layout
// (PARAM_PCM_SUPPORT), AC97 its own notion of variable rate, USB audio
// a list of discrete rates in its descriptors -- so each driver
// translates into these, and a reader compares cards without knowing
// which bus it is looking at.
#define SND_RATE_8000   (1u << 0)
#define SND_RATE_11025  (1u << 1)
#define SND_RATE_16000  (1u << 2)
#define SND_RATE_22050  (1u << 3)
#define SND_RATE_32000  (1u << 4)
#define SND_RATE_44100  (1u << 5)
#define SND_RATE_48000  (1u << 6)
#define SND_RATE_88200  (1u << 7)
#define SND_RATE_96000  (1u << 8)
#define SND_RATE_176400 (1u << 9)
#define SND_RATE_192000 (1u << 10)

#define SND_DEPTH_8   (1u << 0)
#define SND_DEPTH_16  (1u << 1)
#define SND_DEPTH_20  (1u << 2)
#define SND_DEPTH_24  (1u << 3)
#define SND_DEPTH_32  (1u << 4)

#define SND_RATE      48000
#define SND_CHANNELS  2
#define SND_FRAME_BYTES 4 // 2 channels x 16-bit

// The ring: 32 chunks of 2 KiB = 64 KiB, ~341 ms. A chunk is one
// completion interrupt (~47/s) and one hardware descriptor.
#define SND_CHUNKS      32
#define SND_CHUNK_BYTES 2048
#define SND_RING_BYTES  (SND_CHUNKS * SND_CHUNK_BYTES)

// Where SYS_SND_OPEN maps the control page (the ring follows on the
// next page) -- fixed, like WIN_FB_VADDR, so an
// app computes addresses instead of being told them.
#define SND_MAP_VADDR 0x8F00000000ULL

#define SND_CTL_MAGIC 0x534e4431 // "SND1"

// A client's application name, and deliberately PROC_NAME_MAX
// (abi/proc_info.h) rather than a size of this ABI's own: it is filled
// from the process's own name, and two different limits for one string
// is a truncation nobody would predict.
#define SND_APP_MAX 24

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
    // HOW FAR THE WRITER HAS GOT -- a DAEMON CLIENT RING only; the
    // kernel's own stream never reads or writes it and leaves it 0.
    //
    // The hardware does not need this because it is free to run ahead of
    // a starved writer: the kernel zeroes what it consumes, so overrun
    // plays silence. A DAEMON must not, and the difference is that its
    // client uses `hw_pos` to work out where it may write -- a consumer
    // that overtakes the producer eventually parks exactly one chunk
    // ahead of it, which reads as "no room" forever and stalls the
    // client's decoder rather than merely playing quiet.
    uint32_t wr_pos;
    // WHO IS PLAYING, for a per-application volume. A DAEMON CLIENT
    // RING only; the kernel's own stream leaves it "".
    //
    // The ring's NAME is `snd.<pid>` and a pid is not an application:
    // it means nothing next boot, so a volume keyed on it could not be
    // remembered. This is the stable key -- the process's own name,
    // which is why it is PROC_NAME_MAX and not a size of its own.
    // PulseAudio keys application.name and the Windows mixer keys the
    // executable for the same reason.
    //
    // Written by the client BEFORE `magic`, so the daemon never reads a
    // half-filled one; empty is legal and mixes at full gain.
    char app[SND_APP_MAX];
};

// --- the sound daemon's client rings ---------------------------------
//
// A client of /bin/soundd writes into a ring of EXACTLY THIS SHAPE, in
// a shared-memory object it creates (SYS_SHM_OPEN) and the daemon maps.
// One control page then the samples, the same struct above, and the
// same refill rule -- so a sink written against the kernel's stream
// works against the daemon's by changing where the pointers come from.
//
// THE DAEMON PLAYS THE ROLE THE HARDWARE PLAYS: it advances `hw_pos`
// as it consumes, and ZEROES each chunk before moving past it. That is
// what makes an abandoned ring go quiet instead of looping, exactly as
// it does for the kernel -- and it is why a client that dies needs no
// cleanup path in the daemon at all.
//
// `running` is the CLIENT saying it wants to be mixed; `device_gone` is
// the daemon saying it is going away, so a client can fall back to the
// kernel stream rather than writing into a ring nobody reads.
#define SND_CLIENT_BYTES (4096 + SND_RING_BYTES)

// The daemon's presence beacon: an object of this name exists exactly
// while a daemon is running, which is how a client chooses a sink
// without a connect() to fail.
#define SND_SERVER_NAME "snd.server"

// AND ITS CONTENT IS THE ROSTER -- who is being mixed right now, and at
// what gain. The beacon was a 4 KiB object with nothing in it; a mixer
// UI needs exactly this list, and publishing it here costs no new IPC
// and no second object. THE DAEMON IS THE ONLY WRITER and the page is
// mapped read-only by everyone else, which is what keeps a UI from
// reaching into a client's own ring to find out who it is.
//
// Torn reads are possible and deliberately tolerated: `gen` is bumped
// before and after a rewrite, so a reader that sees an ODD value (or a
// different one either side) looks again rather than locking. A volume
// slider redrawing one frame late is not worth a lock in a page a
// dying daemon can leave behind.
#define SND_ROSTER_MAX 8

struct snd_roster_entry {
    char     app[SND_APP_MAX]; // "" when the client never said
    int32_t  pid;              // from the ring's name, so a UI can
                               // tell two copies of one program apart
    uint32_t gain;             // 0..100, what the daemon is applying
    uint32_t playing;          // 1 while it is actually feeding samples
};

struct snd_roster {
    uint32_t magic;            // SND_CTL_MAGIC, so a reader can check
    uint32_t gen;              // odd while being written; see above
    uint32_t count;
    uint32_t reserved;
    struct snd_roster_entry e[SND_ROSTER_MAX];
};

// --- a sound device implemented by a PROCESS --------------------------
//
// docs/umdf-design.md's end state: the DRIVER is a ring-3 program and
// `soundd` mixes on top of it, unchanged. The kernel's sound core still
// owns the ring, the exclusivity and the consumed-chunk zeroing -- what
// moves out is the code that talks to the card.
//
// THE CORE CANNOT CALL A PROCESS, so `start`/`stop`/`set_volume` become
// a REQUEST the driver reads: the kernel writes the op, bumps `seq` and
// wakes the driver's wakeword (SYS_WAKEWORD), and the driver acts and
// answers in `running`. That makes `start()` ASYNCHRONOUS, which the
// core already tolerates -- it publishes `running` and an app watches
// it, rather than assuming the engine is live the moment it asked.
//
// The page is the DRIVER's, created and mapped by it (an shm object,
// as every other cross-process page here is) and handed over at
// registration. The kernel writes only the request half.
#define SND_DRV_MAGIC 0x53445256u  // 'SDRV'

#define SND_REQ_NONE   0
#define SND_REQ_START  1  // begin at the ring's first chunk
#define SND_REQ_STOP   2  // halt the engine; the ring stays
#define SND_REQ_VOLUME 3  // `volume` holds 0..100

struct snd_driver_page {
    uint32_t magic;    // SND_DRV_MAGIC, written by the driver LAST
    // BUMPED ON EVERY REQUEST, not just changed: two STARTs in a row
    // are two requests, and a driver comparing only `op` would see the
    // second as nothing new.
    uint32_t seq;
    uint32_t op;       // SND_REQ_*
    uint32_t volume;   // 0..100, meaningful for SND_REQ_VOLUME
    // The driver's answer, and what the core publishes to apps. A
    // driver that cannot start says 0 here and the app sees the stream
    // never came up, rather than silence with everything looking fine.
    uint32_t running;
    uint32_t reserved;
};

// What SYS_SND_REGISTER is handed. `ring_phys` comes back in it: the
// core owns the ring, and a driver needs its PHYSICAL address to point
// a descriptor at. It never needs to READ the samples -- soundd writes
// them and the card fetches them -- so no mapping is granted.
#define SND_DRV_NAME_MAX  16
#define SND_DRV_LABEL_MAX 40

struct snd_register_msg {
    char     name[SND_DRV_NAME_MAX];    // "hda-ring3"
    char     label[SND_DRV_LABEL_MAX];  // what a person sees
    uint32_t rates, depths;             // SND_RATE_*/SND_DEPTH_*
    uint64_t page;                      // the driver's snd_driver_page
    uint64_t ring_phys;                 // OUT: where the ring is
};

// Per-application gains, by application name. A file of its own rather
// than keys in /etc/toyos.conf: these accumulate one per program ever
// played, which does not belong in the system's own config, and the
// settings REGISTRY cannot hold them at all (it is a fixed catalogue of
// build-time knobs, one System Settings row each).
#define SND_CONFIG_FILE "/etc/sound.conf"

// A client's own ring is "snd." plus its pid -- unique without a
// registry, and the pid is what QUERY_SHM already reports, so the
// daemon can tell a live client from a stale name.
#define SND_CLIENT_PREFIX "snd."

// SYS_SND_CTL ops (RDI).
#define SND_CTL_START 1 // begin playback from the ring's start
#define SND_CTL_STOP  2 // stop the engine; the ring stays mapped
#define SND_CTL_CLOSE 3 // stop, unmap, release the (exclusive) stream

#endif
