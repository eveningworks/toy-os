// See ubootwords.h.
#include "lib/ubootwords.h"
#include <string.h>
#include "bootwords.h"   // build/gen, from docs/boot-flags.md

#define NWORDS ((int)(sizeof UBOOTWORDS / sizeof UBOOTWORDS[0]))

int ubootword_count(void) { return NWORDS; }

const struct ubootword *ubootword_at(int i) {
    return i >= 0 && i < NWORDS ? &UBOOTWORDS[i] : 0;
}

const struct ubootword *ubootword_find(const char *word) {
    for (int i = 0; i < NWORDS; i++) {
        const struct ubootword *w = &UBOOTWORDS[i];
        size_t k = strlen(w->key);
        if (strncmp(word, w->key, k)) continue;
        if (w->key[k - 1] == '=') {
            if (word[k]) return w;
        } else if (!word[k] || (word[k] == '=' && word[k + 1] && !strncmp(w->hint, "[=", 2))) {
            return w;
        }
    }
    return 0;
}

// Levenshtein over the name part, capped: anything past 2 is "no".
static int distance(const char *a, int na, const char *b, int nb) {
    int row[40];
    if (na >= 40 || nb >= 40) return 99;
    for (int j = 0; j <= nb; j++) row[j] = j;
    for (int i = 1; i <= na; i++) {
        int diag = row[0];
        row[0] = i;
        for (int j = 1; j <= nb; j++) {
            int up = row[j];
            int best = diag + (a[i - 1] != b[j - 1]);
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            if (up + 1 < best) best = up + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[nb];
}

static int name_len(const char *s) {
    const char *eq = strchr(s, '=');
    return eq ? (int)(eq - s) : (int)strlen(s);
}

const struct ubootword *ubootword_suggest(const char *word) {
    const struct ubootword *best = 0;
    int best_d = 3;
    for (int i = 0; i < NWORDS; i++) {
        const char *k = UBOOTWORDS[i].key;
        int d = distance(word, name_len(word), k, name_len(k));
        if (d < best_d) { best_d = d; best = &UBOOTWORDS[i]; }
    }
    return best;
}
