#ifndef ULIB_UTAGS_H
#define ULIB_UTAGS_H

// utags -- what a sound file says about itself: title, artist, album,
// and an embedded picture. ID3v2 (2.2, 2.3, 2.4) and ID3v1 for MP3; the
// sequence/track name for MIDI; Vorbis comments, PICTURE and the length
// for FLAC. WAV's LIST/INFO chunk is not read.
//
// **IT PARSES UNTRUSTED BYTES**, like ttf.h: every length is checked
// against what is left, and a frame that does not fit, an unknown text
// encoding or an unsynchronised tag is SKIPPED, never guessed at. Text
// comes back as Latin-1 (this system's codeset); a character outside it
// becomes '?'.
//
// The parsers take a buffer, so a test can feed them bytes it built;
// utags_read() is the file-reading wrapper an app calls.
#include <stddef.h>
#include <stdint.h>

#define UTAGS_TEXT_MAX 64

struct utags {
    char title[UTAGS_TEXT_MAX];
    char artist[UTAGS_TEXT_MAX];
    char album[UTAGS_TEXT_MAX];
    uint32_t length_ms;           // ID3 TLEN, 0 when not stated
    // The embedded picture (ID3 APIC/PIC), malloc'd, or NULL. Only when
    // utags_read() was asked for it; utags_free() releases it.
    uint8_t *art;
    size_t art_len;
};

// Which fields a call found: a bit per field, 0 when nothing.
enum { UTAGS_TITLE = 1, UTAGS_ARTIST = 2, UTAGS_ALBUM = 4, UTAGS_ART = 8, UTAGS_LENGTH = 16 };

// The ID3v2 tag at the start of `buf` (the whole tag must be there; its
// header says how long it is -- utags_id3_size()). `want_art` copies
// the picture out.
int utags_from_id3v2(const uint8_t *buf, size_t len, struct utags *t, int want_art);
// The 128-byte ID3v1 trailer -- only fills fields still empty.
int utags_from_id3v1(const uint8_t *trailer128, struct utags *t);
// A FLAC file's metadata blocks, from its "fLaC" marker. `want_art`
// copies the PICTURE out.
int utags_from_flac(const uint8_t *buf, size_t len, struct utags *t, int want_art);
// A Standard MIDI File: the first track's sequence/track name (meta 03).
int utags_from_midi(const uint8_t *buf, size_t len, struct utags *t);

// The whole ID3v2 tag's size from its 10-byte header, footer included,
// or 0 when `head` does not start one.
size_t utags_id3_size(const uint8_t *head, size_t len);

// Zeroes `t`, reads `path` and fills what it finds. Returns the UTAGS_*
// bits found (0: nothing, or not a format this reads).
int utags_read(const char *path, struct utags *t, int want_art);
void utags_free(struct utags *t);

#endif
