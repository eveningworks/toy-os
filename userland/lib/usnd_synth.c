// A General MIDI synthesiser over a SoundFont 2 bank.
//
// The SHAPE is FluidSynth's, cut down: a voice per sounding region, its
// envelopes, LFOs, modulators and filter updated once per BLOCK (64
// frames, as FluidSynth does) and interpolated sample by sample inside
// it. What is deliberately not here: reverb and chorus (their sends are
// computed and dropped), linked modulators, and GS/XG SysEx beyond what
// usnd_mid.c resets.
//
// **EVERY CONTROLLER REACHES SOUND THROUGH THE REGION'S MODULATORS.**
// Velocity, volume, expression, pan, the wheel and the bend have no
// paths of their own: they are the SF2 default modulators, merged with
// the bank's in usnd_sf2.c, so a bank that redefines one (a velocity
// curve, a wheel that opens the filter) is played the way it says.
//
// **THE BLOCKS ARE ON A FIXED GRID OF SONG TIME**, not wherever a read
// call happens to start: `left` carries a block across calls, and a
// voice starting mid-block gets a control pass for the part that is
// left. Without that the output depends on how the CALLER chunks its
// reads, and a seek cannot land where continuous playback would have.
// Events therefore take effect at the next block boundary for anything
// already sounding (a key-up, a controller) -- FluidSynth's latency too.
//
// **THE VOLUME ENVELOPE IS LINEAR IN AMPLITUDE DURING ATTACK AND LINEAR
// IN DECIBELS AFTER IT** (SF2 2.04, 8.1.2 generators 34-38). `value` runs
// 0..1 in both, and voice_control() is the only place that knows which
// one it is; a release that starts mid-attack converts between them first, or
// the note jumps in level at the key-up.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd_synth.h"

#define BLOCK       64
#define ENV_DB      96.0        // what "100%" of the volume envelope spans
#define MIN_RELEASE 0.003       // seconds: a shorter key-up clicks
#define STEAL_FADE  0.005       // seconds, for a stolen or cut voice
#define MASTER_GAIN 1.0
#define ATTEN_SCALE 0.4         // see voice_control()

enum { E_DELAY, E_ATTACK, E_HOLD, E_DECAY, E_SUSTAIN, E_RELEASE, E_DONE };

struct env {
    int stage;
    double value;
    long left;                  // samples left in DELAY or HOLD
    long delay, hold;
    double attack_inc, decay_dec, sustain, release_dec;
};

struct lfo {
    double phase, inc;
    long delay;
};

struct voice {
    int active;
    uint32_t age;
    uint8_t chan, key, vel;
    uint8_t mkey;               // the key modulators and pitch see (keynum wins)
    int released, held;
    const struct sf2_region *r;
    uint64_t pos;               // source sample, 32.32 fixed point
    int loop;                   // loops while playing (mode 1, or 3 until key-up)
    struct env vol, mod;
    struct lfo vib, modlfo;
    double rate_ratio;          // sample rate / output rate
    float m[SF2_GEN_COUNT];     // modulator sums, added to the region's generators

    // This block's controls, set by voice_control() at the boundary.
    // The per-sample path is INTEGER: under TCG every SSE instruction is
    // a softfloat helper call, and a float inner loop ran ~30x slower
    // than on the host -- too slow for a dense song on a real bank.
    uint64_t step;              // source samples per output frame, 32.32
    int32_t gl, gr, dgl, dgr;   // gain (amplitude x pan) Q24, and its per-frame ramp
    int32_t b0, b1, b2, a1, a2; // biquad, Q28
    int32_t x1, x2, y1, y2;     // filter state, 24-bit samples
    double fc_set, q_set, pan_set;  // what the coefficients / gains were made for
    double pan_l, pan_r;
    double amp;                 // this block's target amplitude
    int filt;
    int ending;                 // silent by the block's end: free it then
};

struct chan {
    uint8_t cc[128];
    uint8_t program, pressure;
    uint8_t rpn_msb, rpn_lsb;
    uint8_t bend_semis, bend_cents;
    uint16_t bend;              // 0..16383, 8192 = centre
    const struct sf2_preset *preset;
};

struct usynth {
    const struct sf2_bank *bank;
    uint32_t rate;
    uint32_t age;
    struct chan ch[16];
    struct voice v[USYNTH_VOICES];
    int left;                   // frames until the next block boundary
    int32_t mix_l[BLOCK], mix_r[BLOCK];     // 24-bit samples
};

static double tc_seconds(double tc) { return exp2(tc / 1200.0); }

static long tc_samples(const struct usynth *s, double tc) {
    return (long)(tc_seconds(tc) * s->rate + 0.5);
}

// Frequency of an absolute-cents value: 8.176 Hz is MIDI key 0.
static double cents_hz(double c) { return 8.176 * exp2(c / 1200.0); }

// A generator as the voice hears it: the region's plus its modulators.
static double gv(const struct voice *v, int op) { return v->r->gen[op] + v->m[op]; }

// --- channels ---------------------------------------------------------

// Channel 10, plus the GM2 (120) and XG (127) drum banks anywhere.
static int is_drum(const struct usynth *s, int c) {
    return c == 9 || s->ch[c].cc[0] == 120 || s->ch[c].cc[0] == 127;
}

static void pick_preset(struct usynth *s, int c) {
    struct chan *ch = &s->ch[c];
    const struct sf2_preset *p;
    if (is_drum(s, c)) {
        p = sf2_find(s->bank, 128, ch->program);
        if (!p) p = sf2_find(s->bank, 128, 0);
    } else {
        p = sf2_find(s->bank, ch->cc[0], ch->program);
        if (!p) p = sf2_find(s->bank, 0, ch->program);
    }
    ch->preset = p;
}

// GM's Reset All Controllers (RP-015): not volume, pan or the bank.
static void reset_controllers(struct chan *ch) {
    ch->cc[1] = 0;
    ch->cc[11] = 127;
    for (int i = 64; i <= 69; i++) ch->cc[i] = 0;
    ch->pressure = 0;
    ch->bend = 8192;
    ch->rpn_msb = ch->rpn_lsb = 127;
}

static void reset_channel(struct usynth *s, int c) {
    struct chan *ch = &s->ch[c];
    memset(ch, 0, sizeof *ch);
    ch->cc[7] = 100;
    ch->cc[10] = 64;
    ch->bend_semis = 2;
    reset_controllers(ch);
    pick_preset(s, c);
}

// --- modulators -------------------------------------------------------

// The four SF2 curves over 0..1. Concave is GM's 40 log: the default
// velocity modulator's 960 cB through it is gain = (v/127)^2. A table,
// because every voice evaluates a dozen of these each block.
#define CURVE_STEPS 256
static double g_concave[CURVE_STEPS + 1];

static void curve_init(void) {
    if (g_concave[CURVE_STEPS] == 1) return;
    for (int i = 0; i < CURVE_STEPS; i++) {
        double y = -(40.0 / 96.0) * log10(1 - (double)i / CURVE_STEPS);
        g_concave[i] = y > 1 ? 1 : y;
    }
    g_concave[CURVE_STEPS] = 1;
}

static double concave(double x) {
    double f = x * CURVE_STEPS;
    int i = (int)f;
    if (i >= CURVE_STEPS) return 1;
    return g_concave[i] + (g_concave[i + 1] - g_concave[i]) * (f - i);
}

static double curve(int type, double x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    switch (type) {
    case 1: return concave(x);
    case 2: return 1 - concave(1 - x);
    case 3: return x >= 0.5 ? 1 : 0;
    }
    return x;
}

static double source(const struct usynth *s, const struct voice *v, uint16_t src) {
    const struct chan *ch = &s->ch[v->chan];
    int idx = src & 127;
    double x;
    if (src & 0x80) {
        x = ch->cc[idx] / 127.0;
    } else {
        switch (idx) {
        case 0:  x = 1; break;                      // "no controller"
        case 2:  x = v->vel / 127.0; break;
        case 3:  x = v->mkey / 127.0; break;
        case 13: x = ch->pressure / 127.0; break;
        case 14: x = ch->bend / 16384.0; break;
        case 16: x = (ch->bend_semis + ch->bend_cents / 100.0) / 127.0; break;
        default: return 0;                          // poly pressure, links: none here
        }
    }
    if (src & 0x100) x = 1 - x;
    int type = (src >> 10) & 63;
    if (src & 0x200) {
        double y = 2 * x - 1;
        double m = type == 3 ? 1 : curve(type, y < 0 ? -y : y);
        return y < 0 ? -m : m;
    }
    return curve(type, x);
}

static void voice_mods(const struct usynth *s, struct voice *v) {
    memset(v->m, 0, sizeof v->m);
    const struct sf2_mod *m = s->bank->mods + v->r->mod_first;
    for (int i = 0; i < v->r->mod_count; i++, m++) {
        double a = source(s, v, m->src);
        if (a == 0) continue;
        a *= m->amount * source(s, v, m->amt_src);
        if (m->transform == 2 && a < 0) a = -a;
        v->m[m->dest] += (float)a;
    }
}

// --- envelopes --------------------------------------------------------

static void env_start(const struct usynth *s, struct env *e, double delay, double attack,
                      double hold, double decay, double sustain, double release) {
    memset(e, 0, sizeof *e);
    e->delay = tc_samples(s, delay);
    e->hold = tc_samples(s, hold);
    long a = tc_samples(s, attack), d = tc_samples(s, decay), r = tc_samples(s, release);
    long rmin = (long)(MIN_RELEASE * s->rate);
    e->attack_inc = 1.0 / (double)(a > 0 ? a : 1);
    e->decay_dec = 1.0 / (double)(d > 0 ? d : 1);
    e->release_dec = 1.0 / (double)(r > rmin ? r : rmin);
    e->sustain = sustain < 0 ? 0 : sustain > 1 ? 1 : sustain;
    e->stage = E_DELAY;
    e->left = e->delay;
}

// Advance by n samples. Stage changes land on block boundaries, which at
// 64 frames is 1.3 ms -- FluidSynth's granularity too.
static void env_step(struct env *e, long n) {
    switch (e->stage) {
    case E_DELAY:
        e->left -= n;
        if (e->left <= 0) { e->stage = E_ATTACK; e->value = 0; }
        break;
    case E_ATTACK:
        e->value += e->attack_inc * n;
        if (e->value >= 1.0) { e->value = 1.0; e->stage = E_HOLD; e->left = e->hold; }
        break;
    case E_HOLD:
        e->left -= n;
        if (e->left <= 0) e->stage = E_DECAY;
        break;
    case E_DECAY:
        e->value -= e->decay_dec * n;
        if (e->value <= e->sustain) { e->value = e->sustain; e->stage = E_SUSTAIN; }
        break;
    case E_SUSTAIN:
        break;
    case E_RELEASE:
        e->value -= e->release_dec * n;
        if (e->value <= 0) { e->value = 0; e->stage = E_DONE; }
        break;
    }
}

static void vol_release(struct env *e) {
    if (e->stage == E_DELAY) { e->stage = E_DONE; return; }
    if (e->stage == E_ATTACK) {
        // Linear amplitude -> the dB domain release runs in.
        double a = e->value;
        e->value = a > 0 ? 1.0 + 20.0 * log10(a) / ENV_DB : 0;
        if (e->value < 0) e->value = 0;
    }
    if (e->stage != E_DONE) e->stage = E_RELEASE;
}

static void mod_release(struct env *e) {
    if (e->stage != E_DONE) e->stage = E_RELEASE;
}

static double lfo_step(struct lfo *l, long n) {
    if (l->delay > 0) { l->delay -= n; return 0; }
    l->phase += l->inc * n;
    l->phase -= floor(l->phase);
    double p = l->phase;
    // Triangle, starting at 0 and rising, as the spec draws it.
    return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
}

// --- voices -----------------------------------------------------------

static void voice_cut(const struct usynth *s, struct voice *v) {
    vol_release(&v->vol);
    v->vol.release_dec = 1.0 / (STEAL_FADE * s->rate);
    v->released = 1;
    v->held = 0;
    if ((v->r->gen[SF2_SAMPLE_MODES] & 3) == 3) v->loop = 0;
}

static void voice_release(struct voice *v) {
    vol_release(&v->vol);
    mod_release(&v->mod);
    v->released = 1;
    v->held = 0;
    if ((v->r->gen[SF2_SAMPLE_MODES] & 3) == 3) v->loop = 0;
}

static struct voice *voice_alloc(struct usynth *s) {
    struct voice *best = 0;
    for (int i = 0; i < USYNTH_VOICES; i++)
        if (!s->v[i].active) return &s->v[i];
    // Steal: the quietest released voice, else the oldest.
    for (int i = 0; i < USYNTH_VOICES; i++) {
        struct voice *v = &s->v[i];
        if (v->released && (!best || v->amp < best->amp)) best = v;
    }
    if (!best)
        for (int i = 0; i < USYNTH_VOICES; i++)
            if (!best || s->v[i].age < best->age) best = &s->v[i];
    return best;
}

static void voice_control(struct usynth *s, struct voice *v, long n);

static void voice_start(struct usynth *s, int c, int key, int vel,
                        const struct sf2_region *r) {
    struct voice *v = voice_alloc(s);
    const int16_t *g = r->gen;
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->age = ++s->age;
    v->chan = (uint8_t)c;
    v->key = (uint8_t)key;
    v->vel = (uint8_t)(g[SF2_VELOCITY] > 0 && g[SF2_VELOCITY] <= 127 ? g[SF2_VELOCITY] : vel);
    v->mkey = (uint8_t)(g[SF2_KEYNUM] >= 0 && g[SF2_KEYNUM] <= 127 ? g[SF2_KEYNUM] : key);
    v->r = r;
    voice_mods(s, v);

    // A modulated start offset (a velocity-dependent attack, say) is
    // clamped into the region, which usnd_sf2.c already bounds-checked.
    double start = r->start + v->m[SF2_START_OFS] + 32768.0 * v->m[SF2_START_COARSE];
    double last = (r->loop_end > r->loop_start ? r->loop_end : r->end) - 2.0;
    start = start < r->start ? r->start : start > last ? last : start;
    v->pos = (uint64_t)start << 32;
    v->pan_set = 99;                    // no pan computed yet
    v->fc_set = -1;
    int mode = g[SF2_SAMPLE_MODES] & 3;
    v->loop = r->loop_end > r->loop_start && (mode == 1 || mode == 3);
    v->rate_ratio = (double)r->rate / s->rate;

    int kd = 60 - v->mkey;
    env_start(s, &v->vol, gv(v, SF2_VOLENV_DELAY), gv(v, SF2_VOLENV_ATTACK),
              gv(v, SF2_VOLENV_HOLD) + gv(v, SF2_KEY_VOLENV_HOLD) * kd,
              gv(v, SF2_VOLENV_DECAY) + gv(v, SF2_KEY_VOLENV_DECAY) * kd,
              1.0 - gv(v, SF2_VOLENV_SUSTAIN) / (10.0 * ENV_DB), gv(v, SF2_VOLENV_RELEASE));
    env_start(s, &v->mod, gv(v, SF2_MODENV_DELAY), gv(v, SF2_MODENV_ATTACK),
              gv(v, SF2_MODENV_HOLD) + gv(v, SF2_KEY_MODENV_HOLD) * kd,
              gv(v, SF2_MODENV_DECAY) + gv(v, SF2_KEY_MODENV_DECAY) * kd,
              1.0 - gv(v, SF2_MODENV_SUSTAIN) / 1000.0, gv(v, SF2_MODENV_RELEASE));

    v->vib.inc = cents_hz(gv(v, SF2_VIBLFO_FREQ)) / s->rate;
    v->vib.delay = tc_samples(s, gv(v, SF2_VIBLFO_DELAY));
    v->modlfo.inc = cents_hz(gv(v, SF2_MODLFO_FREQ)) / s->rate;
    v->modlfo.delay = tc_samples(s, gv(v, SF2_MODLFO_DELAY));

    if (s->left) voice_control(s, v, s->left);      // the rest of this block
}

static void note_off(struct usynth *s, int c, int key) {
    for (int i = 0; i < USYNTH_VOICES; i++) {
        struct voice *v = &s->v[i];
        if (!v->active || v->released || v->chan != c || v->key != key) continue;
        if (s->ch[c].cc[64] >= 64) v->held = 1;
        else voice_release(v);
    }
}

static void note_on(struct usynth *s, int c, int key, int vel) {
    const struct sf2_preset *p = s->ch[c].preset;
    if (!p) return;
    const struct sf2_region *rs = s->bank->regions + p->first;

    // A repeated key on a melodic channel releases the note it repeats,
    // rather than stacking a second copy on it.
    if (!is_drum(s, c))
        for (int i = 0; i < USYNTH_VOICES; i++) {
            struct voice *v = &s->v[i];
            if (v->active && !v->released && v->chan == c && v->key == key)
                voice_release(v);
        }

    // Exclusive classes (an open hi-hat choked by a closed one) are cut
    // for every region this note starts BEFORE any of them starts, so a
    // stereo pair sharing a class cannot cut its own other half.
    for (uint32_t i = 0; i < p->count; i++) {
        const struct sf2_region *r = &rs[i];
        int excl = r->gen[SF2_EXCLUSIVE];
        if (!excl || key < r->key_lo || key > r->key_hi || vel < r->vel_lo || vel > r->vel_hi)
            continue;
        for (int j = 0; j < USYNTH_VOICES; j++) {
            struct voice *v = &s->v[j];
            if (v->active && v->chan == c && v->r->gen[SF2_EXCLUSIVE] == excl)
                voice_cut(s, v);
        }
    }
    for (uint32_t i = 0; i < p->count; i++) {
        const struct sf2_region *r = &rs[i];
        if (key >= r->key_lo && key <= r->key_hi && vel >= r->vel_lo && vel <= r->vel_hi)
            voice_start(s, c, key, vel, r);
    }
}

static void controller(struct usynth *s, int c, int cc, int val) {
    struct chan *ch = &s->ch[c];
    ch->cc[cc] = (uint8_t)val;
    switch (cc) {
    case 0: pick_preset(s, c); break;
    case 6:
        if (ch->rpn_msb == 0 && ch->rpn_lsb == 0) ch->bend_semis = (uint8_t)(val > 24 ? 24 : val);
        break;
    case 38:
        if (ch->rpn_msb == 0 && ch->rpn_lsb == 0) ch->bend_cents = (uint8_t)(val > 99 ? 99 : val);
        break;
    case 64:
        if (val < 64)
            for (int i = 0; i < USYNTH_VOICES; i++) {
                struct voice *v = &s->v[i];
                if (v->active && v->chan == c && v->held) voice_release(v);
            }
        break;
    case 98: case 99: ch->rpn_msb = ch->rpn_lsb = 127; break;   // an NRPN: data entry is not ours
    case 100: ch->rpn_lsb = (uint8_t)val; break;
    case 101: ch->rpn_msb = (uint8_t)val; break;
    case 120:
        for (int i = 0; i < USYNTH_VOICES; i++)
            if (s->v[i].active && s->v[i].chan == c) voice_cut(s, &s->v[i]);
        break;
    case 121: reset_controllers(ch); break;
    case 123: case 124: case 125: case 126: case 127:
        for (int i = 0; i < USYNTH_VOICES; i++)
            if (s->v[i].active && s->v[i].chan == c && !s->v[i].released)
                voice_release(&s->v[i]);
        break;
    }
}

void usynth_message(struct usynth *s, uint8_t status, uint8_t d1, uint8_t d2) {
    int c = status & 15;
    d1 &= 127;
    d2 &= 127;
    switch (status & 0xF0) {
    case 0x80: note_off(s, c, d1); break;
    case 0x90: if (d2) note_on(s, c, d1, d2); else note_off(s, c, d1); break;
    case 0xB0: controller(s, c, d1, d2); break;
    case 0xC0: s->ch[c].program = d1; pick_preset(s, c); break;
    case 0xD0: s->ch[c].pressure = d1; break;
    case 0xE0: s->ch[c].bend = (uint16_t)(d1 | d2 << 7); break;
    }
}

// --- rendering --------------------------------------------------------

// Decibels to a linear gain, through exp2: one libm call, and the only
// transcendental most blocks need.
static double db_gain(double db) { return exp2(db * (3.321928094887362 / 20.0)); }

// Q28 biquad coefficients, recomputed only when the cutoff moves by more
// than a cent or Q changes -- most voices hold one filter for life.
static void set_filter(struct usynth *s, struct voice *v, double fc_cents, double q_cb) {
    if (fabs(fc_cents - v->fc_set) < 1.0 && q_cb == v->q_set) return;
    v->fc_set = fc_cents;
    v->q_set = q_cb;
    double hz = cents_hz(fc_cents);
    if (hz > 0.45 * s->rate) hz = 0.45 * s->rate;
    if (hz < 20) hz = 20;
    // Q in centibels over the no-resonance point, and the level dropped
    // by the resonance peak so a resonant filter is not simply louder --
    // both FluidSynth's.
    double q = db_gain(q_cb / 10.0 - 3.01);
    double w = 2 * M_PI * hz / s->rate;
    double cw = cos(w), alpha = sin(w) / (2 * q);
    double a0 = 1 + alpha;
    double comp = 1.0 / sqrt(q > 1 ? q : 1);
    const double one = 268435456.0;     // 1.0 in Q28
    v->b0 = (int32_t)floor((1 - cw) / 2 / a0 * comp * one + 0.5);
    v->b1 = (int32_t)floor((1 - cw) / a0 * comp * one + 0.5);
    v->b2 = v->b0;
    v->a1 = (int32_t)floor(-2 * cw / a0 * one + 0.5);
    v->a2 = (int32_t)floor((1 - alpha) / a0 * one + 0.5);
}

// A voice's controls for the next n frames: modulators re-evaluated,
// envelopes and LFOs stepped, pitch, filter, pan and the gains this
// block ramps to.
static void voice_control(struct usynth *s, struct voice *v, long n) {
    voice_mods(s, v);
    env_step(&v->vol, n);
    env_step(&v->mod, n);
    double vib = lfo_step(&v->vib, n);
    double mlfo = lfo_step(&v->modlfo, n);
    // The MODULATION envelope's attack is CONVEX (spec, generator 26):
    // a filter sweep opens fast and then slows. Linear, a swept pad
    // stays dark for most of its attack.
    double menv = v->mod.stage == E_DELAY ? 0
                : v->mod.stage == E_ATTACK ? curve(2, v->mod.value) : v->mod.value;
    const struct sf2_region *r = v->r;

    // The bend is a default modulator on fine tune, so it is in here.
    double cents = (v->mkey - r->root) * gv(v, SF2_SCALE_TUNING)
                 + gv(v, SF2_COARSE_TUNE) * 100.0 + gv(v, SF2_FINE_TUNE) + r->correction
                 + menv * gv(v, SF2_MODENV_PITCH)
                 + vib * gv(v, SF2_VIBLFO_PITCH)
                 + mlfo * gv(v, SF2_MODLFO_PITCH);
    double step = v->rate_ratio * exp2(cents / 1200.0);
    v->step = (uint64_t)((step > 64 ? 64 : step) * 4294967296.0);

    double fc = gv(v, SF2_FILTER_FC) + menv * gv(v, SF2_MODENV_FC) + mlfo * gv(v, SF2_MODLFO_FC);
    v->filt = v->filt || cents_hz(fc) < 0.45 * s->rate;
    if (v->filt) set_filter(s, v, fc, gv(v, SF2_FILTER_Q));

    // THE ATTENUATION GENERATOR COUNTS 0.4 dB PER UNIT, not the spec's
    // centibel: the EMU8000 did, FluidSynth does (ALT_ATTENUATION_SCALE),
    // and real banks are voiced against it -- read literally, a bank's
    // quiet instruments come out 2.5x too quiet. What the modulators add
    // (velocity, CC7, CC11) is in true centibels.
    double att = r->gen[SF2_ATTENUATION] * ATTEN_SCALE + v->m[SF2_ATTENUATION];
    att = att < 0 ? 0 : att > 1440 ? 1440 : att;
    double lv = gv(v, SF2_MODLFO_VOL);
    double db = -att / 10.0 - (lv ? mlfo * lv / 10.0 : 0);
    double amp;
    const struct env *e = &v->vol;
    if (e->stage == E_DELAY || e->stage == E_DONE) amp = 0;
    else if (e->stage == E_ATTACK) amp = e->value * db_gain(db);
    else amp = e->value > 0 ? db_gain((e->value - 1.0) * ENV_DB + db) : 0;
    v->ending = e->stage == E_DONE || (e->stage >= E_DECAY && e->value <= 0) ||
                (v->released && amp < 1e-5);
    if (v->ending) amp = 0;
    v->amp = amp;

    // Constant-power pan, -500 (left) .. 500 (right).
    double p = gv(v, SF2_PAN) / 1000.0;
    p = p < -0.5 ? -0.5 : p > 0.5 ? 0.5 : p;
    if (p != v->pan_set) {
        v->pan_set = p;
        v->pan_l = cos((p + 0.5) * M_PI / 2);
        v->pan_r = sin((p + 0.5) * M_PI / 2);
    }
    const double one = 16777216.0 * MASTER_GAIN;    // Q24
    int32_t tl = (int32_t)(amp * v->pan_l * one), tr = (int32_t)(amp * v->pan_r * one);
    v->dgl = (tl - v->gl) / (int32_t)n;
    v->dgr = (tr - v->gr) / (int32_t)n;
}

// The per-sample loop, integer throughout: 24-bit samples, a 32.32
// position, a Q24 gain per channel.
static void voice_run(struct usynth *s, struct voice *v, long n) {
    const int16_t *data = s->bank->data;
    const struct sf2_region *r = v->r;
    uint64_t pos = v->pos, step = v->step;
    uint64_t le = (uint64_t)r->loop_end << 32;
    uint64_t len = (uint64_t)(r->loop_end - r->loop_start) << 32;
    uint64_t end = (uint64_t)(r->end - 1) << 32;
    int32_t gl = v->gl, gr = v->gr, dgl = v->dgl, dgr = v->dgr;
    int32_t x1 = v->x1, x2 = v->x2, y1 = v->y1, y2 = v->y2;
    int filt = v->filt, loop = v->loop;

    for (long i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)(pos >> 32);
        int32_t fr = (int32_t)((pos >> 17) & 0x7FFF);  // 15 bits: the product fits int32
        int32_t s0 = data[idx], s1;
        // Interpolating across the loop seam must read the loop START,
        // not whatever lies past the loop end.
        if (loop && idx + 1 >= r->loop_end) s1 = data[r->loop_start + (idx + 1 - r->loop_end)];
        else s1 = data[idx + 1];
        int32_t x = s0 * 256 + (((s1 - s0) * fr) >> 7);     // not << 8: s0 is signed
        if (filt) {
            int64_t acc = (int64_t)v->b0 * x + (int64_t)v->b1 * x1 + (int64_t)v->b2 * x2
                        - (int64_t)v->a1 * y1 - (int64_t)v->a2 * y2;
            int64_t y = acc >> 28;
            // A resonant filter can ring past full scale: 18 dB of room,
            // then clamp, so the mix sum cannot overflow.
            if (y > (1 << 26)) y = 1 << 26;
            if (y < -(1 << 26)) y = -(1 << 26);
            x2 = x1; x1 = x;
            y2 = y1; y1 = (int32_t)y;
            x = (int32_t)y;
        }
        s->mix_l[i] += (int32_t)(((int64_t)x * gl) >> 24);
        s->mix_r[i] += (int32_t)(((int64_t)x * gr) >> 24);
        gl += dgl;
        gr += dgr;

        pos += step;
        if (loop) {
            while (pos >= le) pos -= len;
        } else if (pos >= end) {
            v->active = 0;              // the sample ran out
            break;
        }
    }
    v->pos = pos;
    v->gl = gl;
    v->gr = gr;
    v->x1 = x1; v->x2 = x2; v->y1 = y1; v->y2 = y2;
}

// 24-bit mix to 16-bit output, with a soft knee above 0.75 full scale: a
// dense passage bends rather than clipping into square waves.
static int16_t to_s16(int32_t m) {
    int32_t v = m >> 8;
    if (v > 24575 || v < -24575) {
        double x = v / 32767.0, a = x < 0 ? -x : x;
        double over = (a - 0.75) / 0.25;
        double t = over > 3 ? 1 : over * (27 + over * over) / (27 + 9 * over * over);
        a = 0.75 + 0.25 * t;
        v = (int32_t)((x < 0 ? -a : a) * 32767.0);
    }
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

void usynth_render(struct usynth *s, int16_t *out, long frames) {
    while (frames > 0) {
        if (s->left == 0) {
            for (int i = 0; i < USYNTH_VOICES; i++)
                if (s->v[i].active) voice_control(s, &s->v[i], BLOCK);
            s->left = BLOCK;
        }
        long n = frames < s->left ? frames : s->left;
        memset(s->mix_l, 0, (size_t)n * sizeof s->mix_l[0]);
        memset(s->mix_r, 0, (size_t)n * sizeof s->mix_r[0]);
        for (int i = 0; i < USYNTH_VOICES; i++)
            if (s->v[i].active) voice_run(s, &s->v[i], n);
        for (long i = 0; i < n; i++) {
            out[i * 2]     = to_s16(s->mix_l[i]);
            out[i * 2 + 1] = to_s16(s->mix_r[i]);
        }
        out += n * 2;
        frames -= n;
        s->left -= (int)n;
        if (s->left == 0)
            for (int i = 0; i < USYNTH_VOICES; i++)
                if (s->v[i].ending) s->v[i].active = 0;
    }
}

void usynth_align(struct usynth *s, uint64_t frame) {
    s->left = (int)((BLOCK - frame % BLOCK) % BLOCK);
}

int usynth_active_voices(const struct usynth *s) {
    int n = 0;
    for (int i = 0; i < USYNTH_VOICES; i++) n += s->v[i].active;
    return n;
}

void usynth_reset(struct usynth *s) {
    for (int i = 0; i < USYNTH_VOICES; i++) s->v[i].active = 0;
    for (int c = 0; c < 16; c++) reset_channel(s, c);
}

struct usynth *usynth_new(const struct sf2_bank *bank, uint32_t rate) {
    struct usynth *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->bank = bank;
    s->rate = rate;
    curve_init();
    usynth_reset(s);
    return s;
}

void usynth_free(struct usynth *s) { free(s); }
