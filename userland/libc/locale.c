// The C locale, and only the C locale -- see <locale.h> for why that is
// a decision rather than a gap.
#include <locale.h>
#include <limits.h>
#include <string.h>

static char g_c[] = "C";

char *setlocale(int category, const char *locale) {
    (void)category;
    // A query, or "take it from the environment" -- which here is C
    // whatever the environment says, because there is nothing else to
    // take. Both answer with the locale in force.
    if (!locale || !locale[0]) return g_c;
    if (strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0) return g_c;
    // REFUSED, not ignored. <locale.h> has the reasoning; the short
    // version is that a caller told "yes" formats numbers wrongly and
    // never finds out.
    return 0;
}

// The C locale's conventions, verbatim from the C standard: a '.'
// decimal point, no grouping, and "" for everything monetary.
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

struct lconv *localeconv(void) { return &g_lconv; }
