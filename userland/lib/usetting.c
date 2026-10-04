// See usetting.h.
#include "lib/usetting.h"
#include "lib/usetting_schema.h"
#include "rt/sys.h"
#include "knum.h"
#include "lib/uconf.h"
#include "lib/usetting_text.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   // strcasecmp

// --- the merged registry ---------------------------------------------

// The kernel's own count, and the generation that rides every reply.
// Asked before any index is split, which is what makes one syscall
// answer both questions.
static int kernel_count(uint32_t *generation) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (sys_setting(&m) != 0) return -1;
    if (generation) *generation = m.generation;
    return (int)m.count;
}

// Announce a change this process made by writing /etc itself. The
// counter lives in the kernel because every consumer already polls it
// (see SETTING_OP_TOUCH in abi/setting_abi.h).
static void touch(uint32_t *generation) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_TOUCH;
    if (sys_setting(&m) == 0 && generation) *generation = m.generation;
}

static void fill_from_schema(const struct uschema *s, struct setting_msg *m) {
    strlcpy(m->name, s->name, sizeof m->name);
    strlcpy(m->ns, s->ns, sizeof m->ns);
    strlcpy(m->label, s->label, sizeof m->label);
    strlcpy(m->file, s->file, sizeof m->file);
    strlcpy(m->category, s->category, sizeof m->category);
    strlcpy(m->group, s->group, sizeof m->group);
    m->type = s->type;
    m->count = uschema_choice_count(s);
    m->imin = m->imax = m->istep = 0;
    m->unit[0] = '\0';
    if (s->type == SETTING_ABI_TYPE_INT) {
        m->imin = s->min;
        m->imax = s->max;
        m->istep = s->step;
        strlcpy(m->unit, s->unit, sizeof m->unit);
    }
    // ONE READ, NOT TWO: `value` is what the file says or the default,
    // so asking uschema_get() after uschema_stored() would open and
    // parse the same document a second time -- 50 reads across a `config
    // list` rather than 25.
    m->stored[0] = '\0';
    uschema_stored(s, m->stored, sizeof m->stored);
    strlcpy(m->value, m->stored[0] ? m->stored : s->def, sizeof m->value);
    // A SCHEMA SETTING IS UNAVAILABLE ONLY THROUGH ANOTHER DECLARED ONE.
    // `unavailable` otherwise describes the MACHINE -- a control policy
    // or hardware has taken away -- a sentence only the owner of the knob
    // can write, and a way for /etc to invent one would let it disable a
    // control the kernel is perfectly willing to change. Requires= can
    // name only another DECLARED setting (uschema_unmet()), so it links
    // two knobs /etc already owns and reaches nothing of the kernel's.
    if (uschema_unmet(s, m->unavailable, sizeof m->unavailable) && s->otherwise[0])
        strlcpy(m->value, s->otherwise, sizeof m->value);
    uschema_text(s, m);
}

// One choice in REGISTRY order: the kernel's or the schema's, with the
// display name filled from /etc/settings.d.
static int raw_choice(struct setting_msg *m, int kcount, uint32_t gen) {
    if (m->index < kcount) {
        int rc = sys_setting(m);
        if (rc != 0) return rc;
        uschema_choice_label_for(m->ns, m->name, m->value, m->label, sizeof m->label);
        return 0;
    }
    struct uschema s;
    if (!uschema_at(m->index - kcount, &s)) return -1;
    m->generation = gen;
    if (!uschema_choice(&s, m->choice, m->value, sizeof m->value)) return -1;
    uschema_choice_label(&s, m->value, m->label, sizeof m->label);
    return 0;
}

// --- `Sort=label`: the choice order every front end sees ---------------
//
// HERE, NOT IN A CLIENT, so System Settings, `config` and anything else
// enumerating SETTING_OP_CHOICE agree. The order is worked out once per
// (setting, generation) and kept: one read of the text file and one pass
// over the choices, not one per choice asked for. A choice list can grow
// without the generation moving (a file dropped into /etc/kbs); the
// cache is per process, so a fresh process sees it.
#define SORT_MAX   128   // = System Settings' MAX_CHOICES
#define SORT_SLOTS 16
struct sort_entry {
    int used, index, sorted, count;
    uint32_t gen;
    uint8_t order[SORT_MAX];
};
static struct sort_entry g_sort[SORT_SLOTS];
static int g_sort_next;
static char g_sort_label[SORT_MAX][SETTING_ABI_LABEL_MAX];

static const struct sort_entry *sort_entry_for(int index, int kcount, uint32_t gen) {
    for (int i = 0; i < SORT_SLOTS; i++)
        if (g_sort[i].used && g_sort[i].index == index && g_sort[i].gen == gen)
            return &g_sort[i];

    struct sort_entry *e = &g_sort[g_sort_next];
    g_sort_next = (g_sort_next + 1) % SORT_SLOTS;
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->index = index;
    e->gen = gen;

    // Whose text file: the setting's own (ns, name).
    struct setting_msg t;
    memset(&t, 0, sizeof t);
    if (index < kcount) {
        t.op = SETTING_OP_INFO;
        t.index = (uint32_t)index;
        if (sys_setting(&t) != 0) return e;
    } else {
        struct uschema s;
        if (!uschema_at(index - kcount, &s)) return e;
        strlcpy(t.ns, s.ns, sizeof t.ns);
        strlcpy(t.name, s.name, sizeof t.name);
    }
    char ns[SETTING_ABI_NS_MAX], name[SETTING_ABI_NAME_MAX];
    strlcpy(ns, t.ns, sizeof ns);
    strlcpy(name, t.name, sizeof name);
    uschema_text_for(ns, name, &t);
    if (!(t.sflags & SETTING_ABI_SF_SORTED)) return e;

    for (int c = 0; c < SORT_MAX; c++) {
        memset(&t, 0, sizeof t);
        t.op = SETTING_OP_CHOICE;
        t.index = (uint32_t)index;
        t.choice = (uint32_t)c;
        if (raw_choice(&t, kcount, gen) != 0) break;
        strlcpy(g_sort_label[c], t.label[0] ? t.label : t.value, sizeof g_sort_label[c]);
        e->order[c] = (uint8_t)c;
        e->count = c + 1;
    }
    // Insertion sort by label; a few dozen rows at most.
    for (int i = 1; i < e->count; i++)
        for (int j = i; j > 0 && strcasecmp(g_sort_label[e->order[j - 1]],
                                            g_sort_label[e->order[j]]) > 0; j--) {
            uint8_t x = e->order[j];
            e->order[j] = e->order[j - 1];
            e->order[j - 1] = x;
        }
    e->sorted = 1;
    return e;
}

static int sorted_choice(struct setting_msg *m, int kcount, uint32_t gen) {
    const struct sort_entry *e = sort_entry_for((int)m->index, kcount, gen);
    if (e->sorted) {
        if (m->choice >= (uint32_t)e->count) return -1;
        m->choice = e->order[m->choice];
    }
    return raw_choice(m, kcount, gen);
}

// Which half owns `name`, and the schema when it is this one.
#define OWNER_NONE   0
#define OWNER_KERNEL 1
#define OWNER_SCHEMA 2

// How many schema settings a BARE name matches. Answered from the
// cached listing, so it costs no I/O -- which is what makes it
// affordable on every GET and SET.
static int schema_bare_matches(const char *name, char *out_qualified,
                               uint32_t cap) {
    int n = 0;
    for (int i = 0; i < uschema_count(); i++) {
        struct uschema s;
        if (!uschema_at(i, &s)) continue;
        if (strcmp(s.name, name) != 0) continue;
        if (out_qualified && cap) snprintf(out_qualified, cap, "%s.%s", s.ns, s.name);
        n++;
    }
    return n;
}

// A BARE NAME MATCHING BOTH HALVES IS REFUSED, not resolved -- the rule
// api/setting.h sets for the kernel registry, kept across the split.
// Resolving by which half was asked first would make the answer depend
// on where a setting happens to live, which is exactly what a
// namespace exists to stop mattering.
static int resolve(const char *name, struct uschema *out, uint32_t *generation) {
    if (!name || !name[0]) return OWNER_NONE;

    struct setting_msg probe;
    memset(&probe, 0, sizeof probe);
    probe.op = SETTING_OP_GET;
    strlcpy(probe.name, name, sizeof probe.name);
    int in_kernel = sys_setting(&probe) == 0;
    if (in_kernel && generation) *generation = probe.generation;

    if (strchr(name, '.')) {
        if (in_kernel) return OWNER_KERNEL;
        return uschema_find(name, out) ? OWNER_SCHEMA : OWNER_NONE;
    }

    char qualified[SETTING_ABI_QUALIFIED_MAX];
    int matches = schema_bare_matches(name, qualified, sizeof qualified);
    if (in_kernel) return matches ? OWNER_NONE : OWNER_KERNEL; // ambiguous
    if (matches != 1) return OWNER_NONE;
    return uschema_find(qualified, out) ? OWNER_SCHEMA : OWNER_NONE;
}

int usetting_dispatch(struct setting_msg *m) {
    if (!m) return -1;

    switch (m->op) {
    case SETTING_OP_COUNT: {
        uint32_t gen = 0;
        int kcount = kernel_count(&gen);
        if (kcount < 0) return -1;
        m->count = kcount + uschema_count();
        m->generation = gen;
        return 0;
    }

    case SETTING_OP_CHOICE: {
        uint32_t gen = 0;
        int kcount = kernel_count(&gen);
        if (kcount < 0) return -1;
        return sorted_choice(m, kcount, gen);
    }

    case SETTING_OP_INFO: {
        uint32_t gen = 0;
        int kcount = kernel_count(&gen);
        if (kcount < 0) return -1;
        if (m->index < kcount) {
            // A KERNEL SETTING'S PRESENTATION IS READ HERE TOO. Ring 0
            // answers what the setting IS -- its type, bounds, value --
            // and stopped reading /etc/settings.d at all, so the
            // description, the widget and a choice's display name are
            // filled from the same reader the declared settings use.
            // One parser for that directory instead of two.
            int rc = sys_setting(m);
            if (rc != 0) return rc;
            uschema_text_for(m->ns, m->name, m);
            return 0;
        }

        struct uschema s;
        if (!uschema_at(m->index - kcount, &s)) return -1;
        m->generation = gen;
        fill_from_schema(&s, m);
        return 0;
    }

    case SETTING_OP_GET: {
        struct uschema s;
        uint32_t gen = 0;
        switch (resolve(m->name, &s, &gen)) {
        case OWNER_KERNEL: return sys_setting(m);
        case OWNER_SCHEMA:
            uschema_effective(&s, m->value, sizeof m->value);
            kernel_count(&m->generation);
            return 0;
        default: return -1;
        }
    }

    case SETTING_OP_SET: {
        struct uschema s;
        uint32_t gen = 0;
        switch (resolve(m->name, &s, &gen)) {
        case OWNER_KERNEL: return sys_setting(m);
        case OWNER_SCHEMA: {
            // ANNOUNCED ONLY IF SOMETHING CHANGED. A refused value
            // changed nothing, and so did setting a value to what it
            // already was -- and the kernel's setting_set() skips the
            // bump for exactly that second case, because everything
            // watching the generation does real work when it moves.
            int changed = 0;
            // UNAVAILABLE REFUSES A WRITE, as the ABI promises for a
            // kernel setting's. (An UNSET is allowed: the default is
            // always a safe place to go back to.)
            if (uschema_unmet(&s, 0, 0)) {
                m->result = SETTING_INVALID;
                kernel_count(&m->generation);
                return 0;
            }
            m->result = (uint32_t)uschema_write(&s, m->value, &changed);
            if (changed) touch(&m->generation);
            else kernel_count(&m->generation);
            return 0;
        }
        default: return -1;
        }
    }

    case SETTING_OP_UNSET: {
        struct uschema s;
        uint32_t gen = 0;
        switch (resolve(m->name, &s, &gen)) {
        case OWNER_KERNEL: return sys_setting(m);
        case OWNER_SCHEMA:
            m->result = (uint32_t)uschema_unset(&s);
            if (m->result != SETTING_INVALID) touch(&m->generation);
            else kernel_count(&m->generation);
            return 0;
        default: return -1;
        }
    }

    case SETTING_OP_RELOAD: {
        // A DECLARATION FILE MAY HAVE BEEN ADDED OR EDITED, and the
        // listing is cached, so the reload that re-reads /etc drops it.
        uschema_invalidate();
        int rc = sys_setting(m);
        if (rc != 0) return rc;
        // AND THE COUNT OF REJECTED VALUES COVERS BOTH HALVES. A
        // schema setting has no live copy to re-apply -- its owner
        // reads the file itself -- but `config reload`'s whole job is
        // to say that a hand edit was refused, and a reload that
        // reported only the kernel's settings would answer "Reloaded."
        // for a /etc/desktop.conf somebody had just typed a bad value
        // into. Validation is the only half there is here, so it is
        // the half that runs.
        for (int i = 0; i < uschema_count(); i++) {
            struct uschema s;
            char stored[SETTING_ABI_VALUE_MAX];
            if (!uschema_at(i, &s)) continue;
            if (!uschema_stored(&s, stored, sizeof stored) || !stored[0]) continue;
            if (!uschema_validate(&s, stored)) m->count++;
        }
        return 0;
    }

    // A PAGE'S OWN TEXT IS RING 3'S NOW, with the rest of the
    // presentation. The kernel refuses this op.
    case SETTING_OP_GROUP_TEXT: {
        char category[SETTING_ABI_CATEGORY_MAX];
        const char *slash = strchr(m->name, '/');
        if (!slash) return -1;
        uint32_t n = (uint32_t)(slash - m->name);
        if (n >= sizeof category) return -1;
        memcpy(category, m->name, n);
        category[n] = '\0';
        kernel_count(&m->generation);
        return uschema_group_text(category, slash + 1, m) ? 0 : -1;
    }

    // The config-FILE registry stays the kernel's: it indexes /etc
    // DOCUMENTS, which is a fact about the machine rather than
    // presentation, and a tunable's namespace is registered there.
    default:
        return sys_setting(m);
    }
}

// --- sidebar order ---------------------------------------------------

// Shared by both lookups below: an `Order=` from one file in
// /etc/settings.d, or 0 when the file or the key is absent -- which is
// the same answer, because "no opinion" and "first" are both fine
// defaults for a list that then falls back to first-seen order.
static int order_of(const char *path) {
    char v[16];
    if (!uconf_get(path, SETTING_TEXT_KEY_ORDER, v, sizeof v) || !v[0]) return 0;
    char *end = 0;
    long n = strtol(v, &end, 10);
    if (end == v || (end && *end)) return 0;
    return (int)n;
}

int usetting_category_order(const char *category) {
    char path[192];
    if (!category || !category[0]) return 0;
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/category.%s", category) <= 0)
        return 0;
    return order_of(path);
}

int usetting_group_order(const char *category, const char *group) {
    char path[192];
    if (!category || !category[0] || !group || !group[0]) return 0;
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/group.%s.%s",
                 category, group) <= 0)
        return 0;
    return order_of(path);
}

int usetting_page_debug(const char *category, const char *group) {
    char path[192], v[8];
    if (!category || !category[0]) return 0;
    if (snprintf(path, sizeof path, SETTING_TEXT_DIR "/category.%s", category) > 0 &&
        uconf_get(path, SETTING_TEXT_KEY_DEBUG, v, sizeof v) && !strcmp(v, "1"))
        return 1;
    if (!group || !group[0]) return 0;
    return snprintf(path, sizeof path, SETTING_TEXT_DIR "/group.%s.%s", category, group) > 0 &&
           uconf_get(path, SETTING_TEXT_KEY_DEBUG, v, sizeof v) && !strcmp(v, "1");
}

// --- the by-name helpers ---------------------------------------------

int usetting_get(const char *name, char *out, size_t cap) {
    struct setting_msg m;
    if (cap) out[0] = '\0';
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_GET;
    strlcpy(m.name, name, sizeof m.name);
    if (usetting_dispatch(&m) != 0) return 0;
    strlcpy(out, m.value, cap);
    return 1;
}

int usetting_get_int(const char *name, int *out) {
    char v[SETTING_ABI_VALUE_MAX];
    uint32_t n;
    if (!usetting_get(name, v, sizeof v)) return 0;
    if (!k_parse_u32(v, &n) || n > 0x7fffffff) return 0;
    *out = (int)n;
    return 1;
}

int usetting_set(const char *name, const char *value) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_SET;
    strlcpy(m.name, name, sizeof m.name);
    strlcpy(m.value, value, sizeof m.value);
    if (usetting_dispatch(&m) != 0) return -1;
    return (int)m.result;
}

int usetting_find(const char *name, struct setting_msg *out) {
    struct setting_msg m;
    memset(out, 0, sizeof *out);
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (usetting_dispatch(&m) != 0) return -1;
    int count = (int)m.count;
    for (int i = 0; i < count; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;
        // Qualified, because a bare name is only unique until something
        // else registers one.
        char qualified[SETTING_ABI_QUALIFIED_MAX];
        snprintf(qualified, sizeof qualified, "%s.%s", m.ns, m.name);
        if (strcmp(qualified, name) != 0) continue;
        *out = m;
        return i;
    }
    return -1;
}

int usetting_set_int(const char *name, int value) {
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    return usetting_set(name, v);
}
