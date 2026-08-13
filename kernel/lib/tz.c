// Timezone selection and application, on top of the raw UTC time
// rtc_read() (timer.c) returns from the CMOS/RTC hardware clock.
//
// Two separate things live under /etc here, and it's worth keeping
// them straight: /etc/timezones is the DATABASE (every city this
// build knows about, one per line -- see tz_load_cities()'s top
// comment for the file format), and /etc/toyos.conf's "timezone=<city>"
// key is the SELECTION (which one of those cities is currently
// active) -- see etc_config.h for the shared reader/writer that key
// goes through. Before the database file existed, the city list was a
// small hardcoded C array; it's now loaded from disk at boot instead,
// so adding/editing/removing a city is a text edit, not a rebuild.
//
// DST is real added complexity for what's otherwise a one-line offset
// add, but was asked for specifically (Finland is UTC+2 in winter, +3
// in summer). Two rules are implemented:
//   - TZ_DST_EU: last Sunday of March 01:00 UTC -> last Sunday of
//     October 01:00 UTC (the actual EU-wide rule, which is also what
//     the UK uses post-Brexit).
//   - TZ_DST_US: second Sunday of March -> first Sunday of November.
// Known simplification: both are checked against the transition *date*
// only (not the exact 01:00/02:00 clock-change instant), so the
// displayed time can be off by up to a few hours right at the
// spring/fall boundary itself. Not worth the extra precision for a
// shell clock.

#include "tz.h"
#include "fs.h"
#include "string.h"
#include "knum.h"
#include "etc_config.h"

enum dst_rule { TZ_DST_NONE, TZ_DST_EU, TZ_DST_US };

#define TZ_NAME_MAX 20 // longest city name today is "losangeles" (10 chars) -- plenty of headroom

struct tz_city {
    char name[TZ_NAME_MAX];    // lowercase, matched by `timezone <name>`
    int base_offset_minutes;   // standard-time UTC offset, before DST
    enum dst_rule dst_rule;
};

// The in-memory city table, loaded from /etc/timezones by
// tz_load_or_seed_db() (called once from tz_init()). No heap allocator
// exists in this kernel, so this is a fixed-capacity array rather than
// something sized to the file's actual content -- TZ_MAX_CITIES is
// generous headroom above the 7 cities this ships with.
#define TZ_MAX_CITIES 32
static struct tz_city TZ_CITIES[TZ_MAX_CITIES];
static int tz_city_count_loaded = 0;

// Compiled-in defaults -- used to seed /etc/timezones the first time
// it's missing (see tz_load_or_seed_db()), and as an in-memory last
// resort if the file exists but somehow yields zero valid rows
// (emptied or badly mangled by hand). Once the file exists and parses
// to at least one real row, IT is the source of truth, not this list
// -- these defaults never silently override a file that's actually
// there and valid.
static const struct tz_city TZ_DEFAULT_CITIES[] = {
    { "utc",         0,    TZ_DST_NONE },
    { "helsinki",  120,    TZ_DST_EU   },
    { "london",      0,    TZ_DST_EU   },
    { "berlin",     60,    TZ_DST_EU   },
    { "newyork",  -300,    TZ_DST_US   },
    { "losangeles", -480,  TZ_DST_US   },
    { "tokyo",     540,    TZ_DST_NONE },
};
#define TZ_DEFAULT_CITY_COUNT ((int)(sizeof(TZ_DEFAULT_CITIES) / sizeof(TZ_DEFAULT_CITIES[0])))

#define TZ_DB_FILE "/etc/timezones" // the database -- see this file's top comment

#define TZ_CONFIG_FILE "/etc/toyos.conf"
#define TZ_CONFIG_KEY "timezone"

static int current_index = 0; // UTC until tz_init() loads/sets otherwise

// ---- date math ----

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int DAYS[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == 2 && is_leap_year(year)) return 29;
    return DAYS[month - 1];
}

// Sakamoto's algorithm. Returns 0=Sunday .. 6=Saturday.
static int day_of_week(int year, int month, int day) {
    static const int T[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (month < 3) year -= 1;
    return (year + year/4 - year/100 + year/400 + T[month - 1] + day) % 7;
}

// Day-of-month of the nth (1-based) Sunday in `month`.
static int nth_sunday(int year, int month, int n) {
    int first_dow = day_of_week(year, month, 1);
    int first_sunday = 1 + ((7 - first_dow) % 7);
    return first_sunday + (n - 1) * 7;
}

// Day-of-month of the last Sunday in `month`.
static int last_sunday(int year, int month) {
    int last_day = days_in_month(year, month);
    int last_dow = day_of_week(year, month, last_day);
    return last_day - last_dow;
}

static int eu_dst_active(int year, int month, int day) {
    if (month > 3 && month < 10) return 1;
    if (month < 3 || month > 10) return 0;
    if (month == 3) return day >= last_sunday(year, 3);
    return day < last_sunday(year, 10); // month == 10
}

static int us_dst_active(int year, int month, int day) {
    if (month > 3 && month < 11) return 1;
    if (month < 3 || month > 11) return 0;
    if (month == 3) return day >= nth_sunday(year, 3, 2);
    return day < nth_sunday(year, 11, 1); // month == 11
}

// Adds (possibly negative) minutes to *t, rolling day/month/year over
// as needed. General enough for any offset, though in practice every
// TZ_CITIES entry is well within a single day's rollover.
static void rtc_add_minutes(struct rtc_time *t, int delta_minutes) {
    int total = (int)t->hour * 60 + (int)t->minute + delta_minutes;
    int day_delta = 0;
    while (total < 0) { total += 24 * 60; day_delta--; }
    while (total >= 24 * 60) { total -= 24 * 60; day_delta++; }
    t->hour = (uint8_t)(total / 60);
    t->minute = (uint8_t)(total % 60);

    int day = (int)t->day + day_delta;
    int month = t->month;
    int year = t->year;
    while (day < 1) {
        month--;
        if (month < 1) { month = 12; year--; }
        day += days_in_month(year, month);
    }
    while (day > days_in_month(year, month)) {
        day -= days_in_month(year, month);
        month++;
        if (month > 12) { month = 1; year++; }
    }
    t->day = (uint8_t)day;
    t->month = (uint8_t)month;
    t->year = (uint16_t)year;
}

// ---- timezone database (/etc/timezones) ----

static int is_tz_space(char c) { return c == ' ' || c == '\t'; }

// Narrows [*start, end) by trimming leading/trailing spaces/tabs and
// returns the trimmed length -- same trim-in-place approach as
// etc_config.c's, kept as its own copy here since this file's line
// format (comma-separated fields, not "key=value") isn't a fit for
// that file's parser.
static uint32_t tz_trim(const char **start, const char *end) {
    const char *s = *start;
    while (s < end && is_tz_space(*s)) s++;
    while (end > s && is_tz_space(end[-1])) end--;
    *start = s;
    return (uint32_t)(end - s);
}

// Parses a (possibly negative) integer from [s, end); no overflow
// checking, since a UTC offset is always small (-720..840 in practice,
// nowhere near int's range). Non-digit characters after a run of
// digits are just ignored -- callers already trimmed whitespace, and
// there's nothing else valid that could follow a number in this file.
static int tz_parse_int(const char *s, const char *end) {
    // knum.h's bounded parser does the digit loop (and rejects junk,
    // which the hand-rolled version silently ignored). A malformed
    // field yields 0 here rather than a load error, matching this
    // file's existing "don't guess, but don't refuse the row either"
    // handling of a bad dst_rule -- see the comment below.
    int64_t v = 0;
    if (end <= s) return 0;
    if (!k_parse_i64_n(s, (size_t)(end - s), &v)) return 0;
    return (int)v;
}

// "eu"/"us" (case-sensitive, matching how city names are matched) ->
// the matching dst_rule; anything else -- "none", empty, a typo --
// means no DST rather than a load error, same "don't guess, but don't
// refuse to load the row either" spirit as etc_config.c's malformed-
// line handling.
static enum dst_rule tz_parse_dst(const char *s, uint32_t len) {
    if (len == 2 && s[0] == 'e' && s[1] == 'u') return TZ_DST_EU;
    if (len == 2 && s[0] == 'u' && s[1] == 's') return TZ_DST_US;
    return TZ_DST_NONE;
}

// Formats a (possibly negative) int as decimal into `out` (no NUL
// written), returning the number of bytes written -- there's no
// itoa()-equivalent in string.h, and this is the only place in the
// kernel that currently needs one, so it stays local here rather than
// becoming shared kernel-wide surface for a single caller.
static uint32_t tz_format_int(char *out, int v) {
    uint32_t n = 0;
    if (v < 0) { out[n++] = '-'; v = -v; }
    char digits[12];
    int dn = 0;
    if (v == 0) digits[dn++] = '0';
    while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
    for (int i = dn - 1; i >= 0; i--) out[n++] = digits[i];
    return n;
}

// Loads TZ_CITIES[] from `data` (TZ_DB_FILE's content, `size` bytes)
// and returns how many rows were actually loaded (0 if none). Format:
// one "name,offset_minutes,dst" per line, e.g. "helsinki,120,eu" or
// "utc,0,none" -- `dst` is "eu", "us", or anything else (including
// empty) for no DST. '#' starts a comment to end of line (whole-line
// or trailing), blank lines are skipped, and whitespace around each
// field is trimmed -- the same conventions as etc_config.c's
// key=value format, just with 3 comma-separated fields per line
// instead of one key=value pair. A malformed row (missing a comma, an
// empty or too-long name) is skipped rather than aborting the whole
// load -- one bad hand-edited line shouldn't cost every other city.
static int tz_load_cities(const char *data, uint32_t size) {
    int count = 0;
    uint32_t pos = 0;
    while (pos < size && count < TZ_MAX_CITIES) {
        const char *ls = data + pos;
        const char *end = data + size;
        const char *le = ls;
        while (le < end && *le != '\n') le++;
        pos = (uint32_t)((le < end ? le + 1 : le) - data);

        const char *content_end = le;
        for (const char *p = ls; p < le; p++) {
            if (*p == '#') { content_end = p; break; }
        }

        const char *p = ls;
        const char *f1 = p;
        while (p < content_end && *p != ',') p++;
        const char *f1_end = p;
        if (p >= content_end) continue; // no comma at all -- not a valid row
        p++;

        const char *f2 = p;
        while (p < content_end && *p != ',') p++;
        const char *f2_end = p;
        if (p >= content_end) continue; // no second comma
        p++;

        const char *f3 = p, *f3_end = content_end;

        const char *ns = f1, *ne = f1_end;
        uint32_t nlen = tz_trim(&ns, ne);
        if (nlen == 0 || nlen >= TZ_NAME_MAX) continue;

        const char *os = f2, *oe = f2_end;
        uint32_t olen = tz_trim(&os, oe);
        if (olen == 0) continue;

        const char *ds = f3, *de = f3_end;
        uint32_t dlen = tz_trim(&ds, de);

        k_memcpy(TZ_CITIES[count].name, ns, nlen);
        TZ_CITIES[count].name[nlen] = '\0';
        TZ_CITIES[count].base_offset_minutes = tz_parse_int(os, oe);
        TZ_CITIES[count].dst_rule = tz_parse_dst(ds, dlen);
        count++;
    }
    return count;
}

// Builds TZ_DB_FILE's initial content from TZ_DEFAULT_CITIES and
// writes it -- called once, the first time /etc/timezones doesn't
// exist yet, so there's always a real, `cat`-able, hand-editable file
// on disk rather than a compiled-in list you'd have to read tz.c's
// source to see.
static void tz_seed_default_db(void) {
    char buf[512];
    uint32_t len = 0;
    static const char *const DST_NAMES[] = { "none", "eu", "us" };
    for (int i = 0; i < TZ_DEFAULT_CITY_COUNT; i++) {
        const struct tz_city *c = &TZ_DEFAULT_CITIES[i];
        uint32_t nlen = (uint32_t)k_strlen(c->name);
        // "name,offset,dst\n" -- 32 bytes of headroom per line is
        // generous (an offset is at most a sign + 3 digits, dst at
        // most "none"'s 4 chars).
        if (len + nlen + 32 >= sizeof(buf)) break;
        k_memcpy(buf + len, c->name, nlen); len += nlen;
        buf[len++] = ',';
        len += tz_format_int(buf + len, c->base_offset_minutes);
        buf[len++] = ',';
        const char *dst = DST_NAMES[c->dst_rule];
        uint32_t dlen = (uint32_t)k_strlen(dst);
        k_memcpy(buf + len, dst, dlen); len += dlen;
        buf[len++] = '\n';
    }
    buf[len] = '\0';
    fs_write(TZ_DB_FILE, buf, 0);
}

// Populates TZ_CITIES[]/tz_city_count_loaded from /etc/timezones,
// seeding the file first if it doesn't exist yet (see
// tz_seed_default_db()). If the file exists but parses to zero valid
// rows -- emptied or badly mangled by hand -- falls back to the
// compiled-in defaults IN MEMORY ONLY, deliberately without rewriting
// the file: a file that exists but fails to parse might just be
// mid-edit, and silently overwriting it here would be a surprising
// way to lose whatever's actually in it.
static void tz_load_or_seed_db(void) {
    if (!fs_exists(TZ_DB_FILE)) tz_seed_default_db();

    uint32_t size = 0;
    const char *data = fs_read(TZ_DB_FILE, &size);
    tz_city_count_loaded = (data && size > 0) ? tz_load_cities(data, size) : 0;

    if (tz_city_count_loaded == 0) {
        int n = TZ_DEFAULT_CITY_COUNT;
        if (n > TZ_MAX_CITIES) n = TZ_MAX_CITIES;
        for (int i = 0; i < n; i++) TZ_CITIES[i] = TZ_DEFAULT_CITIES[i];
        tz_city_count_loaded = n;
    }
}

// ---- public API ----

void tz_init(void) {
    current_index = 0; // default: whichever city loads at index 0 --
                        // "utc" unless /etc/timezones has been hand-
                        // edited to remove or reorder it
    tz_load_or_seed_db();

    char value[TZ_NAME_MAX];
    if (!etc_config_get(TZ_CONFIG_FILE, TZ_CONFIG_KEY, value, sizeof(value))) return;
    int idx = tz_find_by_name(value);
    if (idx >= 0) current_index = idx;
}

int tz_city_count(void) {
    return tz_city_count_loaded;
}

const char *tz_city_name(int index) {
    if (index < 0 || index >= tz_city_count_loaded) return 0;
    return TZ_CITIES[index].name;
}

int tz_current_index(void) {
    return current_index;
}

int tz_set_index(int index) {
    if (index < 0 || index >= tz_city_count_loaded) return 0;
    current_index = index;
    etc_config_set(TZ_CONFIG_FILE, TZ_CONFIG_KEY, TZ_CITIES[index].name);
    return 1;
}

int tz_find_by_name(const char *name) {
    // Case-insensitive: every city in the database is spelled lowercase,
    // but `timezone Helsinki` is what someone actually types. The names
    // are ASCII throughout, which is what makes k_strcasecmp's
    // ASCII-only folding sufficient here.
    for (int i = 0; i < tz_city_count_loaded; i++) {
        if (k_strcasecmp(TZ_CITIES[i].name, name) == 0) return i;
    }
    return -1;
}

void rtc_read_local(struct rtc_time *out) {
    rtc_read(out);

    const struct tz_city *city = &TZ_CITIES[current_index];
    int offset = city->base_offset_minutes;

    int dst = 0;
    if (city->dst_rule == TZ_DST_EU) dst = eu_dst_active(out->year, out->month, out->day);
    else if (city->dst_rule == TZ_DST_US) dst = us_dst_active(out->year, out->month, out->day);
    if (dst) offset += 60;

    rtc_add_minutes(out, offset);
}
