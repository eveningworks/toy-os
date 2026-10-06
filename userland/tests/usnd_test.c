// usnd_test -- the audio DECODE path, with no sound hardware involved.
//
// This is the half of lib/usnd.h that can be tested without a card:
// the WAV parser and the rate/channel/width conversion. Playback needs
// an AC97 and is judged on the HOST instead (tools/audio_test.py
// records what the device emitted), so the two halves are checked by
// the two things that can actually see them.
//
// **THE FIXTURES ARE BUILT HERE, BYTE BY BYTE**, rather than read from
// /usr/share/sounds. A test that reads the shipped files can only
// assert what those files happen to be; writing the bytes means the
// expected samples are known exactly, and it is the only way to
// construct the awkward cases -- a chunk between `fmt ` and `data`, a
// data chunk that lies about its length, a float WAV.
#include "rt/sys.h"
#include "lib/usnd.h"
#include "lib/usndfmt.h"
#include "sound_abi.h"
#include "lib/usnd_internal.h"
#include <stdio.h>
#include <string.h>
#include "tmppath.h"

#include "lib/utest.h"

// The call sites here read `check(what, ok, detail)`; the harness takes
// the boolean first. One adapter rather than transposing thirty call
// sites: a transposed argument pair compiles and INVERTS the check,
// which is the failure a green suite hides.
static void check(const char *what, int ok, const char *detail) {
    utest_check_detail(ok, what, detail);
}

static void eq(const char *what, long got, long want) {
    utest_checkf(got == want, "%s -- got %ld, wanted %ld", what, got, want);
}

// --- building a WAV ---------------------------------------------------

static unsigned char g_buf[64 * 1024];
static int g_len;

static void put(const void *p, int n) {
    if (g_len + n > (int)sizeof g_buf) return;
    memcpy(g_buf + g_len, p, (size_t)n);
    g_len += n;
}
static void put32(unsigned v) { unsigned char b[4] = { (unsigned char)v, (unsigned char)(v>>8), (unsigned char)(v>>16), (unsigned char)(v>>24) }; put(b, 4); }
static void put16(unsigned v) { unsigned char b[2] = { (unsigned char)v, (unsigned char)(v>>8) }; put(b, 2); }

// `extra` inserts a junk chunk between `fmt ` and `data`, which is what
// a real recorder's LIST/INFO looks like. `data_size` is written into
// the header as given, so a caller can make it LIE.
static void build_wav(int tag, int channels, int rate, int bits,
                      const void *samples, int sample_bytes,
                      int extra, long data_size) {
    g_len = 0;
    put("RIFF", 4); put32(0); put("WAVE", 4);   // size patched below
    put("fmt ", 4); put32(16);
    put16(tag); put16(channels); put32(rate);
    put32(rate * channels * (bits / 8));
    put16(channels * (bits / 8)); put16(bits);
    if (extra) {
        // ODD length on purpose: the pad byte is not counted in the
        // chunk size, and a walker that forgets it lands one byte short
        // of "data" and never finds it.
        put("LIST", 4); put32(5); put("INFOx", 5); put16(0);
        g_len--;  // the pad is one byte, not two
    }
    put("data", 4); put32((unsigned)(data_size < 0 ? sample_bytes : data_size));
    put(samples, sample_bytes);
    unsigned char b[4] = { (unsigned char)(g_len - 8), (unsigned char)((g_len - 8) >> 8),
                           (unsigned char)((g_len - 8) >> 16), (unsigned char)((g_len - 8) >> 24) };
    memcpy(g_buf + 4, b, 4);
}

static const char *write_fixture(const char *path) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) return "cannot create fixture";
    long long n = sys_write(fd, g_buf, (size_t)g_len);
    sys_close(fd);
    return n == g_len ? 0 : "short write";
}

static const char *p_fix(void) {
    static char p[64];
    if (!p[0]) tmppath(p, sizeof p, TMP_VOLATILE, "usnd_fix.wav");
    return p;
}
#define FIX p_fix()

// --- the checks -------------------------------------------------------

static void check_header(void) {
    short s[8] = { 100, -100, 200, -200, 300, -300, 400, -400 };
    build_wav(1, 2, 44100, 16, s, sizeof s, 0, -1);
    const char *err = write_fixture(FIX);
    check("write fixture", !err, err ? err : "");

    struct usnd_info in;
    int rc = usnd_load_info(FIX, &in);
    check("load_info accepts a WAV", rc == 0, usnd_last_error());
    eq("rate", in.fmt.rate, 44100);
    eq("channels", in.fmt.channels, 2);
    eq("bits", in.fmt.bits, 16);
    eq("frames", (long)in.frames, 4);
    check("codec name", strcmp(in.format, "wav") == 0, in.format);
}

// A chunk BETWEEN fmt and data. A reader that seeks to a fixed offset
// passes every other check here and fails only this one.
static void check_chunk_walk(void) {
    short s[4] = { 1, 2, 3, 4 };
    build_wav(1, 2, 48000, 16, s, sizeof s, 1, -1);
    write_fixture(FIX);

    struct usnd_info in;
    int rc = usnd_load_info(FIX, &in);
    check("a chunk between fmt and data is skipped", rc == 0, usnd_last_error());
    eq("frames past a LIST chunk", (long)in.frames, 2);
}

// A data chunk claiming more than the file holds. Believing it means
// reading past the end for the whole tail.
static void check_truncated(void) {
    short s[4] = { 7, 8, 9, 10 };
    build_wav(1, 2, 48000, 16, s, sizeof s, 0, 40000);
    write_fixture(FIX);

    struct usnd_info in;
    check("a lying data size is accepted", usnd_load_info(FIX, &in) == 0, usnd_last_error());
    eq("frames clamped to the file", (long)in.frames, 2);
}

// 48 kHz stereo takes the copy-only fast path: the samples must come
// back EXACTLY, not merely close.
static void check_passthrough(void) {
    short s[8] = { 1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000 };
    build_wav(1, 2, 48000, 16, s, sizeof s, 0, -1);
    write_fixture(FIX);

    struct usnd_stream st;
    check("open 48k stereo", usnd_open(FIX, &st) == 0, usnd_last_error());
    int32_t got[8];
    long n = usnd_read(&st, got, 4);
    eq("48k stereo frame count", n, 4);
    int same = 1;
    for (int i = 0; i < 8; i++) if (got[i] != (int32_t)s[i] * 65536) same = 0;
    check("48k stereo is exact, each sample in the top 16 bits", same,
          "the fast path altered the samples");
    eq("stream length in device frames", (long)usnd_stream_frames(&st), 4);
    eq("a read past the end of the fast path", usnd_read(&st, got, 4), 0);
    usnd_close(&st);
}

// Mono is DUPLICATED, not halved: both channels must equal the source.
static void check_mono_upmix(void) {
    short s[4] = { 500, -500, 1500, -1500 };
    build_wav(1, 1, 48000, 16, s, sizeof s, 0, -1);
    write_fixture(FIX);

    struct usnd_stream st;
    check("open 48k mono", usnd_open(FIX, &st) == 0, usnd_last_error());
    int32_t got[8];
    long n = usnd_read(&st, got, 4);
    eq("mono frame count", n, 4);
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (got[i * 2] != (int32_t)s[i] * 65536 || got[i * 2 + 1] != (int32_t)s[i] * 65536) ok = 0;
    check("mono is duplicated to both channels", ok, "channels differ from the source");
    usnd_close(&st);
}

// 8-bit WAV is UNSIGNED, centred on 128. Reading it as signed makes
// silence full-scale, which is the loudest possible way to be wrong.
static void check_8bit(void) {
    unsigned char s[4] = { 128, 255, 0, 192 };   // 0, +max, -max, +half
    build_wav(1, 1, 48000, 8, s, sizeof s, 0, -1);
    write_fixture(FIX);

    struct usnd_stream st;
    check("open 8-bit", usnd_open(FIX, &st) == 0, usnd_last_error());
    int32_t got[8];
    eq("8-bit frame count", usnd_read(&st, got, 4), 4);
    eq("8-bit 128 is silence", got[0], 0);
    check("8-bit 255 is positive", got[2] > 30000 * 65536, "255 did not map near full scale");
    check("8-bit 0 is negative", got[4] < -30000 * 65536, "0 did not map near negative full scale");
    usnd_close(&st);
}

// 24-BIT KEEPS ITS LOW BYTE -- the reason the path is s32. A version
// that still cut every sample to 16 bits passes every check above.
static void check_24bit(void) {
    unsigned char s[12] = { 0x56, 0x34, 0x12,  0xAA, 0xCB, 0xED,     // +0x123456, -0x123456
                            0x01, 0x00, 0x00,  0xFF, 0xFF, 0x7F };   // 1, +max
    build_wav(1, 2, 48000, 24, s, sizeof s, 0, -1);
    write_fixture(FIX);

    struct usnd_stream st;
    check("open 24-bit", usnd_open(FIX, &st) == 0, usnd_last_error());
    int32_t got[4];
    eq("24-bit frame count", usnd_read(&st, got, 2), 2);
    eq("24-bit sample, all 24 bits, at the top", got[0], 0x12345600);
    eq("a negative one too", got[1], -0x12345600);
    eq("the least significant bit survives", got[2], 0x100);
    eq("24-bit full scale", got[3], 0x7FFFFF00);
    usnd_close(&st);
}

// Half the device rate must yield twice the frames.
static void check_resample(void) {
    short s[64];
    for (int i = 0; i < 64; i++) s[i] = (short)(i * 100);
    build_wav(1, 2, 24000, 16, s, sizeof s, 0, -1);
    write_fixture(FIX);

    struct usnd_stream st;
    check("open 24k", usnd_open(FIX, &st) == 0, usnd_last_error());
    eq("24k reports double length", (long)usnd_stream_frames(&st), 64);

    int32_t got[256];
    long total = 0;
    int rounds = 0;
    // BOUNDED. A read that never returns 0 is the failure mode this
    // exists to catch, and an unbounded loop would HANG the suite
    // instead of failing it -- which is strictly worse, because a hang
    // looks like a slow machine.
    for (; rounds < 100; rounds++) {
        long n = usnd_read(&st, got, 64);
        if (n <= 0) break;
        total += n;
    }
    check("the end of a resampled stream is reached", rounds < 100,
          "usnd_read never returned 0 -- the stream does not end");
    // EXACTLY double, not "about". The step is exactly 0.5 at these
    // rates, and the last source frame is emitted rather than dropped --
    // which is the bug this number is set to catch (32 in, 63 out).
    eq("24k decodes to exactly twice the frames", total, 64);
    // STICKY: reading again past the end must still be 0. A version
    // that emitted one more stale frame per call would satisfy every
    // check above and loop forever in a player.
    eq("a read past the end of a resampled stream", usnd_read(&st, got, 64), 0);
    usnd_close(&st);
}

// Seeking must land where a straight read would have been.
static void check_seek(void) {
    short s[64];
    for (int i = 0; i < 64; i++) s[i] = (short)(i * 500);
    build_wav(1, 2, 48000, 16, s, sizeof s, 0, -1);
    write_fixture(FIX);

    struct usnd_stream st;
    check("open for seek", usnd_open(FIX, &st) == 0, usnd_last_error());
    int32_t whole[64];
    usnd_read(&st, whole, 32);

    check("seek is supported", usnd_seek(&st, 8) == 0, usnd_last_error());
    int32_t after[32];
    long n = usnd_read(&st, after, 8);
    eq("frames after seek", n, 8);
    check("seek lands on the right samples",
          memcmp(after, whole + 8 * 2, 8 * 2 * sizeof(int32_t)) == 0,
          "the samples after a seek are not the ones at that offset");
    usnd_close(&st);
}

// A REFUSAL IS NOT A CORRUPTION, and usnd reports them differently:
// a float WAV is a good file this build will not play.
static void check_refusals(void) {
    short s[4] = { 0, 0, 0, 0 };
    build_wav(3 /* IEEE_FLOAT */, 2, 48000, 32, s, sizeof s, 0, -1);
    write_fixture(FIX);
    struct usnd_info in;
    check("float WAV is refused", usnd_load_info(FIX, &in) == -ENOTSUP, usnd_last_error());

    build_wav(0x11 /* ADPCM */, 2, 48000, 16, s, sizeof s, 0, -1);
    write_fixture(FIX);
    check("compressed WAV is refused", usnd_load_info(FIX, &in) == -ENOTSUP, usnd_last_error());

    // Not a WAV at all -- a DIFFERENT answer from the two above.
    int fd = sys_open(FIX, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd >= 0) { sys_write(fd, "this is not audio, not at all", 29); sys_close(fd); }
    check("a non-audio file is -EINVAL", usnd_load_info(FIX, &in) == -EINVAL, usnd_last_error());

    unsigned char head[16];
    memcpy(head, "RIFF____WAVE____", 16);
    check("probe accepts a RIFF/WAVE header", usnd_probe(head, 16), "probe said no");
    check("probe rejects other bytes", !usnd_probe("not audio at all", 16), "probe said yes");
}

// --- MP3 -------------------------------------------------------------
//
// The fixture here is READ rather than built, which is the opposite of
// every WAV case above and deliberate: an MP3 cannot be written by hand,
// and /tests/sine1k.mp3 is a steady 1 kHz tone whose decoded shape is
// therefore known without a reference decoder. The exhaustive comparison
// against ffmpeg lives on the host (tools/usnd_hostcheck.py); what only
// this side can prove is that the decoder works in RING 3 -- against the
// real filesystem, the real heap, and a 2 KiB frame budget.
static void check_mp3(void) {
    check("the Huffman tables are complete prefix codes",
          usnd_mp3_selftest() == 0, "usnd_mp3_selftest() refused");

    struct usnd_info in;
    int rc = usnd_load_info("/tests/sine1k.mp3", &in);
    check("an MP3 is recognised", rc == 0, usnd_last_error());
    if (rc != 0) return;
    eq("the MP3's rate", in.fmt.rate, 44100);
    eq("the MP3's channel count", in.fmt.channels, 1);
    check("the codec names itself mp3", strcmp(in.format, "mp3") == 0, in.format);

    struct usnd_clip clip;
    rc = usnd_clip_load("/tests/sine1k.mp3", &clip);
    check("an MP3 decodes to a clip", rc == 0, usnd_last_error());
    if (rc != 0) return;

    // 1.5 s at 44.1 kHz, resampled to the device's 48 kHz and upmixed to
    // stereo. Allow a frame either way: an MP3 carries whole 1152-sample
    // frames, so the length is quantised rather than exact.
    uint64_t want = 1500 * 48;
    check("the clip is about 1.5 seconds",
          clip.frames > want - 2400 && clip.frames < want + 2400, "wrong length");

    // ONE STEADY FREQUENCY is the whole point of this fixture: count zero
    // crossings over the middle, away from the encoder's ramp at each end.
    long lo = (long)(clip.frames / 4), hi = (long)(clip.frames * 3 / 4);
    long crossings = 0, peak = 0;
    for (long i = lo + 1; i < hi; i++) {
        int a = clip.pcm[(i - 1) * 2] / 65536, b = clip.pcm[i * 2] / 65536;
        if ((a < 0) != (b < 0)) crossings++;
        if (b > peak) peak = b;
        if (-b > peak) peak = -b;
    }
    long hz = crossings * 48000 / (2 * (hi - lo - 1));
    check("the decoded tone is 1 kHz", hz > 980 && hz < 1020, "wrong frequency");
    check("the decoded tone has real amplitude", peak > 8000, "too quiet");

    // Both channels, because a mono MP3 reaches the mixer through the
    // same upmix a mono WAV does and a decoder that filled only the left
    // would still pass every check above.
    long diff = 0;
    for (long i = lo; i < hi; i += 97)
        if (clip.pcm[i * 2] != clip.pcm[i * 2 + 1]) diff++;
    eq("the mono upmix filled both channels", diff, 0);

    usnd_clip_free(&clip);
}

// --- FLAC -------------------------------------------------------------
//
// Exactness against `flac -d` is tools/usnd_hostcheck.py's job; here the
// codec runs in ring 3. ramp24.flac's samples are a FORMULA of their
// index (tools/gen_music.py's ramp24()), so the check needs no reference
// decoder: every 24-bit sample must come back whole, at the top of s32.
static void check_flac(void) {
    struct usnd_info in;
    int rc = usnd_load_info("/tests/ramp24.flac", &in);
    check("a FLAC is recognised", rc == 0, usnd_last_error());
    if (rc != 0) return;
    check("the codec names itself flac", strcmp(in.format, "flac") == 0, in.format);
    eq("the FLAC's depth", in.fmt.bits, 24);
    eq("its rate", in.fmt.rate, 48000);
    eq("its length", (long)in.frames, 4800);

    struct usnd_stream st;
    check("open the 24-bit FLAC", usnd_open("/tests/ramp24.flac", &st) == 0, usnd_last_error());
    static int32_t got[4800 * 2];
    long n = 0, k;
    while (n < 4800 && (k = usnd_read(&st, got + n * 2, 4800 - n)) > 0) n += k;
    eq("every frame decoded", n, 4800);
    long bad = -1;
    for (long i = 0; i < n && bad < 0; i++) {
        int32_t left = (int32_t)((i * 7919) % 0x1000000) - 0x800000;
        if (got[i * 2] != left * 256 || got[i * 2 + 1] != (-left - 1) * 256) bad = i;
    }
    char detail[96];
    snprintf(detail, sizeof detail, "first wrong frame %ld", bad);
    check("every 24-bit sample exact, low byte and all", bad < 0, detail);

    // A seek lands on the very frame the formula names.
    check("FLAC seeks", usnd_seek(&st, 3333) == 0, usnd_last_error());
    int32_t one[2];
    eq("a frame after the seek", usnd_read(&st, one, 1), 1);
    int32_t left = (int32_t)((3333L * 7919) % 0x1000000) - 0x800000;
    check("the seek is exact", one[0] == left * 256, "landed on another frame");
    usnd_close(&st);

    // 44.1 kHz mono 16-bit, through the resampler and the upmix: the tone
    // must still be 1 kHz with both channels filled.
    struct usnd_clip clip;
    rc = usnd_clip_load("/tests/sine1k.flac", &clip);
    check("a 16-bit FLAC decodes to a clip", rc == 0, usnd_last_error());
    if (rc != 0) return;
    long lo = (long)(clip.frames / 4), hi = (long)(clip.frames * 3 / 4), crossings = 0, diff = 0;
    for (long i = lo + 1; i < hi; i++) {
        if ((clip.pcm[(i - 1) * 2] < 0) != (clip.pcm[i * 2] < 0)) crossings++;
        if (clip.pcm[i * 2] != clip.pcm[i * 2 + 1]) diff++;
    }
    long hz = crossings * 48000 / (2 * (hi - lo - 1));
    check("the FLAC tone is 1 kHz", hz > 990 && hz < 1010, "wrong frequency");
    eq("its mono upmix filled both channels", diff, 0);
    usnd_clip_free(&clip);
}

// THE OUTPUT RATE IS THE CARD'S: a file at it is not resampled at all
// (every sample exact), and at twice it every source frame lands on an
// even output frame, untouched, with the interpolated one between.
static void check_out_rate(void) {
    struct usnd_stream st;
    if (usnd_open("/tests/ramp24.flac", &st) != 0) { check("open for rates", 0, usnd_last_error()); return; }
    usnd_stream_set_rate(&st, 96000);
    static int32_t got[9600 * 2];
    long n = 0, k;
    while (n < 9600 && (k = usnd_read(&st, got + n * 2, 9600 - n)) > 0) n += k;
    check("at 96 kHz a 48 kHz file is twice the frames", n >= 9599 && n <= 9600, "count off");
    long bad = -1;
    for (long i = 0; i < 4800 && bad < 0; i++) {
        int32_t left = (int32_t)((i * 7919) % 0x1000000) - 0x800000;
        if (got[i * 4] != left * 256) bad = i;
    }
    char d[64];
    snprintf(d, sizeof d, "first wrong source frame %ld", bad);
    check("...each source frame kept exactly on an even one", bad < 0, d);
    usnd_close(&st);

    if (usnd_open("/tests/sine1k.flac", &st) != 0) { check("open the 44.1 kHz FLAC", 0, usnd_last_error()); return; }
    usnd_stream_set_rate(&st, 44100);
    static int32_t buf[4096 * 2];
    long total = 0;
    while ((k = usnd_read(&st, buf, 4096)) > 0) total += k;
    eq("at the file's own 44.1 kHz, exactly its frames", total, 66150);
    eq("...and its length is still told in 48 kHz frames", (long)usnd_stream_frames(&st), 72000);
    usnd_close(&st);
}

// The one rule soundd and the device sink share (lib/usndfmt.h).
static void check_policy(void) {
    struct usndfmt f = { .match = 1, .fixed = 48000,
                         .allowed = SND_RATE_44100 | SND_RATE_48000, .bits = 0 };
    uint32_t card = SND_RATE_44100 | SND_RATE_48000 | SND_RATE_96000;
    eq("match takes an allowed file rate", usndfmt_pick_rate(&f, card, 44100), 44100);
    eq("...not one outside the allowed list", usndfmt_pick_rate(&f, card, 96000), 48000);
    eq("...nor one the card lacks", usndfmt_pick_rate(&f, SND_RATE_48000, 44100), 48000);
    eq("no preference is 48 kHz", usndfmt_pick_rate(&f, card, 0), 48000);
    f.allowed = SND_RATE_44100;
    eq("48 kHz not allowed: the lowest that is", usndfmt_pick_rate(&f, card, 0), 44100);
    f.match = 0;
    f.fixed = 96000;
    eq("a fixed rate holds whatever plays", usndfmt_pick_rate(&f, card, 44100), 96000);
    f.fixed = 192000;
    eq("...unless the card cannot", usndfmt_pick_rate(&f, card, 44100), 48000);
    eq("a card that does not say takes SND_RATE", usndfmt_pick_rate(&f, 0, 44100), 48000);
    f.bits = 24;
    eq("a width the card has is asked for", usndfmt_pick_bits(&f, SND_DEPTH_16 | SND_DEPTH_24), 24);
    eq("...and one it lacks is the deepest", usndfmt_pick_bits(&f, SND_DEPTH_16), 0);
    char b[16];
    check("22.05 kHz is named whole", strcmp(usndfmt_rate_label(22050, b, sizeof b), "22.05 kHz") == 0, b);
    check("11.025 kHz too", strcmp(usndfmt_rate_label(11025, b, sizeof b), "11.025 kHz") == 0, b);
}

int main(void) {
    utest_begin("usnd_test", "the audio DECODE path, with no sound hardware involved", UTEST_QUIET);

    check_header();
    check_chunk_walk();
    check_truncated();
    check_passthrough();
    check_mono_upmix();
    check_8bit();
    check_24bit();
    check_flac();
    check_out_rate();
    check_policy();
    check_resample();
    check_seek();
    check_refusals();
    check_mp3();

    sys_unlink(FIX);
    return utest_end();
}
