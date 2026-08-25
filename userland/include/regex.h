#ifndef _REGEX_H
#define _REGEX_H

// POSIX regular expressions for tolibc.
//
// WHY THIS IS IN THE LIBRARY AND NOT INSIDE /bin/grep. tolibc's bar is
// the OPPOSITE of the rest of this project (see docs/libc-design.md):
// complete rather than second-real-caller, because its audience is code
// not yet written. `grep` is the first caller; `sed` and `awk` are the
// obvious next ones, and a matcher private to one command is a matcher
// they would each reimplement.
//
// THE ENGINE IS A THOMPSON NFA, SIMULATED. It tracks a SET of active
// states rather than backtracking, so matching is O(pattern x text) and
// there is no input that makes it explode -- `(a*)*b` against a long
// run of `a` is linear here and is the classic catastrophic case for a
// backtracking engine. That property is the reason for the choice: a
// regex is attacker-shaped data in exactly the way a font file is, and
// this one is fed strings from files and pipes.
//
// WHAT IT DOES NOT DO, stated rather than discovered:
//
//   - **Submatch positions are not full POSIX.** The overall match
//     (pmatch[0]) is leftmost-longest, which is POSIX's rule. Group
//     positions come from whichever thread produced that match, which
//     is leftmost-first (Perl's rule) where the two disagree. Getting
//     true POSIX submatch disambiguation out of an NFA simulation is a
//     genuinely hard problem and no caller here needs it.
//   - **No back-references** (`\1` inside a pattern). They are what
//     makes matching NP-hard and are precisely what an NFA cannot do;
//     POSIX puts them in BRE only, and regcomp() reports REG_BADPAT.
//   - **No collating elements or equivalence classes** (`[[.a.]]`,
//     `[[=a=]]`). Named classes (`[[:digit:]]`) ARE supported.
//   - **Single-byte only.** This system has no Unicode anywhere else
//     either; a byte is a character.

#include <stddef.h>
#include <sys/types.h>

typedef ptrdiff_t regoff_t;

typedef struct {
    size_t re_nsub;   // number of parenthesised subexpressions
    void  *re_prog;   // opaque compiled program -- regfree() owns it
} regex_t;

typedef struct {
    regoff_t rm_so;   // byte offset of the match start, -1 if unset
    regoff_t rm_eo;   // byte offset one past the match end
} regmatch_t;

// regcomp() cflags
#define REG_EXTENDED 0x01 // ERE rather than BRE -- see the note below
#define REG_ICASE    0x02 // case-insensitive
#define REG_NOSUB    0x04 // report match/no-match only; ignore pmatch
#define REG_NEWLINE  0x08 // '.' and a negated class do not match '\n',
                          // and ^/$ anchor at embedded newlines too

// regexec() eflags
#define REG_NOTBOL   0x01 // the string's start is not a line start
#define REG_NOTEOL   0x02 // the string's end is not a line end

// Return codes. REG_NOMATCH is 1 rather than 0 because 0 is success.
#define REG_NOMATCH   1
#define REG_BADPAT    2
#define REG_ECOLLATE  3
#define REG_ECTYPE    4
#define REG_EESCAPE   5
#define REG_ESUBREG   6
#define REG_EBRACK    7
#define REG_EPAREN    8
#define REG_EBRACE    9
#define REG_BADBR    10
#define REG_ERANGE   11
#define REG_ESPACE   12
#define REG_BADRPT   13

// BRE vs ERE, since the difference is pure lexis and trips everyone:
// without REG_EXTENDED, `+ ? |` are ORDINARY characters and grouping
// and intervals are spelled `\( \) \{ \}`. With it, those are the
// operators and a backslash makes them literal. `/bin/grep` passes
// REG_EXTENDED, which is a deliberate divergence from POSIX's grep --
// BRE's escaping rules are decades of compatibility baggage this OS has
// no reason to inherit (CLAUDE.md's "copy the SHAPE, not the size").
//
// One documented simplification in BRE mode: `^` is an anchor at the
// start of the pattern or immediately after `\(`, and `$` at the end or
// immediately before `\)`; elsewhere both are literal. POSIX words it
// slightly more broadly.
int    regcomp(regex_t *preg, const char *pattern, int cflags);
int    regexec(const regex_t *preg, const char *string,
               size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void   regfree(regex_t *preg);

#endif
