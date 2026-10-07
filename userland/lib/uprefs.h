#ifndef ULIB_UPREFS_H
#define ULIB_UPREFS_H

// uprefs -- an app's options file described as a TABLE: one row per key
// names the field it fills, its type, its range or words and its default,
// and load, save and Defaults all walk that one table -- so they cannot
// disagree about which keys exist or what they default to. KDE's
// KConfigXT (a .kcfg generating the settings class) in this OS's shape:
// `name=value` through lib/uconf.h, booleans as on/off, enums as WORDS.
// The window half is ui/uui_prefs.h's bound rows, which edit the same
// struct by the same offsets.
//
// A VALUE THIS CANNOT READ LEAVES THE DEFAULT: a typo in the file is a key
// ignored, never an option silently set to zero. Anything a table cannot
// say -- a palette, a key bound elsewhere -- stays the app's, beside it.

#include <stddef.h>

enum {
    UPREF_BOOL,   // int; `on` / `off` (read: `1`/`0` and `yes`/`no` too)
    UPREF_INT,    // int in lo..hi, decimal
    UPREF_WORD,   // int index into `words` (`hi` of them); written as the word
    UPREF_TEXT,   // char[hi]; `valid`, when set, refuses a value it does not take
};

struct upref {
    const char *key;
    size_t off;                       // offsetof the field in the app's struct
    int type;
    int lo, hi;                       // INT's range; WORD's count; TEXT's buffer size
    const char *const *words;         // WORD's spellings in the file
    int dflt;                         // BOOL / INT / WORD default
    const char *dflt_text;            // TEXT default
    int (*valid)(const char *value);  // TEXT: 1 to take it
};

struct uprefs {
    const char *path;                 // the options file
    const struct upref *keys;
    int count;
};

#define UPREF_BOOL_KEY(k, type, field, d)     { k, offsetof(type, field), UPREF_BOOL, 0, 0, 0, d, 0, 0 }
#define UPREF_INT_KEY(k, type, field, l, h, d) { k, offsetof(type, field), UPREF_INT, l, h, 0, d, 0, 0 }
#define UPREF_WORD_KEY(k, type, field, w, n, d) { k, offsetof(type, field), UPREF_WORD, 0, n, w, d, 0, 0 }
#define UPREF_TEXT_KEY(k, type, field, d, ok)  \
    { k, offsetof(type, field), UPREF_TEXT, 0, (int)sizeof(((type *)0)->field), 0, 0, d, ok }

// Every key's default into `obj`. The app's other fields are untouched,
// so it zeroes or sets them itself first.
void uprefs_defaults(const struct uprefs *p, void *obj);

// The defaults, then whatever the file says. One read of the file.
void uprefs_load(const struct uprefs *p, void *obj);
// Only what the file says, over whatever `obj` holds -- for an app whose
// defaults are more than the table's (a preset filling a sub-struct).
void uprefs_read(const struct uprefs *p, void *obj);

// Writes each key whose value differs from `was` -- every key when `was`
// is NULL. The number written, or -1 if any write failed: a save that
// failed on the fifth key has half-applied the change and must say so.
int uprefs_save(const struct uprefs *p, const void *obj, const void *was);

#endif
