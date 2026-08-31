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
#include "lib/usnd_internal.h"
#include <stdio.h>
#include <string.h>

static int g_fails;

static void check(const char *what, int ok, const char *detail) {
    if (ok) return;
    g_fails++;
    printf("usnd_test: FAIL %s -- %s\n", what, detail);
}

static void eq(const char *what, long got, long want) {
    if (got == want) return;
    g_fails++;
    printf("usnd_test: FAIL %s -- got %ld, wanted %ld\n", what, got, want);
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

#define FIX "/tmp/usnd_fix.wav"

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
    short got[8];
    long n = usnd_read(&st, got, 4);
    eq("48k stereo frame count", n, 4);
    check("48k stereo is byte-identical", memcmp(got, s, sizeof s) == 0,
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
    short got[8];
    long n = usnd_read(&st, got, 4);
    eq("mono frame count", n, 4);
    int ok = 1;
    for (int i = 0; i < 4; i++)
        if (got[i * 2] != s[i] || got[i * 2 + 1] != s[i]) ok = 0;
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
    short got[8];
    eq("8-bit frame count", usnd_read(&st, got, 4), 4);
    eq("8-bit 128 is silence", got[0], 0);
    check("8-bit 255 is positive", got[2] > 30000, "255 did not map near full scale");
    check("8-bit 0 is negative", got[4] < -30000, "0 did not map near negative full scale");
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

    short got[256];
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
    short whole[64];
    usnd_read(&st, whole, 32);

    check("seek is supported", usnd_seek(&st, 8) == 0, usnd_last_error());
    short after[32];
    long n = usnd_read(&st, after, 8);
    eq("frames after seek", n, 8);
    check("seek lands on the right samples",
          memcmp(after, whole + 8 * 2, 8 * 2 * sizeof(short)) == 0,
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
        int a = clip.pcm[(i - 1) * 2], b = clip.pcm[i * 2];
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

int main(void) {
    check_header();
    check_chunk_walk();
    check_truncated();
    check_passthrough();
    check_mono_upmix();
    check_8bit();
    check_resample();
    check_seek();
    check_refusals();
    check_mp3();

    sys_unlink(FIX);
    printf("usnd_test: %d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
