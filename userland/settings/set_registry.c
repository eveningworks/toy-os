// System Settings: reading the registry, and the sidebar built from it.
#include "settings/settings_internal.h"

// --- what the registry says ------------------------------------------

char     g_label[MAX_SETTINGS][SETTING_ABI_LABEL_MAX];
char     g_desc[MAX_SETTINGS][SETTING_ABI_DESC_MAX];
// The QUALIFIED name ("system.font_size"): a setting's identity is
// (namespace, name), so the bare key is not necessarily usable alone.
char     g_name[MAX_SETTINGS][SETTING_ABI_QUALIFIED_MAX];
char     g_ns[MAX_SETTINGS][SETTING_ABI_NS_MAX];
char     g_file[MAX_SETTINGS][SETTING_ABI_FILE_MAX];
char     g_value[MAX_SETTINGS][SETTING_ABI_VALUE_MAX];
char     g_cat_of[MAX_SETTINGS][SETTING_ABI_CATEGORY_MAX];
char     g_group_of[MAX_SETTINGS][SETTING_ABI_CATEGORY_MAX];
uint32_t g_type[MAX_SETTINGS];
uint32_t g_widget[MAX_SETTINGS];
// INT settings only: the range the REGISTRY enforces, reported so a
// control can bound itself to it. Cached with everything else rather
// than re-read per draw -- SETTING_OP_INFO is a syscall.
int32_t  g_imin[MAX_SETTINGS], g_imax[MAX_SETTINGS], g_istep[MAX_SETTINGS];
char     g_unit[MAX_SETTINGS][SETTING_ABI_UNIT_MAX];
uint32_t g_sflags[MAX_SETTINGS];
int      g_order[MAX_SETTINGS];
// Why this setting cannot be changed on this machine, or empty. From
// the registry (abi/setting_abi.h) -- the app never decides this, and
// never invents the sentence, because a control the UI disabled on a
// rule of its own is one the registry would still let `config set`
// change.
char     g_unavail[MAX_SETTINGS][SETTING_ABI_DESC_MAX];
int      g_setting_count;
uint32_t g_generation;

// The sidebar: categories, and the pages under them.
char g_cat[MAX_CATEGORIES][SETTING_ABI_CATEGORY_MAX];
int  g_cat_count;
char g_group_cat[MAX_GROUPS][SETTING_ABI_CATEGORY_MAX];
char g_group_key[MAX_GROUPS][SETTING_ABI_CATEGORY_MAX];
char g_group_label[MAX_GROUPS][SETTING_ABI_LABEL_MAX];
// Where each category and page sits, from /etc/settings.d (lib/
// usetting.h). Parallel to the tables above and sorted with them.
int g_cat_order[MAX_CATEGORIES];
int g_group_order[MAX_GROUPS];
// **THE SIDEBAR LABEL, WHICH IS THE PAGE'S OWN UNLESS IT COLLIDES.**
// A flat list has no category captions to disambiguate by position, and
// several names genuinely repeat: the Kernel category's pages are
// Display, Memory, Sound, Diagnostics and Storage, four of which are
// also the name of a top-level page. A colliding label is qualified
// with its category ("Kernel: Display"); a unique one is left alone, so
// the common row stays short. DERIVED per rebuild rather than a list
// somebody maintains -- a hardcoded "prefix the Kernel ones" is wrong
// the day two other categories collide.
char g_group_display[MAX_GROUPS][GROUP_DISPLAY_MAX];
int  g_group_count;

struct uui_sidebar_row g_nodes[MAX_CATEGORIES + MAX_GROUPS + 1];
int g_node_count;


// --- reading the registry --------------------------------------------

// The PAGE a setting belongs to. A setting with no group gets one of its
// own, named by its label -- which is what every setting did before
// groups existed, so nothing had to be edited to keep working.
const char *group_key_of(int i) {
    return g_group_of[i][0] ? g_group_of[i] : g_label[i];
}

// THE ICON FOR A CATEGORY, or NULL. A NAME rather than a path, which
// icon_get() resolves under /usr/share/icons -- the same rule a
// `.desktop` entry's `Icon=` follows.
//
// **MATCHED ON THE CATEGORY STRING, which is the registry's and not
// this app's.** A category comes from whatever a `struct setting`
// declared, so this table can only ever be a best effort: a category
// nobody here anticipated gets NO icon and a plain heading, which is
// exactly what every heading looked like before. That is the right
// failure -- the alternative is a generic icon on rows it says nothing
// about, which is worse than none.
const char *category_icon(const char *cat) {
    static const struct { const char *cat, *icon; } MAP[] = {
        { "Time & Locale", "cat-time" },
        { "Appearance",    "cat-appearance" },
        { "Desktop",       "cat-desktop" },
        { "Input",         "cat-input" },
        { "Shortcuts",     "cat-shortcuts" },
        { "Kernel",        "cat-kernel" },
        { "System",        "cat-system" },
        { "Network",       "cat-network" },
        { "Display",       "cat-display" },
        { "Storage",       "cat-storage" },
        { "Sound",         "cat-sound" },
    };
    for (unsigned i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (strcmp(cat, MAP[i].cat) == 0) return MAP[i].icon;
    return 0;
}

// THE SEARCH FILTER, as typed; empty shows everything. Matched without
// case against a page's name and category and against each of its
// settings' label, description and choice names -- the words a person
// would search by, not the registry's keys (GNOME and Windows search the
// same way).
char g_filter[UUI_TEXTBOX_MAX];

static int has_word(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static int setting_matches(int i) {
    if (has_word(g_label[i], g_filter) || has_word(g_desc[i], g_filter)) return 1;
    for (int c = 0; c < MAX_CHOICES; c++) {
        struct setting_msg m;
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_CHOICE;
        m.index = i;
        m.choice = c;
        if (usetting_dispatch(&m) != 0) break;
        if (has_word(m.label, g_filter)) return 1;
    }
    return 0;
}

int group_matches(int g) {
    if (!g_filter[0]) return 1;
    // THE OPEN PAGE STAYS LISTED WHILE IT HOLDS A CHANGE: the filter may
    // not navigate away from it, so filtering its row out would leave the
    // sidebar highlighting some other page beside it.
    if (g == g_page_group && !g_show_sysinfo && page_dirty()) return 1;
    if (has_word(g_group_label[g], g_filter) || has_word(g_group_cat[g], g_filter)) return 1;
    for (int i = 0; i < g_setting_count; i++)
        if (!strcmp(g_cat_of[i], g_group_cat[g]) &&
            !strcmp(group_key_of(i), g_group_key[g]) && setting_matches(i))
            return 1;
    return 0;
}

void rebuild_sidebar(void) {
    // Read BEFORE g_nodes is rewritten: the sidebar holds a row index into
    // this same array, so afterwards it would name whatever row took its place.
    int keep = uui_sidebar_selected_id(&g_tree);
    g_cat_count = 0;
    g_group_count = 0;
    g_node_count = 0;

    // Categories and pages collected in FIRST-SEEN order, then sorted by
    // a DECLARED weight below. The sort is not alphabetical, and that
    // was the original objection to sorting at all: a sidebar that
    // rearranges itself when a setting moves is one nobody builds
    // muscle memory for. A weight in /etc/settings.d is the opposite --
    // it is the one thing here that does not move when the code does.
    for (int i = 0; i < g_setting_count; i++) {
        int c = -1;
        for (int j = 0; j < g_cat_count; j++)
            if (strcmp(g_cat[j], g_cat_of[i]) == 0) { c = j; break; }
        if (c < 0 && g_cat_count < MAX_CATEGORIES) {
            c = g_cat_count++;
            strlcpy(g_cat[c], g_cat_of[i], sizeof g_cat[c]);
            g_cat_order[c] = usetting_category_order(g_cat[c]);
        }
        const char *key = group_key_of(i);
        int g = -1;
        for (int j = 0; j < g_group_count; j++)
            if (strcmp(g_group_cat[j], g_cat_of[i]) == 0 &&
                strcmp(g_group_key[j], key) == 0) { g = j; break; }
        if (g < 0 && g_group_count >= MAX_GROUPS) {
            // SAID, not silently cut. A missing page looks exactly like
            // a setting nobody registered.
            ulogf("settings: OVERFLOW -- more than %d pages; %s/%s is not "
                  "shown\n", MAX_GROUPS, g_cat_of[i], group_key_of(i));
        }
        if (g < 0 && g_group_count < MAX_GROUPS) {
            g = g_group_count++;
            strlcpy(g_group_cat[g], g_cat_of[i], sizeof g_group_cat[g]);
            strlcpy(g_group_key[g], key, sizeof g_group_key[g]);
            g_group_order[g] = usetting_group_order(g_cat_of[i], key);
            // The page's own label, if /etc/settings.d gives it one --
            // otherwise the group key, which is already a human word.
            struct setting_msg m;
            memset(&m, 0, sizeof m);
            m.op = SETTING_OP_GROUP_TEXT;
            snprintf(m.name, sizeof m.name, "%s/%s", g_cat_of[i], key);
            if (usetting_dispatch(&m) == 0 && m.label[0])
                strlcpy(g_group_label[g], m.label, sizeof g_group_label[g]);
            else
                strlcpy(g_group_label[g], key, sizeof g_group_label[g]);
        }
    }

    // **THE ORDER IS DECLARED, AND FIRST-SEEN IS ONLY THE TIE-BREAK.**
    // It used to be first-seen alone, which was the kernel's boot
    // sequence -- so moving a setting out of ring 0 rearranged a list
    // people navigate by muscle memory, and nothing in the tree said
    // where a page was supposed to be. /etc/settings.d carries an
    // `Order=` per category and per page now (lib/usetting.h), which is
    // the weight KDE and GNOME both give a panel.
    //
    // INSERTION SORT, because it is STABLE: everything sharing an order
    // -- including everything with none, which is 0 -- keeps first-seen
    // order between its peers, so a machine whose /etc says nothing
    // looks exactly as it did.
    for (int i = 1; i < g_cat_count; i++) {
        for (int j = i; j > 0 && g_cat_order[j] < g_cat_order[j - 1]; j--) {
            int t = g_cat_order[j]; g_cat_order[j] = g_cat_order[j - 1];
            g_cat_order[j - 1] = t;
            char tmp[SETTING_ABI_CATEGORY_MAX];
            strlcpy(tmp, g_cat[j], sizeof tmp);
            strlcpy(g_cat[j], g_cat[j - 1], sizeof g_cat[j]);
            strlcpy(g_cat[j - 1], tmp, sizeof g_cat[j - 1]);
        }
    }
    // The pages, by the same rule. A global sort is enough because the
    // emit loop below filters by category -- two categories' pages
    // interleaving in this table changes nothing about the sidebar.
    for (int i = 1; i < g_group_count; i++) {
        for (int j = i; j > 0 && g_group_order[j] < g_group_order[j - 1]; j--) {
            int t = g_group_order[j]; g_group_order[j] = g_group_order[j - 1];
            g_group_order[j - 1] = t;
            char a[SETTING_ABI_CATEGORY_MAX], b[SETTING_ABI_CATEGORY_MAX];
            char l[SETTING_ABI_LABEL_MAX];
            strlcpy(a, g_group_cat[j], sizeof a);
            strlcpy(b, g_group_key[j], sizeof b);
            strlcpy(l, g_group_label[j], sizeof l);
            strlcpy(g_group_cat[j], g_group_cat[j - 1], sizeof g_group_cat[j]);
            strlcpy(g_group_key[j], g_group_key[j - 1], sizeof g_group_key[j]);
            strlcpy(g_group_label[j], g_group_label[j - 1], sizeof g_group_label[j]);
            strlcpy(g_group_cat[j - 1], a, sizeof g_group_cat[j - 1]);
            strlcpy(g_group_key[j - 1], b, sizeof g_group_key[j - 1]);
            strlcpy(g_group_label[j - 1], l, sizeof g_group_label[j - 1]);
        }
    }

    // THE ROW LABEL IS THE PAGE'S OWN. Every page sits under a heading
    // naming its category, so position disambiguates -- "Display" under
    // Kernel is not "Display" under Display -- and a category's only page
    // keeps the label its descriptor gives it ("Sound" > "Output").
    for (int g = 0; g < g_group_count; g++)
        strlcpy(g_group_display[g], g_group_label[g], sizeof g_group_display[g]);

    // A HEADING PER CATEGORY, ITS PAGES UNDER IT -- KDE System Settings'
    // and macOS's sidebar. Every destination is an indented ITEM and
    // every heading an inert caption, so which rows can be clicked is
    // never ambiguous (the flat list with rules this replaced named no
    // category at all). A search filter keeps only matching pages and
    // the headings over them.
    int rows_max = (int)(sizeof g_nodes / sizeof g_nodes[0]);
    int shown = 0;
    for (int c = 0; c < g_cat_count; c++) {
        int heading = 0;
        for (int g = 0; g < g_group_count; g++) {
            if (strcmp(g_group_cat[g], g_cat[c]) != 0 || !group_matches(g)) continue;
            if (g_node_count + 2 > rows_max) break;
            if (!heading) {
                heading = 1;
                g_nodes[g_node_count++] = (struct uui_sidebar_row){
                    .label = g_cat[c], .kind = UUI_SIDEBAR_HEADING,
                    .icon = category_icon(g_cat[c]) };
            }
            g_nodes[g_node_count++] = (struct uui_sidebar_row){
                .label = g_group_display[g], .kind = UUI_SIDEBAR_ITEM,
                .id = NODE_GROUP_BASE + g };
            shown++;
        }
    }
    if (!g_filter[0] && g_node_count + 2 <= rows_max) {
        g_nodes[g_node_count++] = (struct uui_sidebar_row){
            .label = "About", .kind = UUI_SIDEBAR_HEADING };
        g_nodes[g_node_count++] = (struct uui_sidebar_row){
            .label = "System Information", .kind = UUI_SIDEBAR_ITEM, .id = NODE_SYSINFO };
    }
    if (g_filter[0] && !shown)
        g_nodes[g_node_count++] = (struct uui_sidebar_row){
            .label = "No settings match", .kind = UUI_SIDEBAR_HEADING };

    uui_sidebar_set_rows(&g_tree, g_nodes, g_node_count);
    if (keep >= 0) uui_sidebar_select_id(&g_tree, keep);
}

int reload_settings(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;
    if (usetting_dispatch(&m) != 0) { g_setting_count = 0; return 0; }

    int n = m.count;
    if (n > MAX_SETTINGS) n = MAX_SETTINGS;
    g_setting_count = 0;

    for (int i = 0; i < n; i++) {
        memset(&m, 0, sizeof m);
        m.op = SETTING_OP_INFO;
        m.index = i;
        if (usetting_dispatch(&m) != 0) continue;

        int k = g_setting_count;
        strlcpy(g_label[k], m.label, sizeof g_label[k]);
        strlcpy(g_desc[k],  m.description, sizeof g_desc[k]);
        strlcpy(g_ns[k],    m.ns,    sizeof g_ns[k]);
        if (m.ns[0]) snprintf(g_name[k], sizeof g_name[k], "%s.%s", m.ns, m.name);
        else         strlcpy(g_name[k], m.name, sizeof g_name[k]);
        strlcpy(g_file[k],  m.file,  sizeof g_file[k]);
        strlcpy(g_value[k], m.value, sizeof g_value[k]);
        strlcpy(g_cat_of[k],   m.category, sizeof g_cat_of[k]);
        strlcpy(g_group_of[k], m.group,    sizeof g_group_of[k]);
        g_type[k]   = m.type;
        g_widget[k] = m.widget;
        g_imin[k]   = m.imin;
        g_imax[k]   = m.imax;
        g_istep[k]  = m.istep;
        strlcpy(g_unit[k], m.unit, sizeof g_unit[k]);
        g_sflags[k] = m.sflags;
        g_order[k]  = m.order;
        strlcpy(g_unavail[k], m.unavailable, sizeof g_unavail[k]);
        g_setting_count++;
    }
    g_generation = m.generation;

    // The NAMESPACE is appended only where it is needed to tell two rows
    // apart. Every label used to carry it unconditionally, which put the
    // same word on every row -- distinguishing nothing while costing
    // width. It cannot simply be dropped: two programs may own settings
    // with the same human label, and identical rows would be unusable.
    //
    // TWO PASSES, because appending while still comparing would suffix
    // only the FIRST of a clashing pair.
    int clash[MAX_SETTINGS];
    for (int i = 0; i < g_setting_count; i++) {
        clash[i] = 0;
        for (int j = 0; j < g_setting_count && !clash[i]; j++)
            if (j != i && strcmp(g_label[i], g_label[j]) == 0) clash[i] = 1;
    }
    for (int i = 0; i < g_setting_count; i++) {
        if (!clash[i] || !g_ns[i][0]) continue;
        char q[SETTING_ABI_LABEL_MAX];
        snprintf(q, sizeof q, "%s  (%s)", g_label[i], g_ns[i]);
        strlcpy(g_label[i], q, sizeof g_label[i]);
    }

    rebuild_sidebar();
    return g_setting_count;
}

uint32_t registry_generation(void) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_COUNT;   // any op carries it; COUNT is the cheapest
    return sys_setting(&m) == 0 ? m.generation : g_generation;
}
