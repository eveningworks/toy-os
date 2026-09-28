// midi_test -- the MIDI codec and the SoundFont synth, with no sound
// hardware involved.
//
// **THE BANK AND THE SONGS ARE BUILT HERE, BYTE BY BYTE**, as usnd_test
// builds its WAVs. The bank holds ONE sample -- ten cycles of a 1 kHz
// sine at 48 kHz, looped -- so every note's frequency is known exactly
// and can be counted in zero crossings, and every onset lands on a
// frame that can be computed from the file's ticks and tempo map. A
// real bank (tools/midi_hostcheck.py compares one against FluidSynth)
// could only be judged by resemblance.
//
// The strongest checks are ROUND TRIPS: the same song written as format
// 0 and format 1 must render IDENTICALLY, and a seek into a rest must
// resume sample-for-sample where continuous playback was.
#include "rt/sys.h"
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tmppath.h"
#include "errno.h"

#include "lib/utest.h"

#define RATE 48000

static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

// --- a byte buffer ----------------------------------------------------

static unsigned char g_buf[16 * 1024];
static int g_len;

static void put(const void *p, int n) {
    if (g_len + n > (int)sizeof g_buf) return;
    memcpy(g_buf + g_len, p, (size_t)n);
    g_len += n;
}
static void put8(unsigned v) { unsigned char b = (unsigned char)v; put(&b, 1); }
static void le16(unsigned v) { put8(v); put8(v >> 8); }
static void le32(unsigned v) { le16(v); le16(v >> 16); }
static void be16(unsigned v) { put8(v >> 8); put8(v); }
static void be32(unsigned v) { be16(v >> 16); be16(v); }
static void patch_le32(int at, unsigned v) {
    for (int i = 0; i < 4; i++) g_buf[at + i] = (unsigned char)(v >> (8 * i));
}
static void patch_be32(int at, unsigned v) {
    for (int i = 0; i < 4; i++) g_buf[at + i] = (unsigned char)(v >> (24 - 8 * i));
}

static int write_file(const char *path) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return -1;
    long long n = sys_write(fd, g_buf, (size_t)g_len);
    sys_close(fd);
    return n == g_len ? 0 : -1;
}

static const char *tmp(const char *name) {
    static char p[4][64];
    static int i;
    char *out = p[i++ & 3];
    tmppath(out, 64, TMP_VOLATILE, name);
    return out;
}

// --- the bank ---------------------------------------------------------
//
// Preset (0, 0) plays the sine with its own root, A4 = 69, so key 69 is
// exactly 1 kHz. Preset (128, 0) is a kit: key 36 rooted at 36 (1 kHz),
// and keys 42 and 46 at 500 Hz and 2 kHz sharing exclusive class 1 -- a
// closed hat that chokes an open one.

enum { G_KEYRANGE = 43, G_MODES = 54, G_ROOT = 58, G_EXCL = 57, G_INST = 41, G_SAMPLE = 53 };

static int g_chunk[8], g_depth;
static void chunk_begin(const char *id) { put(id, 4); g_chunk[g_depth++] = g_len; le32(0); }
static void chunk_end(void) {
    int at = g_chunk[--g_depth];
    patch_le32(at, (unsigned)(g_len - at - 4));
    if (g_len & 1) put8(0);
}
static void name20(const char *s) {
    char b[20] = { 0 };
    strncpy(b, s, 19);
    put(b, 20);
}
static void gen(unsigned op, int amount) { le16(op); le16((unsigned)amount & 0xFFFF); }
static void gen_range(unsigned op, int lo, int hi) { le16(op); put8(lo); put8(hi); }

// `bad_instrument` points a preset zone past the instrument table.
static void build_bank(int bad_instrument) {
    g_len = g_depth = 0;
    chunk_begin("RIFF");
    put("sfbk", 4);

    chunk_begin("LIST"); put("INFO", 4);
    chunk_begin("ifil"); le16(2); le16(1); chunk_end();
    chunk_end();

    chunk_begin("LIST"); put("sdta", 4);
    chunk_begin("smpl");
    for (int i = 0; i < 480; i++)                       // 10 cycles of 48 samples
        le16((unsigned)(int)(30000 * sin(2 * M_PI * i / 48)) & 0xFFFF);
    for (int i = 0; i < 46; i++) le16(0);
    chunk_end();
    chunk_end();

    chunk_begin("LIST"); put("pdta", 4);
    chunk_begin("phdr");
    name20("sine");  le16(0); le16(0);   le16(0); le32(0); le32(0); le32(0);
    name20("kit");   le16(0); le16(128); le16(1); le32(0); le32(0); le32(0);
    name20("EOP");   le16(0); le16(0);   le16(2); le32(0); le32(0); le32(0);
    chunk_end();
    chunk_begin("pbag"); le16(0); le16(0); le16(1); le16(0); le16(2); le16(0); chunk_end();
    chunk_begin("pmod"); for (int i = 0; i < 10; i++) put8(0); chunk_end();
    chunk_begin("pgen");
    gen(G_INST, 0);
    gen(G_INST, bad_instrument ? 7 : 1);
    gen(0, 0);
    chunk_end();
    chunk_begin("inst");
    name20("sine"); le16(0);
    name20("kit");  le16(1);
    name20("EOI");  le16(4);
    chunk_end();
    chunk_begin("ibag");
    le16(0); le16(0);       // sine: 2 gens
    le16(2); le16(0);       // kit 36: 4 gens
    le16(6); le16(0);       // kit 42: 5 gens
    le16(11); le16(0);      // kit 46: 5 gens
    le16(16); le16(0);
    chunk_end();
    chunk_begin("imod"); for (int i = 0; i < 10; i++) put8(0); chunk_end();
    chunk_begin("igen");
    gen(G_MODES, 1); gen(G_SAMPLE, 0);
    gen_range(G_KEYRANGE, 36, 36); gen(G_MODES, 1); gen(G_ROOT, 36); gen(G_SAMPLE, 0);
    gen_range(G_KEYRANGE, 42, 42); gen(G_MODES, 1); gen(G_ROOT, 54); gen(G_EXCL, 1); gen(G_SAMPLE, 0);
    gen_range(G_KEYRANGE, 46, 46); gen(G_MODES, 1); gen(G_ROOT, 34); gen(G_EXCL, 1); gen(G_SAMPLE, 0);
    gen(0, 0);
    chunk_end();
    chunk_begin("shdr");
    name20("sine1k"); le32(0); le32(480); le32(0); le32(480); le32(RATE);
    put8(69); put8(0); le16(0); le16(1);
    name20("EOS"); le32(0); le32(0); le32(0); le32(0); le32(0); put8(0); put8(0); le16(0); le16(0);
    chunk_end();
    chunk_end();

    chunk_end();
}

// --- songs ------------------------------------------------------------

static int g_trk;
static void smf_begin(int format, int ntracks, int division) {
    g_len = 0;
    put("MThd", 4); be32(6); be16(format); be16(ntracks); be16(division);
}
static void trk_begin(void) { put("MTrk", 4); g_trk = g_len; be32(0); }
static void trk_end(int with_eot) {
    if (with_eot) { put8(0); put8(0xFF); put8(0x2F); put8(0); }
    patch_be32(g_trk, (unsigned)(g_len - g_trk - 4));
}
static void delta(unsigned d) {
    unsigned char b[4];
    int n = 0;
    b[n++] = d & 0x7F;
    while (d >>= 7) b[n++] = 0x80 | (d & 0x7F);
    while (n) put8(b[--n]);
}
static void ev3(unsigned d, int a, int b, int c) { delta(d); put8(a); put8(b); put8(c); }
static void ev2(unsigned d, int a, int b) { delta(d); put8(a); put8(b); }
static void tempo(unsigned d, unsigned us) {
    delta(d); put8(0xFF); put8(0x51); put8(3); put8(us >> 16); put8(us >> 8); put8(us);
}

// THE REFERENCE SONG, at 480 ticks a quarter: tempo 500000 (a tick is
// 1/960 s), key 69 from tick 0 to 800, then at tick 960 the tempo halves
// and key 81 plays for 480 ticks -- 0.125 s at the new tempo, not 0.5.
// Note-offs are running-status note-ons at velocity 0.
//   key 69: 0 .. 0.8333 s    rest: 0.8333 .. 1.0 s    key 81: 1.0 .. 1.25 s
static void song_format1(void) {
    smf_begin(1, 2, 480);
    trk_begin();
    tempo(0, 500000);
    tempo(960, 250000);
    delta(480); put8(0xFF); put8(0x2F); put8(0);
    trk_end(0);
    trk_begin();
    ev3(0, 0x90, 69, 127);
    ev2(800, 69, 0);            // running status
    ev2(160, 81, 127);
    ev2(480, 81, 0);
    trk_end(1);
}

// The same song as ONE track: the tempo events interleaved in time.
static void song_format0(void) {
    smf_begin(0, 1, 480);
    trk_begin();
    tempo(0, 500000);
    ev3(0, 0x90, 69, 127);
    ev2(800, 69, 0);
    tempo(160, 250000);
    ev3(0, 0x90, 81, 127);
    ev2(480, 81, 0);
    trk_end(1);
}

// --- rendering and measuring ------------------------------------------

struct pcm { int16_t *d; long frames; };

static int render(const char *path, struct pcm *out, uint64_t seek_to) {
    struct usnd_stream s;
    int rc = usnd_open(path, &s);
    if (rc != 0) return rc;
    if (seek_to && (rc = usnd_seek(&s, seek_to)) != 0) { usnd_close(&s); return rc; }
    long cap = (long)usnd_stream_frames(&s) + 1024;
    out->d = malloc((size_t)cap * 4);
    out->frames = 0;
    if (!out->d) { usnd_close(&s); return -ENOMEM; }
    long n;
    while (out->frames < cap && (n = usnd_read(&s, out->d + out->frames * 2, 1024)) > 0)
        out->frames += n;
    usnd_close(&s);
    return 0;
}

// Crossings of the LEFT channel, as a frequency. A sine crosses twice a
// cycle.
static long freq(const struct pcm *p, double t0, double t1) {
    long a = (long)(t0 * RATE), b = (long)(t1 * RATE), zc = 0;
    if (b > p->frames) b = p->frames;
    for (long i = a + 1; i < b; i++)
        if ((p->d[(i - 1) * 2] < 0) != (p->d[i * 2] < 0)) zc++;
    return b > a ? (long)((double)zc * RATE / (b - a) / 2 + 0.5) : 0;
}

static int peak(const struct pcm *p, double t0, double t1) {
    long a = (long)(t0 * RATE), b = (long)(t1 * RATE);
    int m = 0;
    if (b > p->frames) b = p->frames;
    for (long i = a; i < b; i++) {
        int v = p->d[i * 2] < 0 ? -p->d[i * 2] : p->d[i * 2];
        if (v > m) m = v;
    }
    return m;
}

static long onset(const struct pcm *p, double from) {
    for (long i = (long)(from * RATE); i < p->frames; i++)
        if (p->d[i * 2] > 2000 || p->d[i * 2] < -2000) return i;
    return -1;
}

static void near(const char *what, long got, long want, long tol) {
    utest_checkf(got >= want - tol && got <= want + tol,
                 "%s -- got %ld, wanted %ld +- %ld", what, got, want, tol);
}

// --- the checks -------------------------------------------------------

static char g_bank[64];

static void check_probe(void) {
    song_format1();
    check("a MIDI file is probed as audio", usnd_probe(g_buf, 16), "");
    static const char trk[] = "MTrk\0\0\0\4\0\x90\x3c\x64\0\0\0\0";
    check("a bare track chunk is not", !usnd_probe(trk, 16), "");
}

static struct pcm g_ref;

static void check_timing(void) {
    song_format1();
    const char *p = tmp("midi_f1.mid");
    check("write the format 1 song", write_file(p) == 0, "");

    struct usnd_info in;
    int rc = usnd_load_info(p, &in);
    check("load_info accepts format 1", rc == 0, usnd_last_error());
    check("the codec names itself midi", rc == 0 && strcmp(in.format, "midi") == 0,
          rc == 0 ? in.format : "");
    // 1.25 s of song, then the codec's fixed 2 s tail for releases.
    near("length follows the tempo map", (long)in.frames, 60000 + 2 * RATE, 0);

    rc = render(p, &g_ref, 0);
    check("the song renders", rc == 0, usnd_last_error());
    if (rc != 0) return;
    near("key 69 plays at its root: 1 kHz", freq(&g_ref, 0.1, 0.7), 1000, 5);
    near("key 81 plays an octave up: 2 kHz", freq(&g_ref, 1.02, 1.23), 2000, 10);
    check("the note-off (a running-status zero velocity) is a rest",
          peak(&g_ref, 0.86, 0.99) < 50, "");
    // Onsets land on the tempo map to within the envelope's 1 ms delay
    // and the synth's 64-frame block.
    near("the second note starts at 1.0 s despite the tempo change",
         onset(&g_ref, 0.9), RATE, 150);
    check("and ends at 1.25 s", peak(&g_ref, 1.27, 1.5) < 50, "");
    check("the note is loud enough to measure", peak(&g_ref, 0.1, 0.7) > 8000, "");
}

static void check_format0(void) {
    song_format0();
    const char *p = tmp("midi_f0.mid");
    write_file(p);
    struct pcm f0;
    int rc = render(p, &f0, 0);
    check("format 0 renders", rc == 0, usnd_last_error());
    if (rc != 0 || !g_ref.d) return;
    long diff = f0.frames == g_ref.frames ? 0 : -1;
    for (long i = 0; diff == 0 && i < f0.frames * 2; i++)
        if (f0.d[i] != g_ref.d[i]) diff = i / 2 + 1;
    utest_checkf(diff == 0, "format 0 and format 1 of one song render identically "
                 "(first difference at frame %ld)", diff);
    free(f0.d);
}

// A seek into the rest must resume exactly where continuous playback
// was: controllers replayed, nothing sounding, the tempo map honoured.
static void check_seek(void) {
    if (!g_ref.d) return;
    struct pcm s;
    // OFF the synth's 64-frame block grid on purpose: 0.9 s alone is a
    // multiple of 64, where a seek that forgot the grid still lands.
    uint64_t at = (uint64_t)(0.9 * RATE) + 7;
    int rc = render(tmp("midi_f1.mid"), &s, at);
    check("seek succeeds", rc == 0, usnd_last_error());
    if (rc != 0) return;
    long want = g_ref.frames - (long)at, diff = s.frames == want ? 0 : -1;
    for (long i = 0; diff == 0 && i < want * 2; i++)
        if (s.d[i] != g_ref.d[at * 2 + i]) diff = i / 2 + 1;
    utest_checkf(diff == 0, "a seek into the rest resumes sample for sample "
                 "(got %ld frames, first difference at %ld)", s.frames, diff);
    free(s.d);
}

// Channel 10 is the kit: key 36 there is 1 kHz, and on channel 1 the
// same key is the melodic sine 33 semitones down.
static void check_drums(void) {
    smf_begin(0, 1, 480);
    trk_begin();
    ev3(0, 0x99, 36, 127);
    ev3(480, 0x89, 36, 0);
    ev3(96, 0x90, 36, 127);
    ev3(480, 0x80, 36, 0);
    trk_end(1);
    const char *p = tmp("midi_drum.mid");
    write_file(p);
    struct pcm d;
    if (render(p, &d, 0) != 0) { check("drum song renders", 0, usnd_last_error()); return; }
    near("channel 10 plays the kit (1 kHz)", freq(&d, 0.05, 0.45), 1000, 5);
    near("channel 1 plays the melodic preset (148.6 Hz)", freq(&d, 0.65, 1.05), 149, 3);
    free(d.d);
}

// Full bend at the default range is two semitones; RPN 0 set to twelve
// makes the same bend an octave.
static void check_bend(void) {
    smf_begin(0, 1, 480);
    trk_begin();
    ev3(0, 0xE0, 0x7F, 0x7F);
    ev3(0, 0x90, 69, 127);
    ev3(480, 0x80, 69, 0);
    ev3(0, 0xB0, 101, 0);
    ev3(0, 0xB0, 100, 0);
    ev3(0, 0xB0, 6, 12);
    ev3(96, 0x90, 69, 127);
    ev3(480, 0x80, 69, 0);
    trk_end(1);
    const char *p = tmp("midi_bend.mid");
    write_file(p);
    struct pcm d;
    if (render(p, &d, 0) != 0) { check("bend song renders", 0, usnd_last_error()); return; }
    near("a full bend is two semitones (1122 Hz)", freq(&d, 0.05, 0.45), 1122, 6);
    near("RPN 0 widens it to an octave (2 kHz)", freq(&d, 0.65, 1.05), 2000, 10);
    free(d.d);
}

// The pedal holds a released note; lifting it lets go.
static void check_sustain(void) {
    smf_begin(0, 1, 480);
    trk_begin();
    ev3(0, 0xB0, 64, 127);
    ev3(0, 0x90, 69, 127);
    ev3(192, 0x80, 69, 0);          // key up at 0.2 s
    ev3(384, 0xB0, 64, 0);          // pedal up at 0.6 s
    trk_end(1);
    const char *p = tmp("midi_ped.mid");
    write_file(p);
    struct pcm d;
    if (render(p, &d, 0) != 0) { check("pedal song renders", 0, usnd_last_error()); return; }
    check("the pedal holds a released key", peak(&d, 0.35, 0.55) > 8000, "");
    check("lifting it releases the note", peak(&d, 0.65, 0.9) < 50, "");
    free(d.d);
}

// Exclusive class: the closed hat (500 Hz) chokes the open one (2 kHz),
// which is never released by a key-up.
static void check_exclusive(void) {
    smf_begin(0, 1, 480);
    trk_begin();
    ev3(0, 0x99, 46, 127);
    ev3(192, 0x99, 42, 127);        // 0.2 s
    ev3(192, 0x89, 42, 0);
    trk_end(1);
    const char *p = tmp("midi_excl.mid");
    write_file(p);
    struct pcm d;
    if (render(p, &d, 0) != 0) { check("hat song renders", 0, usnd_last_error()); return; }
    near("the open hat plays alone first (2 kHz)", freq(&d, 0.02, 0.18), 2000, 10);
    near("the closed hat chokes it (500 Hz alone)", freq(&d, 0.23, 0.38), 500, 5);
    free(d.d);
}

// CC7 at zero silences the channel; a GM System On restores it.
static void check_reset(void) {
    smf_begin(0, 1, 480);
    trk_begin();
    ev3(0, 0xB0, 7, 0);
    ev3(0, 0x90, 69, 127);
    ev3(192, 0x80, 69, 0);
    delta(96); put8(0xF0); put8(5); put8(0x7E); put8(0x7F); put8(0x09); put8(0x01); put8(0xF7);
    ev3(0, 0x90, 69, 127);
    ev3(192, 0x80, 69, 0);
    trk_end(1);
    const char *p = tmp("midi_reset.mid");
    write_file(p);
    struct pcm d;
    if (render(p, &d, 0) != 0) { check("reset song renders", 0, usnd_last_error()); return; }
    check("volume 0 is silence", peak(&d, 0.02, 0.18) < 50, "");
    check("GM System On restores the volume", peak(&d, 0.35, 0.45) > 8000, "");
    free(d.d);
}

static void refuse(const char *what, int want) {
    const char *p = tmp("midi_bad.mid");
    write_file(p);
    struct usnd_info in;
    int rc = usnd_load_info(p, &in);
    utest_checkf(rc == want, "%s -- got %d (%s), wanted %d", what, rc, usnd_last_error(), want);
}

static void check_refusals(void) {
    smf_begin(2, 1, 480);
    trk_begin(); ev3(0, 0x90, 60, 100); trk_end(1);
    refuse("format 2 is refused as unplayable", -ENOTSUP);

    smf_begin(0, 1, 480);
    trk_begin(); delta(0); put8(0x90); put8(60); trk_end(0);
    refuse("an event cut in half is corrupt", -EINVAL);

    smf_begin(0, 1, 480);
    trk_begin(); delta(0); put8(60); put8(100); trk_end(1);
    refuse("a data byte with no status is corrupt", -EINVAL);

    smf_begin(0, 1, 480);
    trk_begin(); ev3(0, 0x90, 60, 100); trk_end(1);
    patch_be32(g_trk, 4096);
    refuse("a chunk past the end of the file is corrupt", -EINVAL);

    smf_begin(1, 1, 480);
    trk_begin(); ev3(0, 0x90, 60, 100); trk_end(0);
    const char *p = tmp("midi_noeot.mid");
    write_file(p);
    struct usnd_info in;
    check("a track without End of Track is accepted", usnd_load_info(p, &in) == 0,
          usnd_last_error());

    usnd_mid_set_soundfont("/nonexistent/bank.sf2");
    song_format1();
    refuse("no SoundFont is unplayable, not corrupt", -ENOTSUP);

    // Describing a song needs no bank -- only playing it loads one -- so
    // this refusal is the PLAY path's.
    build_bank(1);
    const char *bad = tmp("midi_bad.sf2");
    write_file(bad);
    usnd_mid_set_soundfont(bad);
    song_format1();
    const char *song = tmp("midi_bad.mid");
    write_file(song);
    check("describing a song does not load the bank", usnd_load_info(song, &in) == 0,
          usnd_last_error());
    struct usnd_stream st;
    int rc = usnd_open(song, &st);
    utest_checkf(rc == -EINVAL, "playing with a bank naming a missing instrument is refused "
                 "-- got %d (%s), wanted %d", rc, usnd_last_error(), -EINVAL);
    if (rc == 0) usnd_close(&st);
    usnd_mid_set_soundfont(g_bank);
}

// The SHIPPED song through whatever bank the codec picks -- the built-in
// one, or a fetched one that outranks it -- so a run on an EXTRAS image
// times the bank people will actually hear. That both reach the image
// and play is a check; the speed is a note, since under TCG it measures
// the emulator as much as the code.
static void check_shipped(void) {
    usnd_mid_set_soundfont(0);
    struct usnd_stream st;
    unsigned long long t0 = sys_monotonic_ns();
    int rc = usnd_open("/usr/share/music/first-boot.mid", &st);
    unsigned long long load = sys_monotonic_ns() - t0;
    check("the shipped song opens with the installed bank", rc == 0, usnd_last_error());
    if (rc == 0) {
        static int16_t buf[1024 * 2];
        long frames = 0, n;
        int loud = 0;
        t0 = sys_monotonic_ns();
        while (frames < 20L * RATE && (n = usnd_read(&st, buf, 1024)) > 0) {
            for (long i = 0; i < n * 2; i++)
                if (buf[i] > 3000 || buf[i] < -3000) loud = 1;
            frames += n;
        }
        unsigned long long ns = sys_monotonic_ns() - t0;
        check("and it is not silent", loud, "");
        utest_notef("%s: opened in %llu ms; 20 s rendered in %llu ms (%llu.%01llux real time)",
                    st.detail, load / 1000000, ns / 1000000, ns ? 20000000000ULL / ns : 0,
                    ns ? (200000000000ULL / ns) % 10 : 0);
        usnd_close(&st);
    }
    usnd_mid_set_soundfont(g_bank);
}

int main(void) {
    utest_begin("midi_test", "MIDI files through the SoundFont synth, with no sound hardware",
                UTEST_QUIET);

    build_bank(0);
    // Copied: tmp() hands out a rotating buffer, and the bank's path
    // must outlive every other fixture's.
    strlcpy(g_bank, tmp("midi_test.sf2"), sizeof g_bank);
    if (write_file(g_bank) != 0) return utest_skip("cannot write the fixture bank");
    usnd_mid_set_soundfont(g_bank);

    check_probe();
    check_timing();
    check_format0();
    check_seek();
    check_drums();
    check_bend();
    check_sustain();
    check_exclusive();
    check_reset();
    check_refusals();
    check_shipped();

    usnd_mid_set_soundfont(0);
    free(g_ref.d);
    static const char *const files[] = {
        "midi_test.sf2", "midi_bad.sf2", "midi_f1.mid", "midi_f0.mid", "midi_drum.mid",
        "midi_bend.mid", "midi_ped.mid", "midi_excl.mid", "midi_reset.mid", "midi_bad.mid",
        "midi_noeot.mid",
    };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) sys_unlink(tmp(files[i]));
    return utest_end();
}
