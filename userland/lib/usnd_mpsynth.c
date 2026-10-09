// The polyphase synthesis filterbank every MPEG-1 audio layer ends in
// (usnd_internal.h). Moved out of usnd_mp3.c when Layer II arrived: the
// standard defines it once, for all three layers.
#include <math.h>
#include <string.h>
#include "lib/usnd_internal.h"
#include "lib/usnd_mp3_tables.h"

static float g_cos[64][32];
static int g_ready;

static void build(void) {
    if (g_ready) return;
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            g_cos[i][k] = (float)cos((16 + i) * (2 * k + 1) * M_PI / 64.0);
    g_ready = 1;
}

void usnd_mpsynth_reset(struct usnd_mpsynth *s) {
    memset(s->v, 0, sizeof s->v);
    s->pos = 0;
}

// Full scale onto s32. Compared in float BEFORE the cast: a float at or
// past 2^31 converted to an integer is undefined, not clamped.
static int32_t clip32(float v) {
    float x = v * 2147483648.0f;
    if (x >= 2147483647.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return (int32_t)(int64_t)(x + (x >= 0 ? 0.5f : -0.5f));
}

void usnd_mpsynth_run(struct usnd_mpsynth *s, const float *sb, int slots, int32_t *out, int stride) {
    build();
    float *V = s->v;
    for (int slot = 0; slot < slots; slot++) {
        const float *S = sb + slot * 32;
        s->pos = (s->pos + 1024 - 64) & 1023;
        int base = s->pos;
        for (int i = 0; i < 64; i++) {
            float acc = 0.0f;
            for (int k = 0; k < 32; k++) acc += S[k] * g_cos[i][k];
            V[(base + i) & 1023] = acc;
        }
        // U is the 512 taps the window multiplies, gathered out of the
        // 1024-sample history in the standard's own interleave.
        float *u = s->u;
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 32; j++) {
                u[i * 64 + j] = V[(base + i * 128 + j) & 1023];
                u[i * 64 + 32 + j] = V[(base + i * 128 + 96 + j) & 1023];
            }
        }
        for (int j = 0; j < 32; j++) {
            float acc = 0.0f;
            for (int i = 0; i < 16; i++)
                acc += u[j + 32 * i] * mp3_synth_window[j + 32 * i];
            out[(slot * 32 + j) * stride] = clip32(acc);
        }
    }
}
