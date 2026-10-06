#ifndef ULIB_URENAME_H
#define ULIB_URENAME_H

// RENAMING MANY FILES AT ONCE: the new names a rule gives a set of names,
// and whether the set can be renamed as planned -- Dolphin's "Rename
// items", Thunar's Bulk Rename, PowerToys' PowerRename. The rule is one
// of three:
//
//   NUMBER   "photo-##" -> photo-01, photo-02, ... A run of '#' is the
//            number, zero-padded to the run's length (or `digits`, if
//            more); no '#' at all puts " N" at the end.
//   REPLACE  every `find` in the name becomes `replace` -- case-
//            insensitively unless `match_case`.
//   CASE     lower, UPPER, or Title Case (each word's first letter).
//
// With `keep_ext` the extension ("dusk.JPG" -> ".JPG") is left exactly
// as it was and the rule sees only the stem, which is what every one of
// those tools defaults to: a rename that turned photos into "photo-01"
// with no ".jpg" would lose what opens them.
//
// THE NAME LOGIC TOUCHES NOTHING (urename_name); urename_plan() alone
// reads the directory, to find a new name that something else already
// has.

#define URENAME_NAME 64          // a component, as struct sys_dirent's

enum { URENAME_NUMBER, URENAME_REPLACE, URENAME_CASE };
enum { URENAME_LOWER, URENAME_UPPER, URENAME_TITLE };

struct urename_rule {
    int mode;
    char pattern[URENAME_NAME];        // NUMBER
    int start, digits;                 // NUMBER: the first number, and at least this many digits
    char find[URENAME_NAME], replace[URENAME_NAME];
    int match_case;                    // REPLACE
    int casing;                        // CASE: URENAME_LOWER/UPPER/TITLE
    int keep_ext;
};

// The new name for `name`, the `i`th of the set (0-based). 1, or 0 when
// the rule makes nothing usable of it (empty, too long, or a '/').
int urename_name(const struct urename_rule *r, const char *name, int i, char *out, int cap);

// What renaming would do to each name.
enum { URENAME_OK, URENAME_SAME, URENAME_BAD, URENAME_CLASH };

// Every new name and its status. CLASH: two of the set would get the
// same name, or one would take the name of something in `dir` that is
// not itself being renamed. Returns how many would change; 0 with any
// BAD or CLASH means do not rename (the plan says which).
int urename_plan(const struct urename_rule *r, const char *dir, char (*names)[URENAME_NAME],
                 int n, char (*news)[URENAME_NAME], int *status);

#endif
