// The HD Audio codec graph -- api/hda_codec.h.
//
// Compiled twice: into the kernel for kernel/drivers/sound/hda.c, and
// into /bin/lscodec for ring 3 (docs/umdf-design.md stage 3). It names
// nothing kernel-only and allocates nothing; the transport is the
// caller's callback and the graph lives in the caller's struct, which
// is the same rule ttf.c follows for the same reason.
//
// EVERYTHING HERE PARSES UNTRUSTED INPUT. Node counts, widget types,
// connection lists and default configurations all come off the card,
// and a codec that lies about them must not be able to walk this off
// the end of an array or into a loop. So: every write is bounded by the
// caller's capacity, the graph walk carries a visited set, and a nid is
// a uint8_t -- 256 of them, which is what sizes the set.
#include "hda_codec.h"

static int cmd(struct hda_codec *c, uint8_t nid, uint32_t verb20, uint32_t *out) {
    if (out) *out = 0;
    if (!c || !c->cmd) return -1;
    return c->cmd(c->ctx, nid, verb20, out);
}

uint32_t hda_codec_param(struct hda_codec *c, uint8_t nid, uint8_t param) {
    uint32_t v = 0;
    cmd(c, nid, V12(VERB_GET_PARAM, param), &v);
    return v;
}

struct hda_widget *hda_codec_widget(struct hda_codec *c, uint8_t nid) {
    for (int i = 0; i < c->nwidgets; i++)
        if (c->widgets[i].nid == nid) return &c->widgets[i];
    return 0;
}

int hda_codec_conn_list(struct hda_codec *c, uint8_t nid, uint8_t *out, int cap) {
    struct hda_widget *w = hda_codec_widget(c, nid);
    if (!w || !(w->caps & WCAP_CONN_LIST) || cap <= 0) return 0;
    uint32_t len = hda_codec_param(c, nid, PARAM_CONN_LIST_LEN);
    int n = (int)(len & 0x7F);
    int longf = (len & 0x80) != 0;
    int per = longf ? 2 : 4;
    int count = 0;
    for (int i = 0; i < n && count < cap; i += per) {
        uint32_t r = 0;
        if (cmd(c, nid, V12(VERB_GET_CONN_LIST, i), &r) != 0) break;
        for (int j = 0; j < per && i + j < n && count < cap; j++) {
            uint32_t entry = longf ? (r >> (16 * j)) & 0xFFFF : (r >> (8 * j)) & 0xFF;
            uint32_t range = longf ? 0x8000 : 0x80;
            uint32_t mask  = longf ? 0x7FFF : 0x7F;
            uint8_t  nid2  = (uint8_t)(entry & mask);
            if ((entry & range) && count > 0) {
                for (uint8_t k = (uint8_t)(out[count - 1] + 1); k <= nid2 && count < cap; k++)
                    out[count++] = k;
            } else {
                out[count++] = nid2;
            }
        }
    }
    return count;
}

int hda_codec_enumerate(struct hda_codec *c) {
    uint32_t nodes = hda_codec_param(c, 0, PARAM_NODE_COUNT);
    uint8_t fg_start = (uint8_t)(nodes >> 16), fg_count = (uint8_t)nodes;
    c->afg = 0;
    c->nwidgets = 0;
    for (uint8_t i = 0; i < fg_count; i++) {
        uint8_t nid = (uint8_t)(fg_start + i);
        if ((hda_codec_param(c, nid, PARAM_FUNC_TYPE) & 0xFF) == FUNC_AUDIO) {
            c->afg = nid;
            break;
        }
    }
    if (!c->afg) return -1;
    // The one write on this side: a function group left in D3 answers
    // its widgets' parameters with whatever it has powered, so the
    // graph cannot be read without it.
    cmd(c, c->afg, V12(VERB_SET_POWER, 0), 0);

    nodes = hda_codec_param(c, c->afg, PARAM_NODE_COUNT);
    uint8_t start = (uint8_t)(nodes >> 16), count = (uint8_t)nodes;
    for (uint8_t i = 0; i < count && c->nwidgets < HDA_MAX_WIDGET; i++) {
        struct hda_widget *w = &c->widgets[c->nwidgets];
        w->nid = (uint8_t)(start + i);
        w->caps = hda_codec_param(c, w->nid, PARAM_WIDGET_CAPS);
        w->type = (uint8_t)WCAP_TYPE(w->caps);
        w->pincap = w->defcfg = 0;
        if (w->type == WT_PIN) {
            w->pincap = hda_codec_param(c, w->nid, PARAM_PIN_CAPS);
            cmd(c, w->nid, V12(VERB_GET_CONFIG_DEF, 0), &w->defcfg);
        }
        c->nwidgets++;
    }
    return c->nwidgets ? 0 : -1;
}

uint32_t hda_codec_amp_caps(struct hda_codec *c, const struct hda_widget *w, int out) {
    uint8_t p = out ? PARAM_AMP_OUT_CAPS : PARAM_AMP_IN_CAPS;
    return hda_codec_param(c, (w->caps & WCAP_AMP_OVRD) ? w->nid : c->afg, p);
}

int hda_codec_find_path(struct hda_codec *c, uint8_t nid, uint8_t *path,
                        int depth, uint32_t *visited) {
    if (depth >= HDA_PATH_MAX) return 0;
    struct hda_widget *w = hda_codec_widget(c, nid);
    if (!w || (w->caps & WCAP_DIGITAL)) return 0;
    if (visited[nid >> 5] & (1u << (nid & 31))) return 0;
    visited[nid >> 5] |= 1u << (nid & 31);
    path[depth] = nid;
    if (w->type == WT_AUD_OUT) return depth + 1;
    if (w->type != WT_PIN && w->type != WT_MIXER && w->type != WT_SELECTOR) return 0;
    uint8_t conns[HDA_CONN_MAX];
    int n = hda_codec_conn_list(c, nid, conns, HDA_CONN_MAX);
    for (int i = 0; i < n; i++) {
        int len = hda_codec_find_path(c, conns[i], path, depth + 1, visited);
        if (len) return len;
    }
    return 0;
}

int hda_codec_out_rank(const struct hda_widget *w) {
    if (w->type != WT_PIN || !(w->pincap & PINCAP_OUTPUT) || (w->caps & WCAP_DIGITAL)) return 0;
    if (DEFCFG_CONN(w->defcfg) == 1) return 0; // nothing wired
    switch (DEFCFG_DEV(w->defcfg)) {
    case DEV_SPEAKER:  return DEFCFG_CONN(w->defcfg) == 0 ? 3 : 4;
    case DEV_LINE_OUT: return 2;
    case DEV_HP_OUT:   return 1;
    default:           return 0;
    }
}

int hda_codec_pick_outputs(struct hda_codec *c) {
    c->have_spk = c->have_hp = 0;
    int best = 0;
    struct hda_widget *spk = 0, *hp = 0;
    for (int i = 0; i < c->nwidgets; i++) {
        struct hda_widget *w = &c->widgets[i];
        int r = hda_codec_out_rank(w);
        if (r > best) { best = r; spk = w; }
        if (r == 1 && !hp) hp = w;
    }
    if (!spk) return -1;
    if (hp == spk) hp = 0; // headphones are the only output: always on

    uint32_t visited[8] = {0};
    c->spk.len = hda_codec_find_path(c, spk->nid, c->spk.path, 0, visited);
    if (!c->spk.len) return -1;
    c->spk.pin = spk->nid;
    c->spk.dac = c->spk.path[c->spk.len - 1];
    c->have_spk = 1;

    if (hp) {
        uint32_t v2[8] = {0};
        c->hp.len = hda_codec_find_path(c, hp->nid, c->hp.path, 0, v2);
        if (c->hp.len) {
            c->hp.pin = hp->nid;
            c->hp.dac = c->hp.path[c->hp.len - 1];
            c->have_hp = 1;
        }
    }
    return 0;
}

void hda_codec_pick_volume(struct hda_codec *c, struct hda_out *o) {
    // From the DAC end, which is the order route_output() writes in:
    // the knob is the first amplifier the signal meets coming back.
    o->vol_nid = 0;
    o->vol_steps = o->vol_offset = 0;
    o->vol_step_qdb = 1;
    o->vol_mute = 0;
    for (int i = o->len - 1; i >= 0; i--) {
        struct hda_widget *w = hda_codec_widget(c, o->path[i]);
        if (!w || !(w->caps & WCAP_OUT_AMP)) continue;
        // Amp caps: 6:0 the 0 dB step (offset), 14:8 the step count,
        // 22:16 the step size, 31 mute-capable.
        uint32_t oc = hda_codec_amp_caps(c, w, 1);
        uint8_t steps = (uint8_t)((oc >> 8) & 0x7F);
        if (!steps && !(oc & 0x80000000u)) continue;
        o->vol_nid = w->nid;
        o->vol_steps = steps;
        o->vol_offset = (uint8_t)(oc & 0x7F);
        o->vol_step_qdb = (uint8_t)(((oc >> 16) & 0x7F) + 1);
        o->vol_mute = (oc & 0x80000000u) != 0;
        return;
    }
}
