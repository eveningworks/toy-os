#ifndef ULIB_UBOOTWORDS_H
#define ULIB_UBOOTWORDS_H

// The kernel's boot words, as a table: GENERATED from docs/boot-flags.md
// (tools/gen_bootwords.py), which is the only list of them -- the kernel
// matches its command line by substring and keeps no registry. For
// `bootcfg` and the Boot Manager, which refuse a word not on it.

struct ubootword {
    const char *key;   // what a word must start with: "video=", "nokaslr"
    const char *hint;  // the value's shape, "<W>x<H>", or ""
    const char *desc;  // one clause, for a list row
};

int ubootword_count(void);
const struct ubootword *ubootword_at(int i);

// The entry a word as written on a command line belongs to
// ("video=1920x1080" -> "video="), or NULL. A `key=` word needs a value;
// a bare key takes one only when its hint offers "[=...]".
const struct ubootword *ubootword_find(const char *word);

// The nearest key to a word that is NOT one, for "did you mean" -- an
// edit distance of at most 2 on the part before any '=', or NULL.
const struct ubootword *ubootword_suggest(const char *word);

#endif
