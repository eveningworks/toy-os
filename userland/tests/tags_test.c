// lib/utags.h: ID3v2 (2.2/2.3/2.4), ID3v1 and the MIDI track name --
// on tags built here byte by byte, then on the music the image ships.
//
// THE BUILT TAGS ARE THE POINT: each one exercises one rule (a UTF-16
// title, a 2.4 syncsafe frame size, a frame that overruns its tag) that
// the shipped files, written by one tool, never would.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lib/utest.h"
#include "lib/utags.h"

static uint8_t buf[512];
static size_t n;

// A tag header for version `ver`; the size is patched by finish().
static void begin(int ver) {
    memset(buf, 0, sizeof buf);
    memcpy(buf, "ID3", 3);
    buf[3] = (uint8_t)ver;
    n = 10;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void syncsafe(uint8_t *p, uint32_t v) {
    p[0] = (v >> 21) & 0x7F; p[1] = (v >> 14) & 0x7F; p[2] = (v >> 7) & 0x7F; p[3] = v & 0x7F;
}

// A 2.3 (plain size) or 2.4 (syncsafe size) frame.
static void frame(int ver, const char *id, const void *body, size_t len) {
    memcpy(buf + n, id, 4);
    if (ver == 4) syncsafe(buf + n + 4, (uint32_t)len); else be32(buf + n + 4, (uint32_t)len);
    memcpy(buf + n + 10, body, len);
    n += 10 + len;
}

static void finish(void) { syncsafe(buf + 6, (uint32_t)(n - 10)); }

static void str_is(const char *got, const char *want, const char *what) {
    char detail[160];
    snprintf(detail, sizeof detail, "got \"%s\", wanted \"%s\"", got, want);
    utest_check_detail(strcmp(got, want) == 0, what, detail);
}

int main(void) {
    utest_begin("tags_test", "ID3 and MIDI tags, in ring 3", 0);
    struct utags t;

    // --- 2.3, Latin-1 and UTF-16 with a BOM ----------------------------
    begin(3);
    frame(3, "TIT2", "\0First Boot", 11);
    static const uint8_t utf16[] = { 1, 0xFF, 0xFE, 't', 0, 0xF6, 0, 'y', 0, 0, 0 };  // "töy", LE
    frame(3, "TPE1", utf16, sizeof utf16);
    finish();
    memset(&t, 0, sizeof t);
    int got = utags_from_id3v2(buf, n, &t, 0);
    str_is(t.title, "First Boot", "2.3: a Latin-1 title");
    str_is(t.artist, "t\xf6y", "2.3: a UTF-16LE artist, ö kept as Latin-1");
    utest_check(got == (UTAGS_TITLE | UTAGS_ARTIST), "...and the found bits say both");

    // --- 2.4, syncsafe sizes, UTF-8 ------------------------------------
    begin(4);
    // 200 bytes of title: its size, 200, is 0x00 0x00 0x01 0x48 syncsafe
    // and 0x00 0x00 0x00 0xC8 plain -- read the wrong way, it overruns.
    uint8_t long_title[201] = { 3 };
    memset(long_title + 1, 'a', 200);
    memcpy(long_title + 1, "\xc3\xa4", 2);   // "ä" in UTF-8
    frame(4, "TIT2", long_title, sizeof long_title);
    frame(4, "TALB", "\3toy-os", 7);
    finish();
    memset(&t, 0, sizeof t);
    utags_from_id3v2(buf, n, &t, 0);
    utest_check(t.title[0] == (char)0xE4 && strlen(t.title) == UTAGS_TEXT_MAX - 1,
                "2.4: a syncsafe-sized UTF-8 title, decoded and truncated to fit");
    str_is(t.album, "toy-os", "2.4: the frame AFTER it is still found");

    // --- refused, never guessed -------------------------------------------
    begin(3);
    frame(3, "TIT2", "\0Over", 5);
    be32(buf + 10 + 4, 400);              // the frame claims more than the tag holds
    finish();
    memset(&t, 0, sizeof t);
    utest_check(utags_from_id3v2(buf, n, &t, 0) == 0 && !t.title[0],
                "a frame that overruns its tag is not read");
    begin(3);
    frame(3, "TIT2", "\x07Bad", 4);      // encoding 7 does not exist
    finish();
    memset(&t, 0, sizeof t);
    utest_check(utags_from_id3v2(buf, n, &t, 0) == 0, "an unknown text encoding is skipped");
    begin(3);
    buf[5] = 0x80;                        // unsynchronised
    frame(3, "TIT2", "\0Sync", 5);
    finish();
    memset(&t, 0, sizeof t);
    utest_check(utags_from_id3v2(buf, n, &t, 0) == 0, "an unsynchronised tag is refused");

    // --- APIC: the front cover wins ------------------------------------
    begin(3);
    static const uint8_t other[] = { 0, 'i','m','a','g','e','/','p','n','g', 0, 0, 0, 'P','N','G' };
    static const uint8_t front[] = { 0, 'i','m','a','g','e','/','j','p','e','g', 0, 3, 'c', 0, 0xFF, 0xD8, 0xFF };
    frame(3, "APIC", other, sizeof other);
    frame(3, "APIC", front, sizeof front);
    finish();
    memset(&t, 0, sizeof t);
    got = utags_from_id3v2(buf, n, &t, 1);
    utest_check((got & UTAGS_ART) && t.art_len == 3 && t.art[0] == 0xFF && t.art[1] == 0xD8,
                "APIC: the front cover's bytes, after its MIME and description");
    utags_free(&t);

    // --- ID3v1 fills only what is empty ---------------------------------
    uint8_t v1[128] = "TAG";
    memcpy(v1 + 3, "Old Title", 9);
    memcpy(v1 + 33, "Old Artist", 10);
    memset(&t, 0, sizeof t);
    strcpy(t.title, "New Title");
    utags_from_id3v1(v1, &t);
    str_is(t.title, "New Title", "ID3v1 does not overwrite a v2 title");
    str_is(t.artist, "Old Artist", "...and fills the empty artist");

    // --- a MIDI track name, after running status ------------------------
    static const uint8_t mid[] = {
        'M','T','h','d', 0,0,0,6, 0,1, 0,1, 0,96,
        'M','T','r','k', 0,0,0,19,
        0, 0x90, 60, 100,     // note on
        0, 62, 100,           // running status
        0, 0xFF, 0x03, 4, 'S','o','n','g',
        0, 0xFF, 0x2F, 0,
    };
    memset(&t, 0, sizeof t);
    utest_check(utags_from_midi(mid, sizeof mid, &t) == UTAGS_TITLE, "MIDI: a track name is found");
    str_is(t.title, "Song", "...through running status");

    // --- the shipped music -------------------------------------------------
    got = utags_read("/usr/share/music/first-boot.mp3", &t, 0);
    str_is(t.title, "First Boot", "first-boot.mp3's ID3 title");
    str_is(t.artist, "toy-os", "...its artist");
    str_is(t.album, "toy-os", "...and its album");
    utest_checkf(t.length_ms == 77800, "...and its TLEN, 77800 ms (got %u)", (unsigned)t.length_ms);
    got = utags_read("/usr/share/music/first-boot.mid", &t, 0);
    str_is(t.title, "First Boot", "first-boot.mid's track name");
    utest_check(utags_read("/usr/share/sounds/chime.wav", &t, 0) == 0,
                "a WAV with no tags reports nothing");
    return utest_end();
}
