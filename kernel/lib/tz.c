// Timezone selection and application, on top of the raw UTC time
// rtc_read() (timer.c) returns from the CMOS/RTC hardware clock.
//
// Two separate things live under /etc here, and it's worth keeping
// them straight: /etc/timezones is the DATABASE (every city this
// build knows about, one "name,offset,dst,Display Name" row per line
// -- see tz_load_cities()'s top comment for the format), and
// /etc/toyos.conf's "timezone=<city>"
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
#include "caltime.h"
#include "fs.h"
#include "string.h"
#include "klog.h"
#include "knum.h"
#include "etc_config.h"
#include "setting.h"
#include "initcall.h"

enum dst_rule { TZ_DST_NONE, TZ_DST_EU, TZ_DST_US };

#define TZ_NAME_MAX 20 // longest city name today is "losangeles" (10 chars) -- plenty of headroom
#define TZ_LABEL_MAX 32 // longest display name today is "South Georgia" (13)

struct tz_city {
    char name[TZ_NAME_MAX];    // lowercase, matched by `timezone <name>`
    int base_offset_minutes;   // standard-time UTC offset, before DST
    enum dst_rule dst_rule;
    // WHAT A PERSON READS: "Los Angeles" for `losangeles`. The name is
    // a TOKEN -- lowercase, no spaces, so it can be typed as an
    // argument and stored in a config file -- and a token is not a
    // display name. Empty here means "no display name", and
    // tz_city_label() falls back to the token, so every caller may draw
    // what it returns unconditionally.
    char label[TZ_LABEL_MAX];
};

// The in-memory city table, loaded from /etc/timezones by
// tz_load_or_seed_db() (called once from tz_init()). No heap allocator
// exists in this kernel, so this is a fixed-capacity array rather than
// something sized to the file's actual content. Headroom above the
// shipped list, which is what TZ_DEFAULT_CITY_COUNT counts -- a
// hand-edited /etc/timezones may add its own rows, and anything past
// this is silently not loaded, which is why the headroom is generous.
#define TZ_MAX_CITIES 128
static struct tz_city TZ_CITIES[TZ_MAX_CITIES];
static int tz_city_count_loaded = 0;

// Compiled-in defaults -- used to seed /etc/timezones the first time
// it's missing (see tz_load_or_seed_db()), and as an in-memory last
// resort if the file exists but somehow yields zero valid rows
// (emptied or badly mangled by hand). Once the file exists and parses
// to at least one real row, IT is the source of truth, not this list
// -- these defaults never silently override a file that's actually
// there and valid.
// THE DST RULE IS ONLY EVER ONE THIS KERNEL CAN ACTUALLY APPLY.
//
// There are three (EU, US, none), so a city whose real rule is neither
// -- Sydney, Sao Paulo, Tehran, Santiago, Newfoundland -- is listed as
// TZ_DST_NONE and is therefore an hour out during ITS summer. That is a
// deliberate, stated limitation rather than a claim: marking Sydney
// "EU" would put its clocks forward in April, which is not merely
// approximate but backwards, since the southern hemisphere's summer is
// the northern one's winter.
//
// The same reasoning rules out importing the IANA database wholesale:
// completeness that is confidently wrong is worse than a shorter list
// that is right about what it says. Adding a rule here is what makes a
// city eligible for it -- see enum dst_rule.
//
// ORDERED ALPHABETICALLY, because with ninety-two entries and no search
// box the thing a user is doing is LOOKING UP a city they already know
// -- and a name is the only key they have. It was west-to-east, which
// reads better if you are browsing and is useless if you are hunting
// for "tokyo".
//
// The order here IS the order shown: the setting's choice enumerator
// walks this table and no UI sorts. Keeping the sort at the source
// rather than in a client means every client agrees, and a hand-edited
// /etc/timezones is shown in whatever order it was written -- which is
// the honest behaviour for a file somebody chose to edit.
static const struct tz_city TZ_DEFAULT_CITIES[] = {
    { "accra",             0, TZ_DST_NONE,  "Accra"          },
    { "adelaide",        570, TZ_DST_NONE,  "Adelaide"       },
    { "almaty",          360, TZ_DST_NONE,  "Almaty"         },
    { "amsterdam",        60, TZ_DST_EU,    "Amsterdam"      },
    { "anchorage",      -540, TZ_DST_US,    "Anchorage"      },
    { "apia",            780, TZ_DST_NONE,  "Apia"           },
    { "athens",          120, TZ_DST_EU,    "Athens"         },
    { "auckland",        720, TZ_DST_NONE,  "Auckland"       },
    { "azores",          -60, TZ_DST_EU,    "Azores"         },
    { "bakerisland",    -720, TZ_DST_NONE,  "Baker Island"   },
    { "baku",            240, TZ_DST_NONE,  "Baku"           },
    { "bangkok",         420, TZ_DST_NONE,  "Bangkok"        },
    { "beijing",         480, TZ_DST_NONE,  "Beijing"        },
    { "berlin",           60, TZ_DST_EU,    "Berlin"         },
    { "bogota",         -300, TZ_DST_NONE,  "Bogota"         },
    { "brisbane",        600, TZ_DST_NONE,  "Brisbane"       },
    { "brussels",         60, TZ_DST_EU,    "Brussels"       },
    { "bucharest",       120, TZ_DST_EU,    "Bucharest"      },
    { "budapest",         60, TZ_DST_EU,    "Budapest"       },
    { "buenosaires",    -180, TZ_DST_NONE,  "Buenos Aires"   },
    { "cairo",           120, TZ_DST_NONE,  "Cairo"          },
    { "capeverde",       -60, TZ_DST_NONE,  "Cape Verde"     },
    { "caracas",        -240, TZ_DST_NONE,  "Caracas"        },
    { "chatham",         765, TZ_DST_NONE,  "Chatham"        },
    { "chicago",        -360, TZ_DST_US,    "Chicago"        },
    { "colombo",         330, TZ_DST_NONE,  "Colombo"        },
    { "copenhagen",       60, TZ_DST_EU,    "Copenhagen"     },
    { "delhi",           330, TZ_DST_NONE,  "Delhi"          },
    { "denver",         -420, TZ_DST_US,    "Denver"         },
    { "dhaka",           360, TZ_DST_NONE,  "Dhaka"          },
    { "dubai",           240, TZ_DST_NONE,  "Dubai"          },
    { "dublin",            0, TZ_DST_EU,    "Dublin"         },
    { "fiji",            720, TZ_DST_NONE,  "Fiji"           },
    { "guam",            600, TZ_DST_NONE,  "Guam"           },
    { "halifax",        -240, TZ_DST_US,    "Halifax"        },
    { "hanoi",           420, TZ_DST_NONE,  "Hanoi"          },
    { "helsinki",        120, TZ_DST_EU,    "Helsinki"       },
    { "hongkong",        480, TZ_DST_NONE,  "Hong Kong"      },
    { "honolulu",       -600, TZ_DST_NONE,  "Honolulu"       },
    { "istanbul",        180, TZ_DST_NONE,  "Istanbul"       },
    { "jakarta",         420, TZ_DST_NONE,  "Jakarta"        },
    { "johannesburg",    120, TZ_DST_NONE,  "Johannesburg"   },
    { "kabul",           270, TZ_DST_NONE,  "Kabul"          },
    { "karachi",         300, TZ_DST_NONE,  "Karachi"        },
    { "kathmandu",       345, TZ_DST_NONE,  "Kathmandu"      },
    { "kiritimati",      840, TZ_DST_NONE,  "Kiritimati"     },
    { "kyiv",            120, TZ_DST_EU,    "Kyiv"           },
    { "lagos",            60, TZ_DST_NONE,  "Lagos"          },
    { "lima",           -300, TZ_DST_NONE,  "Lima"           },
    { "lisbon",            0, TZ_DST_EU,    "Lisbon"         },
    { "london",            0, TZ_DST_EU,    "London"         },
    { "losangeles",     -480, TZ_DST_US,    "Los Angeles"    },
    { "madrid",           60, TZ_DST_EU,    "Madrid"         },
    { "manila",          480, TZ_DST_NONE,  "Manila"         },
    { "melbourne",       600, TZ_DST_NONE,  "Melbourne"      },
    { "mexicocity",     -360, TZ_DST_NONE,  "Mexico City"    },
    { "midway",         -660, TZ_DST_NONE,  "Midway"         },
    { "montevideo",     -180, TZ_DST_NONE,  "Montevideo"     },
    { "moscow",          180, TZ_DST_NONE,  "Moscow"         },
    { "nairobi",         180, TZ_DST_NONE,  "Nairobi"        },
    { "newfoundland",   -210, TZ_DST_NONE,  "Newfoundland"   },
    { "newyork",        -300, TZ_DST_US,    "New York"       },
    { "noumea",          660, TZ_DST_NONE,  "Noumea"         },
    { "oslo",             60, TZ_DST_EU,    "Oslo"           },
    { "paris",            60, TZ_DST_EU,    "Paris"          },
    { "perth",           480, TZ_DST_NONE,  "Perth"          },
    { "phoenix",        -420, TZ_DST_NONE,  "Phoenix"        },
    { "prague",           60, TZ_DST_EU,    "Prague"         },
    { "reykjavik",         0, TZ_DST_NONE,  "Reykjavik"      },
    { "riga",            120, TZ_DST_EU,    "Riga"           },
    { "riyadh",          180, TZ_DST_NONE,  "Riyadh"         },
    { "rome",             60, TZ_DST_EU,    "Rome"           },
    { "santiago",       -240, TZ_DST_NONE,  "Santiago"       },
    { "saopaulo",       -180, TZ_DST_NONE,  "Sao Paulo"      },
    { "seoul",           540, TZ_DST_NONE,  "Seoul"          },
    { "singapore",       480, TZ_DST_NONE,  "Singapore"      },
    { "sofia",           120, TZ_DST_EU,    "Sofia"          },
    { "southgeorgia",   -120, TZ_DST_NONE,  "South Georgia"  },
    { "stockholm",        60, TZ_DST_EU,    "Stockholm"      },
    { "sydney",          600, TZ_DST_NONE,  "Sydney"         },
    { "taipei",          480, TZ_DST_NONE,  "Taipei"         },
    { "tallinn",         120, TZ_DST_EU,    "Tallinn"        },
    { "tashkent",        300, TZ_DST_NONE,  "Tashkent"       },
    { "tehran",          210, TZ_DST_NONE,  "Tehran"         },
    { "tokyo",           540, TZ_DST_NONE,  "Tokyo"          },
    { "toronto",        -300, TZ_DST_US,    "Toronto"        },
    { "utc",               0, TZ_DST_NONE,  "UTC"            },
    { "vancouver",      -480, TZ_DST_US,    "Vancouver"      },
    { "vienna",           60, TZ_DST_EU,    "Vienna"         },
    { "vilnius",         120, TZ_DST_EU,    "Vilnius"        },
    { "warsaw",           60, TZ_DST_EU,    "Warsaw"         },
    { "yangon",          390, TZ_DST_NONE,  "Yangon"         },
};
#define TZ_DEFAULT_CITY_COUNT ((int)(sizeof(TZ_DEFAULT_CITIES) / sizeof(TZ_DEFAULT_CITIES[0])))

#define TZ_DB_FILE "/etc/timezones" // the database -- see this file's top comment

#define TZ_CONFIG_FILE "/etc/toyos.conf"
#define TZ_CONFIG_KEY "timezone"

static int current_index = 0; // UTC until tz_init() loads/sets otherwise

// ---- date math ----

// The calendar arithmetic itself now lives in kernel/lib/caltime.c,
// which is freestanding and therefore shareable with ring 3's <time.h>
// -- this file is not, because it also owns the /etc/timezones
// database, the persisted city choice and the DST rules below. Those
// are POLICY; the Gregorian calendar is not. See caltime.h.
#define is_leap_year(y)        cal_is_leap(y)
#define days_in_month(y, m)    cal_days_in_month((y), (m))
#define day_of_week(y, m, d)   cal_day_of_week((y), (m), (d))

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

// ---- civil <-> epoch conversion ----
//
// Pure calendar arithmetic (Howard Hinnant's days_from_civil /
// civil_from_days), no timezone knowledge: the rtc_time in and out is
// whatever calendar time the caller had, and the "epoch" is seconds
// since 1970-01-01 00:00:00 *in that same reckoning*. The filesystem
// feeds these LOCAL times (rtc_read_local()), so its stored epochs are
// local-derived -- deliberately, and documented at the call sites: this
// makes timestamps arithmetic-comparable, it does not invent UTC
// handling the kernel doesn't have. Lives here because this file
// already owns every other piece of calendar math in the kernel.

uint64_t tz_rtc_to_epoch(const struct rtc_time *t) {
    int64_t days = cal_days_from_civil((int)t->year, (int)t->month, (int)t->day);
    // Years below 1970 cannot come off this hardware path (the RTC
    // reports a real current date); clamp rather than underflow the
    // unsigned result. caltime returns a SIGNED day count precisely so
    // this decision is the caller's.
    if (days < 0) return 0;
    return (uint64_t)days * 86400u
         + (uint64_t)t->hour * 3600u
         + (uint64_t)t->minute * 60u
         + (uint64_t)t->second;
}

void tz_epoch_to_rtc(uint64_t epoch, struct rtc_time *out) {
    uint64_t days = epoch / 86400u;
    uint32_t rem = (uint32_t)(epoch % 86400u);
    out->hour = (uint8_t)(rem / 3600u);
    out->minute = (uint8_t)((rem % 3600u) / 60u);
    out->second = (uint8_t)(rem % 60u);

    int y, m, d;
    cal_civil_from_days((int64_t)days, &y, &m, &d);
    out->year = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
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

// k_isblank() is string.h's now -- line-oriented, so NOT k_isspace().
#define is_tz_space(c) k_isblank(c)

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
// one "name,offset_minutes,dst,display name" per line, e.g.
// "helsinki,120,eu,Helsinki" or "losangeles,-480,us,Los Angeles" --
// `dst` is "eu", "us", or anything else (including empty) for no DST,
// and the display name is OPTIONAL (see below for what a row without
// one gets). '#' starts a comment to end of line (whole-line
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

        // The FOURTH field is optional: a row written before display
        // names existed has three, and stays valid.
        const char *f3 = p, *f3_end = content_end;
        const char *f4 = 0, *f4_end = content_end;
        for (const char *q = f3; q < content_end; q++) {
            if (*q == ',') { f3_end = q; f4 = q + 1; break; }
        }

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

        // No display name in the file? Ask the compiled-in table for
        // one, by name. THAT IS WHAT MAKES AN EXISTING DISK WORK: this
        // file is seeded once and never rewritten, so a machine that
        // booted before display names existed has 92 three-field rows
        // and would otherwise show tokens forever. A row that DOES
        // carry a name wins, so a hand-added city can name itself and a
        // hand-renamed one stays renamed.
        TZ_CITIES[count].label[0] = '\0';
        if (f4) {
            const char *ls2 = f4, *le2 = f4_end;
            uint32_t llen = tz_trim(&ls2, le2);
            if (llen > 0 && llen < TZ_LABEL_MAX) {
                k_memcpy(TZ_CITIES[count].label, ls2, llen);
                TZ_CITIES[count].label[llen] = '\0';
            }
        }
        if (!TZ_CITIES[count].label[0]) {
            for (int d = 0; d < TZ_DEFAULT_CITY_COUNT; d++) {
                if (k_strcmp(TZ_DEFAULT_CITIES[d].name, TZ_CITIES[count].name) == 0) {
                    k_strlcpy(TZ_CITIES[count].label, TZ_DEFAULT_CITIES[d].label,
                              sizeof TZ_CITIES[count].label);
                    break;
                }
            }
        }
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
    // STATIC, not a local: the shipped list is ~1.9 KB and the kernel's
    // frame budget is 1 KiB (-Wframe-larger-than), on a 16 KiB stack
    // with one guard page. This runs once, at boot, from a single
    // context, so there is nothing to share it with.
    //
    // It was `char buf[512]` and SILENTLY WROTE 27 OF 92 CITIES -- the
    // loop below simply stopped when the next line would not fit, and
    // nothing said so. Sized from the table now, so it cannot truncate,
    // and the break below is a backstop rather than the normal path.
    static char buf[TZ_DEFAULT_CITY_COUNT * (TZ_NAME_MAX + TZ_LABEL_MAX + 16) + 1];
    uint32_t len = 0;
    static const char *const DST_NAMES[] = { "none", "eu", "us" };
    for (int i = 0; i < TZ_DEFAULT_CITY_COUNT; i++) {
        const struct tz_city *c = &TZ_DEFAULT_CITIES[i];
        uint32_t nlen = (uint32_t)k_strlen(c->name);
        // "name,offset,dst\n" -- 32 bytes of headroom per line is
        // generous (an offset is at most a sign + 3 digits, dst at
        // most "none"'s 4 chars).
        // Cannot fire with the buffer sized from the table above -- kept
        // as a backstop, because the day somebody adds a longer name is
        // the day a silent truncation would come back.
        if (len + nlen + (uint32_t)k_strlen(c->label) + 32 >= sizeof(buf)) {
            klog_write("tz: default city list truncated -- buffer too small\n");
            break;
        }
        k_memcpy(buf + len, c->name, nlen); len += nlen;
        buf[len++] = ',';
        len += tz_format_int(buf + len, c->base_offset_minutes);
        buf[len++] = ',';
        const char *dst = DST_NAMES[c->dst_rule];
        uint32_t dlen = (uint32_t)k_strlen(dst);
        k_memcpy(buf + len, dst, dlen); len += dlen;
        buf[len++] = ',';
        uint32_t llen = (uint32_t)k_strlen(c->label);
        k_memcpy(buf + len, c->label, llen); len += llen;
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
INITCALL(tz_init, INIT_CONFIG);

int tz_city_count(void) {
    return tz_city_count_loaded;
}

const char *tz_city_name(int index) {
    if (index < 0 || index >= tz_city_count_loaded) return 0;
    return TZ_CITIES[index].name;
}

const char *tz_city_label(int index) {
    if (index < 0 || index >= tz_city_count_loaded) return 0;
    const struct tz_city *c = &TZ_CITIES[index];
    return c->label[0] ? c->label : c->name;
}

int tz_current_index(void) {
    return current_index;
}

int tz_set_index(int index) {
    if (index < 0 || index >= tz_city_count_loaded) return SETTING_INVALID;
    // The selection applies either way -- it is in memory and every
    // rtc_read_local() honours it from here on. Only the PERSISTENCE
    // can fail, so that is the only part of the answer worth splitting.
    current_index = index;
    return etc_config_set(TZ_CONFIG_FILE, TZ_CONFIG_KEY, TZ_CITIES[index].name)
               ? SETTING_SAVED : SETTING_UNSAVED;
}

// --- the registry descriptor (see setting.h) -------------------------
//
// The choice list is the loaded city database, so a hand-edited
// /etc/timezones changes what a settings UI offers with no code change
// -- the same property tz_load_or_seed_db() already gave the shell's
// `timezone` command.

static int tz_choice(int index, char *out, uint32_t out_size) {
    const char *name = tz_city_name(index);
    if (!name) return 0;
    k_strlcpy(out, name, out_size);
    return 1;
}

// The display half of tz_choice(), for a settings UI. Registered as
// `choice_label` (setting.h) because these names come from DATA -- the
// loaded database -- and /etc/settings.d cannot hold names for a list
// whose contents it does not know. A Choice.<value>= line still wins,
// so an installation may still rename one.
static int tz_choice_label(int index, char *out, uint32_t out_size) {
    const char *label = tz_city_label(index);
    if (!label) return 0;
    k_strlcpy(out, label, out_size);
    return 1;
}

static void tz_get(char *out, uint32_t out_size) {
    const char *name = tz_city_name(tz_current_index());
    k_strlcpy(out, name ? name : "", out_size);
}

static int tz_apply(const char *value) {
    int idx = tz_find_by_name(value);
    if (idx < 0) return SETTING_INVALID;
    return tz_set_index(idx);
}

static const struct setting g_tz_setting = {
    .name   = TZ_CONFIG_KEY,
    .label  = "Time zone",
    .type   = SETTING_TYPE_ENUM,
    .file   = TZ_CONFIG_FILE,
    .category = "Time & Locale",
    .choice = tz_choice,
    .choice_label = tz_choice_label,
    .get    = tz_get,
    .apply  = tz_apply,
};

void tz_setting_register(void) { setting_register(&g_tz_setting); }

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
