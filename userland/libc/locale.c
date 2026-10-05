// LOCALES: "C", and the regions of /etc/locales -- see <locale.h> for
// the model. A locale here is a few choices, each a word naming a row of
// the tables below; a region is a default for each.
//
// THE VOCABULARY IS IN TWO PLACES ON PURPOSE: these tables say what a
// word MEANS, settings.d/locale.* says which words are legal and how
// System Settings names them. A word in one and not the other is a
// region or an override that is refused here (setlocale() returns NULL
// and the locale stays as it was) -- never one guessed at.
#include <locale.h>
#include <langinfo.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOCALES_DB   "/etc/locales"
#define LOCALE_CONF  "/etc/locale.conf"
#define SETTINGS_DIR "/etc/settings.d"

// --- what each word means -------------------------------------------

struct date_style { const char *word, *d_fmt, *d_fmt_long; };
static const struct date_style DATES[] = {
    { "iso",       "%Y-%m-%d",   "%A %-d %B %Y" },
    { "dmy_dot",   "%-d.%-m.%Y", "%A %-d %B %Y" },
    { "dmy_dot0",  "%d.%m.%Y",   "%A %-d %B %Y" },
    { "mdy_slash", "%-m/%-d/%Y", "%A, %B %-d, %Y" },
    { "dmy_slash0", "%d/%m/%Y",  "%A %-d %B %Y" },
    { "dmy_dash0",  "%d-%m-%Y",  "%A %-d %B %Y" },
};

struct time_style { const char *word, *t_fmt, *t_fmt_hm; };
static const struct time_style TIMES[] = {
    { "24colon", "%H:%M:%S",     "%H:%M" },
    { "24dot",   "%-H.%M.%S",    "%-H.%M" },
    { "12",      "%-I:%M:%S %p", "%-I:%M %p" },
};

struct num_style { const char *word, *decimal, *thousands; };
static const struct num_style NUMBERS[] = {
    { "point",       ".", "" },
    { "space_comma", ",", " " },
    { "comma_point", ".", "," },
    { "point_comma", ",", "." },
    { "apostrophe_point", ".", "'" },   // Switzerland; glibc's is U+2019
};

// The row of `table` whose word is `w`, or -1.
#define FIND(table, w) ({                                              \
    int found_ = -1;                                                   \
    for (size_t i_ = 0; i_ < sizeof (table) / sizeof (table)[0]; i_++) \
        if (strcmp((table)[i_].word, (w)) == 0) { found_ = (int)i_; break; } \
    found_; })

// --- the current locale -----------------------------------------------

#define NAME_MAX_LOC 96   // a region and its @modifiers

struct time_loc {
    char name[NAME_MAX_LOC];
    const char *d_fmt, *d_fmt_long, *t_fmt, *t_fmt_hm;
    char d_t_fmt[32];
    char first_wday[2];         // _NL_TIME_FIRST_WEEKDAY: "\1" Sunday, "\2" Monday
    const char *week_numbers;   // _TOY_WEEK_NUMBERS: "1" or "0"
};

struct num_loc {
    char name[NAME_MAX_LOC];
    const char *decimal, *thousands;
};

// C's own: %x is "%m/%d/%y" and %c "%a %b %e %H:%M:%S %Y", as the
// standard fixes them, and glibc's C week starts on a Sunday.
static const struct time_loc C_TIME = {
    "C", "%m/%d/%y", "%A, %B %e, %Y", "%H:%M:%S", "%H:%M",
    "%a %b %e %H:%M:%S %Y", { 1, 0 }, "0",
};
static const struct num_loc C_NUM = { "C", ".", "" };

static struct time_loc g_time = C_TIME;
static struct num_loc  g_num  = C_NUM;

// --- reading the database and the selection ---------------------------

// The value of `key` in a `key=value` file, trimmed. 0 when the file or
// the key is missing.
static int conf_get(const char *path, const char *key, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    size_t klen = strlen(key);
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        char *v = line + klen + 1;
        size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = '\0';
        strlcpy(out, v, cap);
        found = 1;
    }
    fclose(f);
    return found;
}

// A locale.* setting as the registry would answer it: the stored value,
// else the declaration's Default= -- read from the declaration itself,
// so the default has one home.
static void setting_value(const char *name, char *out, size_t cap) {
    if (conf_get(LOCALE_CONF, name, out, cap)) return;
    char decl[64];
    snprintf(decl, sizeof decl, SETTINGS_DIR "/locale.%s", name);
    if (conf_get(decl, "Default", out, cap)) return;
    strlcpy(out, strcmp(name, "region") == 0 ? "iso" : "region", cap);
}

// A region's words, from its /etc/locales row. 0 when no row names
// it, or the row is short.
struct region { char date[24], time[24], number[24], week[24], weeknum[8]; };

static int region_row(const char *name, struct region *r) {
    FILE *f = fopen(LOCALES_DB, "r");
    if (!f) return 0;
    char line[128];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *field[6];
        int n = 0;
        char *p = line;
        while (n < 6) {
            field[n++] = p;
            p = strchr(p, ',');
            if (!p) break;
            *p++ = '\0';
        }
        if (n < 6) continue;
        char *end = field[5] + strlen(field[5]);
        while (end > field[5] && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
        if (strcmp(field[0], name) != 0) continue;
        strlcpy(r->date, field[1], sizeof r->date);
        strlcpy(r->time, field[2], sizeof r->time);
        strlcpy(r->number, field[3], sizeof r->number);
        strlcpy(r->week, field[4], sizeof r->week);
        strlcpy(r->weeknum, field[5], sizeof r->weeknum);
        found = 1;
    }
    fclose(f);
    return found;
}

// Builds both halves from a region and its overrides ("region" = keep
// the region's word). Returns 0, touching nothing, when any word is not
// one these tables know.
static int build(const char *name, const struct region *r, const char *date,
                 const char *time, const char *number, const char *week,
                 const char *weeknum, struct time_loc *t, struct num_loc *nl) {
    int di = FIND(DATES,   strcmp(date, "region")   ? date   : r->date);
    int ti = FIND(TIMES,   strcmp(time, "region")   ? time   : r->time);
    int ni = FIND(NUMBERS, strcmp(number, "region") ? number : r->number);
    const char *wk = strcmp(week, "region") ? week : r->week;
    int wd = strcmp(wk, "sunday") == 0 ? 1 : strcmp(wk, "monday") == 0 ? 2 : 0;
    const char *wn = strcmp(weeknum, "region") ? weeknum : r->weeknum;
    int wn_on = strcmp(wn, "on") == 0, wn_off = strcmp(wn, "off") == 0;
    if (di < 0 || ti < 0 || ni < 0 || !wd || (!wn_on && !wn_off)) return 0;

    strlcpy(t->name, name, sizeof t->name);
    t->d_fmt = DATES[di].d_fmt;
    t->d_fmt_long = DATES[di].d_fmt_long;
    t->t_fmt = TIMES[ti].t_fmt;
    t->t_fmt_hm = TIMES[ti].t_fmt_hm;
    snprintf(t->d_t_fmt, sizeof t->d_t_fmt, "%s %s", t->d_fmt, t->t_fmt);
    t->first_wday[0] = (char)wd;
    t->first_wday[1] = '\0';
    t->week_numbers = wn_on ? "1" : "0";
    strlcpy(nl->name, name, sizeof nl->name);
    nl->decimal = NUMBERS[ni].decimal;
    nl->thousands = NUMBERS[ni].thousands;
    return 1;
}

// The locale a NAME means: C, or a region -- "fi" -- optionally with
// overrides in POSIX's @modifier position: "fi@time=24colon,week=sunday"
// (keys date, time, number, week, weeknum; the words of the settings).
// That is how a preview asks for formats nobody has applied yet.
static int load_named(const char *name, struct time_loc *t, struct num_loc *nl) {
    if (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0) {
        *t = C_TIME;
        *nl = C_NUM;
        return 1;
    }
    char base[NAME_MAX_LOC];
    if (strlen(name) >= sizeof base) return 0;
    strlcpy(base, name, sizeof base);
    char *mods = strchr(base, '@');
    if (mods) *mods++ = '\0';
    const char *v[5] = { "region", "region", "region", "region", "region" };
    static const char *const KEYS[5] = { "date", "time", "number", "week", "weeknum" };
    for (char *m = mods; m && *m; ) {
        char *next = strchr(m, ',');
        if (next) *next++ = '\0';
        char *eq = strchr(m, '=');
        if (!eq) return 0;
        *eq = '\0';
        int k = 0;
        while (k < 5 && strcmp(KEYS[k], m) != 0) k++;
        if (k == 5) return 0;
        v[k] = eq + 1;
        m = next;
    }
    struct region r;
    if (!region_row(base, &r)) return 0;
    if (!build(base, &r, v[0], v[1], v[2], v[3], v[4], t, nl)) return 0;
    strlcpy(t->name, name, sizeof t->name);
    strlcpy(nl->name, name, sizeof nl->name);
    return 1;
}

// The SYSTEM's locale: the region System Settings chose, with its
// overrides applied.
static int load_system(struct time_loc *t, struct num_loc *nl) {
    char region[NAME_MAX_LOC], date[24], time[24], number[24], week[24], weeknum[8];
    setting_value("region", region, sizeof region);
    setting_value("date_format", date, sizeof date);
    setting_value("time_format", time, sizeof time);
    setting_value("number_format", number, sizeof number);
    setting_value("week_start", week, sizeof week);
    setting_value("week_numbers", weeknum, sizeof weeknum);
    struct region r;
    if (!region_row(region, &r)) return 0;
    return build(region, &r, date, time, number, week, weeknum, t, nl);
}

// POSIX's order for "": LC_ALL, then the category's own variable, then
// LANG. NULL when none is set, which here means the system setting.
static const char *env_for(int category) {
    const char *v = getenv("LC_ALL");
    if (v && v[0]) return v;
    const char *own = category == LC_TIME ? "LC_TIME"
                    : category == LC_NUMERIC ? "LC_NUMERIC" : 0;
    if (own && (v = getenv(own)) && v[0]) return v;
    if ((v = getenv("LANG")) && v[0]) return v;
    return 0;
}

static int resolve(int category, const char *locale, struct time_loc *t, struct num_loc *nl) {
    if (locale[0]) return load_named(locale, t, nl);
    const char *env = env_for(category);
    return env ? load_named(env, t, nl) : load_system(t, nl);
}

char *setlocale(int category, const char *locale) {
    if (category < LC_ALL || category > LC_MESSAGES) return 0;
    int want_time = category == LC_ALL || category == LC_TIME;
    int want_num  = category == LC_ALL || category == LC_NUMERIC;
    if (!locale) return want_num && !want_time ? g_num.name : g_time.name;

    struct time_loc t;
    struct num_loc nl;
    if (!resolve(category, locale, &t, &nl)) return 0;
    if (want_time) g_time = t;
    if (want_num) g_num = nl;
    if (want_time) return g_time.name;
    if (want_num) return g_num.name;
    // The categories with nothing in them answer with the name asked
    // for, having checked it is a locale at all.
    static char other[NAME_MAX_LOC];
    strlcpy(other, t.name, sizeof other);
    return other;
}

// --- what a program reads ----------------------------------------------

static struct lconv g_lconv = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = CHAR_MAX,
    .frac_digits = CHAR_MAX,
    .p_cs_precedes = CHAR_MAX,
    .p_sep_by_space = CHAR_MAX,
    .n_cs_precedes = CHAR_MAX,
    .n_sep_by_space = CHAR_MAX,
    .p_sign_posn = CHAR_MAX,
    .n_sign_posn = CHAR_MAX,
};

struct lconv *localeconv(void) {
    g_lconv.decimal_point = (char *)g_num.decimal;
    g_lconv.thousands_sep = (char *)g_num.thousands;
    g_lconv.grouping = g_num.thousands[0] ? (char *)"\3" : (char *)"";
    return &g_lconv;
}

static const char *const DAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday" };
static const char *const ABDAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MON[] = { "January", "February", "March", "April", "May", "June",
                                   "July", "August", "September", "October",
                                   "November", "December" };
static const char *const ABMON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

char *nl_langinfo(nl_item item) {
    const char *s = "";
    if (item >= DAY_1 && item <= DAY_7) s = DAY[item - DAY_1];
    else if (item >= ABDAY_1 && item <= ABDAY_7) s = ABDAY[item - ABDAY_1];
    else if (item >= MON_1 && item <= MON_12) s = MON[item - MON_1];
    else if (item >= ABMON_1 && item <= ABMON_12) s = ABMON[item - ABMON_1];
    else switch (item) {
    case CODESET:     s = "ISO-8859-1"; break;
    case D_T_FMT:     s = g_time.d_t_fmt; break;
    case D_FMT:       s = g_time.d_fmt; break;
    case T_FMT:       s = g_time.t_fmt; break;
    case T_FMT_AMPM:  s = "%I:%M:%S %p"; break;
    case AM_STR:      s = "AM"; break;
    case PM_STR:      s = "PM"; break;
    case RADIXCHAR:   s = g_num.decimal; break;
    case THOUSEP:     s = g_num.thousands; break;
    case YESEXPR:     s = "^[yY]"; break;
    case NOEXPR:      s = "^[nN]"; break;
    case _NL_TIME_FIRST_WEEKDAY: s = g_time.first_wday; break;
    case _TOY_T_FMT_HM:   s = g_time.t_fmt_hm; break;
    case _TOY_D_FMT_LONG: s = g_time.d_fmt_long; break;
    case _TOY_WEEK_NUMBERS: s = g_time.week_numbers; break;
    default: break;
    }
    return (char *)s;
}
