// SoundFont 2 banks: the RIFF walk, the pdta tables, and the flattening
// of presets x instruments into regions.
//
// **THIS PARSES UNTRUSTED INPUT.** Every table index is checked before
// it is followed, and a structural fault (a chunk past the end, a bag
// list that runs backwards, a zone naming an instrument that does not
// exist) REFUSES the bank. A single region whose sample addresses fall
// outside the sample chunk is DROPPED instead -- real banks carry the odd
// one, and dropping it is not a guess about what it meant.
//
// **MODULATORS ARE MERGED HERE, EVALUATED IN THE SYNTH.** Every region
// starts from the spec's DEFAULT list (FluidSynth's version of it --
// velocity and CC7/CC11 to attenuation, the wheel to vibrato, CC10 to
// pan, the bend to pitch); an IDENTICAL instrument modulator (same
// sources, destination and transform) REPLACES a default, and a preset
// modulator ADDS to an identical one or appends (spec 9.5). Real banks
// lean on this -- GeneralUser GS sets each instrument's velocity curve
// and velocity-driven filter sweep this way -- so ignoring the lists, as
// TinySoundFont does, plays a bank audibly wrong. Linked modulators
// (a destination with bit 15 set) are dropped.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lib/usnd_sf2.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"
#include "errno.h"

// Zeroed samples after the chunk, so interpolation reading one past a
// sample's last point never leaves the buffer. The spec already puts 46
// zeroes after every sample; this covers a bank that does not.
#define TAIL_SAMPLES 64

// Refuse, rather than try to allocate, anything past this. FluidR3_GM is
// 141 MB; nothing a player should load is larger.
#define MAX_SMPL_BYTES (256u << 20)
#define MAX_TABLE_BYTES (16u << 20)

struct span { const uint8_t *p; uint32_t n; };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// A sub-chunk of a LIST body, by id. 0 when absent or malformed.
static int find_chunk(struct span list, const char *id, struct span *out) {
    uint32_t off = 0;
    while (list.n - off >= 8 && off <= list.n) {
        uint32_t sz = rd32(list.p + off + 4);
        if (sz > list.n - off - 8) return 0;
        if (memcmp(list.p + off, id, 4) == 0) {
            out->p = list.p + off + 8;
            out->n = sz;
            return 1;
        }
        off += 8 + sz + (sz & 1);
    }
    return 0;
}

// --- the pdta tables --------------------------------------------------

struct tables {
    struct span phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    uint32_t nphdr, npbag, npmod, npgen, ninst, nibag, nimod, nigen, nshdr;
};

static int table(struct span pdta, const char *id, uint32_t rec,
                 struct span *s, uint32_t *count, uint32_t min) {
    if (!find_chunk(pdta, id, s) || s->n % rec != 0) return 0;
    *count = s->n / rec;
    return *count >= min;
}

// Each bag's second field indexes a modulator table, under the same
// monotonic, in-range rule as its generator index.
static int mods_ok(const struct span *bags, uint32_t nbags, uint32_t nmods) {
    for (uint32_t i = 0; i < nbags; i++) {
        uint32_t m = rd16(bags->p + i * 4 + 2);
        if (m >= nmods || (i && m < rd16(bags->p + (i - 1) * 4 + 2))) return 0;
    }
    return 1;
}

static int read_tables(struct span pdta, struct tables *t) {
    // Every table ends in a terminal record, so a real one has two --
    // except the modulator tables, which may hold only the terminal.
    if (!table(pdta, "phdr", 38, &t->phdr, &t->nphdr, 2) ||
        !table(pdta, "pbag", 4, &t->pbag, &t->npbag, 2) ||
        !table(pdta, "pmod", 10, &t->pmod, &t->npmod, 1) ||
        !table(pdta, "pgen", 4, &t->pgen, &t->npgen, 2) ||
        !table(pdta, "inst", 22, &t->inst, &t->ninst, 2) ||
        !table(pdta, "ibag", 4, &t->ibag, &t->nibag, 2) ||
        !table(pdta, "imod", 10, &t->imod, &t->nimod, 1) ||
        !table(pdta, "igen", 4, &t->igen, &t->nigen, 2) ||
        !table(pdta, "shdr", 46, &t->shdr, &t->nshdr, 2))
        return 0;
    if (!mods_ok(&t->pbag, t->npbag, t->npmod) || !mods_ok(&t->ibag, t->nibag, t->nimod))
        return 0;

    // Bag and generator indices must be monotonic and in range, which is
    // what makes every [this, next) walk below safe without a recheck.
    for (uint32_t i = 0; i < t->nphdr; i++) {
        uint32_t b = rd16(t->phdr.p + i * 38 + 24);
        if (b >= t->npbag) return 0;
        if (i && b < rd16(t->phdr.p + (i - 1) * 38 + 24)) return 0;
    }
    for (uint32_t i = 0; i < t->ninst; i++) {
        uint32_t b = rd16(t->inst.p + i * 22 + 20);
        if (b >= t->nibag) return 0;
        if (i && b < rd16(t->inst.p + (i - 1) * 22 + 20)) return 0;
    }
    for (uint32_t i = 0; i < t->npbag; i++) {
        uint32_t g = rd16(t->pbag.p + i * 4);
        if (g >= t->npgen || (i && g < rd16(t->pbag.p + (i - 1) * 4))) return 0;
    }
    for (uint32_t i = 0; i < t->nibag; i++) {
        uint32_t g = rd16(t->ibag.p + i * 4);
        if (g >= t->nigen || (i && g < rd16(t->ibag.p + (i - 1) * 4))) return 0;
    }
    return 1;
}

// --- zones ------------------------------------------------------------

struct zone {
    int16_t gen[SF2_GEN_COUNT];
    uint8_t set[SF2_GEN_COUNT];     // which gens this zone (or its global) named
    int key_lo, key_hi, vel_lo, vel_hi;
    int target;                     // instrument / sample index; -1 = global
    struct sf2_mod mod[SF2_MAX_MODS];
    int nmod;
};

// The default modulators, as FluidSynth ships them. The velocity-to-
// filter one only acts BELOW velocity 64 (its amount source is a
// negative switch), which is FluidSynth's reading of an SF2.01 default
// that darkened every loud note.
static const struct sf2_mod DEFAULT_MODS[] = {
    { 0x0502, SF2_ATTENUATION,  0x0000, 0,   960 },  // velocity, concave
    { 0x0102, SF2_FILTER_FC,    0x0D02, 0, -2400 },  // velocity, soft notes only
    { 0x000D, SF2_VIBLFO_PITCH, 0x0000, 0,    50 },  // channel pressure
    { 0x0081, SF2_VIBLFO_PITCH, 0x0000, 0,    50 },  // CC1, the wheel
    { 0x0587, SF2_ATTENUATION,  0x0000, 0,   960 },  // CC7 volume
    { 0x028A, SF2_PAN,          0x0000, 0,   500 },  // CC10 pan
    { 0x058B, SF2_ATTENUATION,  0x0000, 0,   960 },  // CC11 expression
    { 0x00DB, 16,               0x0000, 0,   200 },  // CC91 reverb send
    { 0x00DD, 15,               0x0000, 0,   200 },  // CC93 chorus send
    { 0x020E, SF2_FINE_TUNE,    0x0010, 0, 12700 },  // bend x its range
};

static int mod_same(const struct sf2_mod *a, const struct sf2_mod *b) {
    return a->src == b->src && a->dest == b->dest &&
           a->amt_src == b->amt_src && a->transform == b->transform;
}

// Add `m` to a list: an identical modulator is REPLACED (`sum` = 0, the
// instrument rule) or has its amount ADDED to (`sum` = 1, the preset
// rule); anything else is appended while there is room.
static void mod_put(struct sf2_mod *list, int *n, const struct sf2_mod *m, int sum) {
    for (int i = 0; i < *n; i++)
        if (mod_same(&list[i], m)) {
            long a = sum ? (long)list[i].amount + m->amount : m->amount;
            list[i].amount = (int16_t)(a < -32768 ? -32768 : a > 32767 ? 32767 : a);
            return;
        }
    if (*n < SF2_MAX_MODS) list[(*n)++] = *m;
}

static void apply_mods(const struct span *mods, uint32_t m0, uint32_t m1, struct zone *z) {
    for (uint32_t i = m0; i < m1; i++) {
        const uint8_t *p = mods->p + i * 10;
        struct sf2_mod m = { rd16(p), rd16(p + 2), rd16(p + 6), rd16(p + 8),
                             (int16_t)rd16(p + 4) };
        // A linked modulator feeds another modulator, not a generator.
        if (m.dest & 0x8000 || (m.src & 0xFF) == 127 || m.dest >= SF2_GEN_COUNT) continue;
        mod_put(z->mod, &z->nmod, &m, 0);
    }
}

// Instrument-level defaults, from the spec's generator table.
static void inst_defaults(struct zone *z) {
    memset(z, 0, sizeof *z);
    for (unsigned i = 0; i < sizeof DEFAULT_MODS / sizeof DEFAULT_MODS[0]; i++)
        z->mod[z->nmod++] = DEFAULT_MODS[i];
    static const uint8_t timecents[] = {
        SF2_MODLFO_DELAY, SF2_VIBLFO_DELAY,
        SF2_MODENV_DELAY, SF2_MODENV_ATTACK, SF2_MODENV_HOLD,
        SF2_MODENV_DECAY, SF2_MODENV_RELEASE,
        SF2_VOLENV_DELAY, SF2_VOLENV_ATTACK, SF2_VOLENV_HOLD,
        SF2_VOLENV_DECAY, SF2_VOLENV_RELEASE,
    };
    for (unsigned i = 0; i < sizeof timecents; i++) z->gen[timecents[i]] = -12000;
    z->gen[SF2_FILTER_FC] = 13500;
    z->gen[SF2_KEYNUM] = -1;
    z->gen[SF2_VELOCITY] = -1;
    z->gen[SF2_SCALE_TUNING] = 100;
    z->gen[SF2_ROOT_KEY] = -1;
    z->key_lo = z->vel_lo = 0;
    z->key_hi = z->vel_hi = 127;
    z->target = -1;
}

// Apply one bag's generators on top of `z`. The terminal generator
// (instrument or sampleID) ends the list; anything after it is ignored,
// as the spec says.
static void apply_bag(const struct span *gens, uint32_t g0, uint32_t g1,
                      int terminal, struct zone *z) {
    for (uint32_t g = g0; g < g1; g++) {
        const uint8_t *p = gens->p + g * 4;
        uint16_t op = rd16(p);
        if (op == terminal) { z->target = rd16(p + 2); return; }
        if (op == SF2_KEY_RANGE) { z->key_lo = p[2]; z->key_hi = p[3]; continue; }
        if (op == SF2_VEL_RANGE) { z->vel_lo = p[2]; z->vel_hi = p[3]; continue; }
        if (op >= SF2_GEN_COUNT) continue;
        z->gen[op] = (int16_t)rd16(p + 2);
        z->set[op] = 1;
    }
}

// Generators a PRESET zone may not change (spec 8.5): addresses, fixed
// key/velocity, sample modes, exclusive class and root key.
static int preset_may_set(int op) {
    switch (op) {
    case SF2_START_OFS: case SF2_END_OFS: case SF2_LSTART_OFS: case SF2_LEND_OFS:
    case SF2_START_COARSE: case SF2_END_COARSE: case SF2_LSTART_COARSE:
    case SF2_LEND_COARSE: case SF2_KEYNUM: case SF2_VELOCITY:
    case SF2_SAMPLE_MODES: case SF2_EXCLUSIVE: case SF2_ROOT_KEY:
        return 0;
    }
    return 1;
}

static int16_t clamp16(long v) {
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

// --- building ---------------------------------------------------------

// The four zones in play while a preset is walked, on the HEAP: with
// their modulator lists they are ~500 bytes each, and four on the stack
// is past the ring-3 frame budget.
struct zones { struct zone pglobal, pz, iglobal, iz; };

struct builder {
    struct sf2_bank *b;
    uint32_t cap, mod_cap;
    struct zones *z;
};

static int push_mods(struct builder *bd, const struct sf2_mod *m, int n, struct sf2_region *r) {
    struct sf2_bank *b = bd->b;
    if (b->nmods + (uint32_t)n > bd->mod_cap) {
        uint32_t cap = bd->mod_cap ? bd->mod_cap * 2 : 1024;
        while (cap < b->nmods + (uint32_t)n) cap *= 2;
        struct sf2_mod *nm = realloc(b->mods, (size_t)cap * sizeof *nm);
        if (!nm) return -ENOMEM;
        b->mods = nm;
        bd->mod_cap = cap;
    }
    memcpy(b->mods + b->nmods, m, (size_t)n * sizeof *m);
    r->mod_first = b->nmods;
    r->mod_count = (uint16_t)n;
    b->nmods += (uint32_t)n;
    return 0;
}

static int push_region(struct builder *bd, const struct sf2_region *r) {
    struct sf2_bank *b = bd->b;
    if (b->nregions == bd->cap) {
        uint32_t cap = bd->cap ? bd->cap * 2 : 256;
        struct sf2_region *n = realloc(b->regions, (size_t)cap * sizeof *n);
        if (!n) return -ENOMEM;
        b->regions = n;
        bd->cap = cap;
    }
    b->regions[b->nregions++] = *r;
    return 0;
}

// One instrument zone under one preset zone. 0 = added or dropped,
// negative = out of memory.
static int make_region(struct builder *bd, const struct tables *t,
                       const struct zone *pz, const struct zone *iz) {
    struct sf2_region r;
    memset(&r, 0, sizeof r);

    int klo = pz->key_lo > iz->key_lo ? pz->key_lo : iz->key_lo;
    int khi = pz->key_hi < iz->key_hi ? pz->key_hi : iz->key_hi;
    int vlo = pz->vel_lo > iz->vel_lo ? pz->vel_lo : iz->vel_lo;
    int vhi = pz->vel_hi < iz->vel_hi ? pz->vel_hi : iz->vel_hi;
    if (klo > khi || vlo > vhi || khi > 127 || vhi > 127) return 0;
    r.key_lo = (uint8_t)klo; r.key_hi = (uint8_t)khi;
    r.vel_lo = (uint8_t)vlo; r.vel_hi = (uint8_t)vhi;

    for (int op = 0; op < SF2_GEN_COUNT; op++) {
        long v = iz->gen[op];
        if (pz->set[op] && preset_may_set(op)) v += pz->gen[op];
        r.gen[op] = clamp16(v);
    }

    int sid = iz->target;
    if (sid < 0 || (uint32_t)sid >= t->nshdr - 1) return 0;
    const uint8_t *sh = t->shdr.p + (uint32_t)sid * 46;
    uint16_t type = rd16(sh + 44);
    if (type & 0x8000) return 0;                // ROM sample: no data here

    // Offsets in 64-bit, so a hostile coarse offset cannot wrap an
    // address back into range.
    int64_t start = (int64_t)rd32(sh + 20) + r.gen[SF2_START_OFS] + 32768LL * r.gen[SF2_START_COARSE];
    int64_t end   = (int64_t)rd32(sh + 24) + r.gen[SF2_END_OFS]   + 32768LL * r.gen[SF2_END_COARSE];
    int64_t ls    = (int64_t)rd32(sh + 28) + r.gen[SF2_LSTART_OFS] + 32768LL * r.gen[SF2_LSTART_COARSE];
    int64_t le    = (int64_t)rd32(sh + 32) + r.gen[SF2_LEND_OFS]   + 32768LL * r.gen[SF2_LEND_COARSE];
    uint32_t rate = rd32(sh + 36);
    if (start < 0 || end > (int64_t)bd->b->nsamples || end - start < 2) return 0;
    if (rate < 400 || rate > 1000000) return 0;
    r.start = (uint32_t)start;
    r.end = (uint32_t)end;
    int mode = r.gen[SF2_SAMPLE_MODES] & 3;
    if ((mode == 1 || mode == 3) && ls >= start && le <= end && le - ls >= 2) {
        r.loop_start = (uint32_t)ls;
        r.loop_end = (uint32_t)le;
    }
    r.rate = rate;
    int root = r.gen[SF2_ROOT_KEY] >= 0 && r.gen[SF2_ROOT_KEY] <= 127
             ? r.gen[SF2_ROOT_KEY] : sh[40];
    r.root = (uint8_t)(root > 127 ? 60 : root);
    r.correction = (int8_t)sh[41];

    // The instrument's list (defaults already folded in), with the
    // preset's modulators ADDED -- spec 9.5.
    struct sf2_mod mods[SF2_MAX_MODS];
    int n = iz->nmod;
    memcpy(mods, iz->mod, (size_t)n * sizeof mods[0]);
    for (int i = 0; i < pz->nmod; i++) mod_put(mods, &n, &pz->mod[i], 1);
    int rc = push_mods(bd, mods, n, &r);
    return rc ? rc : push_region(bd, &r);
}

static int build_preset(struct builder *bd, const struct tables *t, uint32_t p) {
    const uint8_t *ph = t->phdr.p + p * 38;
    uint32_t b0 = rd16(ph + 24), b1 = rd16(ph + 38 + 24);
    struct sf2_preset pr;
    memset(&pr, 0, sizeof pr);
    memcpy(pr.name, ph, 20);
    pr.program = rd16(ph + 20);
    pr.bank = rd16(ph + 22);
    pr.first = bd->b->nregions;

    struct zones *z = bd->z;
    memset(&z->pglobal, 0, sizeof z->pglobal);
    z->pglobal.key_hi = z->pglobal.vel_hi = 127;
    z->pglobal.target = -1;

    for (uint32_t bag = b0; bag < b1; bag++) {
        uint32_t g0 = rd16(t->pbag.p + bag * 4), g1 = rd16(t->pbag.p + (bag + 1) * 4);
        uint32_t m0 = rd16(t->pbag.p + bag * 4 + 2), m1 = rd16(t->pbag.p + (bag + 1) * 4 + 2);
        z->pz = z->pglobal;
        z->pz.target = -1;
        apply_bag(&t->pgen, g0, g1, SF2_INSTRUMENT, &z->pz);
        apply_mods(&t->pmod, m0, m1, &z->pz);
        if (z->pz.target < 0) {
            if (bag == b0) z->pglobal = z->pz;  // the first zone may be global
            continue;
        }
        if ((uint32_t)z->pz.target >= t->ninst - 1) return -EINVAL;

        const uint8_t *in = t->inst.p + (uint32_t)z->pz.target * 22;
        uint32_t ib0 = rd16(in + 20), ib1 = rd16(in + 22 + 20);
        inst_defaults(&z->iglobal);
        for (uint32_t ib = ib0; ib < ib1; ib++) {
            uint32_t h0 = rd16(t->ibag.p + ib * 4), h1 = rd16(t->ibag.p + (ib + 1) * 4);
            uint32_t n0 = rd16(t->ibag.p + ib * 4 + 2), n1 = rd16(t->ibag.p + (ib + 1) * 4 + 2);
            z->iz = z->iglobal;
            z->iz.target = -1;
            apply_bag(&t->igen, h0, h1, SF2_SAMPLE_ID, &z->iz);
            apply_mods(&t->imod, n0, n1, &z->iz);
            if (z->iz.target < 0) {
                if (ib == ib0) z->iglobal = z->iz;
                continue;
            }
            int rc = make_region(bd, t, &z->pz, &z->iz);
            if (rc < 0) return rc;
        }
    }
    pr.count = bd->b->nregions - pr.first;

    struct sf2_bank *b = bd->b;
    struct sf2_preset *n = realloc(b->presets, (size_t)(b->npresets + 1) * sizeof *n);
    if (!n) return -ENOMEM;
    b->presets = n;
    b->presets[b->npresets++] = pr;
    return 0;
}

// Takes ownership of `samples` (nsamples plus TAIL_SAMPLES, zero tail).
static int build(struct span info, struct span pdta, int16_t *samples,
                 uint32_t nsamples, struct sf2_bank **out) {
    struct sf2_bank *b = calloc(1, sizeof *b);
    if (!b) { free(samples); usnd_fail("out of memory for the SoundFont"); return -ENOMEM; }
    b->data = samples;
    b->nsamples = nsamples;

    struct span nam;
    if (info.p && find_chunk(info, "INAM", &nam)) {
        uint32_t n = nam.n < sizeof b->name - 1 ? nam.n : (uint32_t)sizeof b->name - 1;
        memcpy(b->name, nam.p, n);
        b->name[n] = 0;
    }

    struct tables t;
    if (!read_tables(pdta, &t)) {
        sf2_free(b);
        usnd_fail("SoundFont preset tables are malformed");
        return -EINVAL;
    }
    struct builder bd = { b, 0, 0, malloc(sizeof(struct zones)) };
    if (!bd.z) {
        sf2_free(b);
        usnd_fail("out of memory for the SoundFont");
        return -ENOMEM;
    }
    for (uint32_t p = 0; p + 1 < t.nphdr; p++) {
        int rc = build_preset(&bd, &t, p);
        if (rc != 0) {
            free(bd.z);
            sf2_free(b);
            usnd_fail(rc == -ENOMEM ? "out of memory for the SoundFont"
                                    : "SoundFont preset names a missing instrument");
            return rc;
        }
    }
    free(bd.z);
    if (b->npresets == 0) {
        sf2_free(b);
        usnd_fail("SoundFont has no presets");
        return -EINVAL;
    }
    *out = b;
    return 0;
}

static int16_t *alloc_samples(uint32_t n) {
    int16_t *s = malloc(((size_t)n + TAIL_SAMPLES) * sizeof *s);
    if (s) memset(s + n, 0, TAIL_SAMPLES * sizeof *s);
    return s;
}

int sf2_parse(const uint8_t *d, size_t n, struct sf2_bank **out) {
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "sfbk", 4)) {
        usnd_fail("not a SoundFont 2 file");
        return -EINVAL;
    }
    uint32_t riff = rd32(d + 4);
    struct span body = { d + 12, (uint32_t)((riff - 4 < n - 12) ? riff - 4 : n - 12) };
    if (riff < 4) body.n = 0;

    struct span info = { 0, 0 }, sdta = { 0, 0 }, pdta = { 0, 0 };
    uint32_t off = 0;
    while (body.n - off >= 12 && off <= body.n) {
        uint32_t sz = rd32(body.p + off + 4);
        if (sz > body.n - off - 8) break;
        if (memcmp(body.p + off, "LIST", 4) == 0 && sz >= 4) {
            struct span l = { body.p + off + 12, sz - 4 };
            if (!memcmp(body.p + off + 8, "INFO", 4)) info = l;
            else if (!memcmp(body.p + off + 8, "sdta", 4)) sdta = l;
            else if (!memcmp(body.p + off + 8, "pdta", 4)) pdta = l;
        }
        off += 8 + sz + (sz & 1);
    }
    struct span smpl;
    if (!sdta.p || !pdta.p || !find_chunk(sdta, "smpl", &smpl)) {
        usnd_fail("SoundFont is missing its sample or preset data");
        return -EINVAL;
    }
    uint32_t ns = smpl.n / 2;
    int16_t *s = alloc_samples(ns);
    if (!s) { usnd_fail("out of memory for the SoundFont"); return -ENOMEM; }
    for (uint32_t i = 0; i < ns; i++) s[i] = (int16_t)rd16(smpl.p + i * 2);
    return build(info, pdta, s, ns, out);
}

// --- from a file ------------------------------------------------------
//
// Walked with reads rather than by loading the file: the sample chunk is
// nearly all of a bank, and reading it straight into its final buffer
// keeps the peak at one copy rather than two.

static int read_at(int fd, uint32_t off, void *dst, uint32_t n) {
    if (sys_lseek(fd, off, SYS_SEEK_SET) != (long long)off) return 0;
    uint8_t *p = dst;
    while (n) {
        uint32_t chunk = n > (1u << 20) ? (1u << 20) : n;
        long got = (long)sys_read(fd, p, chunk);
        if (got <= 0) return 0;
        p += got;
        n -= (uint32_t)got;
    }
    return 1;
}

static uint8_t *read_list(int fd, uint32_t off, uint32_t n) {
    if (n > MAX_TABLE_BYTES) return 0;
    uint8_t *p = malloc(n ? n : 1);
    if (p && !read_at(fd, off, p, n)) { free(p); p = 0; }
    return p;
}

int sf2_load(const char *path, struct sf2_bank **out) {
    int fd = sys_open(path, 0);
    if (fd < 0) { usnd_fail("cannot open the SoundFont"); return -ENOENT; }

    long long size = sys_lseek(fd, 0, SYS_SEEK_END);
    uint8_t hdr[12];
    if (size < 12 || !read_at(fd, 0, hdr, 12) ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "sfbk", 4)) {
        sys_close(fd);
        usnd_fail("not a SoundFont 2 file");
        return -EINVAL;
    }
    uint64_t end = 8 + (uint64_t)rd32(hdr + 4);
    if (end > (uint64_t)size) end = (uint64_t)size;

    uint8_t *info = 0, *pdta = 0;
    uint32_t info_n = 0, pdta_n = 0, smpl_off = 0, smpl_n = 0;
    int have_smpl = 0;
    uint64_t off = 12;
    while (off + 12 <= end) {
        uint8_t ch[12];
        if (!read_at(fd, (uint32_t)off, ch, 12)) break;
        uint32_t sz = rd32(ch + 4);
        if (sz < 4 || off + 8 + sz > end) break;
        if (!memcmp(ch, "LIST", 4)) {
            uint32_t body = (uint32_t)off + 12, n = sz - 4;
            if (!memcmp(ch + 8, "INFO", 4) && !info) {
                info = read_list(fd, body, n); info_n = n;
            } else if (!memcmp(ch + 8, "pdta", 4) && !pdta) {
                pdta = read_list(fd, body, n); pdta_n = n;
            } else if (!memcmp(ch + 8, "sdta", 4)) {
                // Only the sub-chunk headers are read here.
                uint64_t so = body;
                while (so + 8 <= (uint64_t)body + n) {
                    uint8_t sh[8];
                    if (!read_at(fd, (uint32_t)so, sh, 8)) break;
                    uint32_t ssz = rd32(sh + 4);
                    if (so + 8 + ssz > (uint64_t)body + n) break;
                    if (!memcmp(sh, "smpl", 4)) {
                        smpl_off = (uint32_t)so + 8; smpl_n = ssz; have_smpl = 1;
                        break;
                    }
                    so += 8 + ssz + (ssz & 1);
                }
            }
        }
        off += 8 + (uint64_t)sz + (sz & 1);
    }

    int rc;
    if (!pdta || !have_smpl) {
        usnd_fail("SoundFont is missing its sample or preset data");
        rc = -EINVAL;
    } else if (smpl_n > MAX_SMPL_BYTES) {
        usnd_fail("SoundFont is too large to load");
        rc = -ENOTSUP;
    } else {
        uint32_t ns = smpl_n / 2;
        int16_t *s = alloc_samples(ns);
        if (!s) {
            usnd_fail("out of memory for the SoundFont");
            rc = -ENOMEM;
        } else if (!read_at(fd, smpl_off, s, ns * 2)) {
            free(s);
            usnd_fail("SoundFont sample data is truncated");
            rc = -EINVAL;
        } else {
            struct span si = { info, info_n }, sp = { pdta, pdta_n };
            rc = build(si, sp, s, ns, out);
            if (rc == 0) strlcpy((*out)->path, path, sizeof (*out)->path);
        }
    }
    free(info);
    free(pdta);
    sys_close(fd);
    return rc;
}

void sf2_free(struct sf2_bank *b) {
    if (!b) return;
    free(b->data);
    free(b->presets);
    free(b->regions);
    free(b->mods);
    free(b);
}

// --- the per-process cache ------------------------------------------
//
// ONE bank, because a player plays one song at a time and a second
// 30 MB bank held "in case" is memory nobody asked for. A request for a
// different path replaces it once nothing holds it.

static struct sf2_bank *g_cached;

int sf2_get(const char *path, struct sf2_bank **out) {
    if (g_cached && strcmp(g_cached->path, path) == 0) {
        g_cached->refs++;
        *out = g_cached;
        return 0;
    }
    struct sf2_bank *b;
    int rc = sf2_load(path, &b);
    if (rc != 0) return rc;
    if (g_cached && g_cached->refs == 0) sf2_free(g_cached);
    // A cached bank still held elsewhere is left to its holder: sf2_put
    // frees an uncached bank at its last reference.
    g_cached = b;
    b->refs = 1;
    *out = b;
    return 0;
}

void sf2_put(struct sf2_bank *b) {
    if (!b) return;
    if (b->refs > 0) b->refs--;
    if (b->refs == 0 && b != g_cached) sf2_free(b);
}

const struct sf2_preset *sf2_find(const struct sf2_bank *b, int bank, int program) {
    for (uint32_t i = 0; i < b->npresets; i++)
        if (b->presets[i].bank == bank && b->presets[i].program == program)
            return &b->presets[i];
    return 0;
}
