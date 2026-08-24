// The shared kfmt case table -- see api/kfmt_cases.h for why it is
// shared and why it is exhaustive rather than interesting.
#include "kfmt_cases.h"
#include "kfmt.h"
#include "string.h"
#include <stdint.h>

const struct kfmt_case kfmt_cases[] = {
    // --- the plain conversions ---------------------------------------
    { "%d",      KFMT_ARG_INT,   42, 0, 0, "42" },
    { "%d",      KFMT_ARG_INT,  -42, 0, 0, "-42" },
    { "%i",      KFMT_ARG_INT,   42, 0, 0, "42" },   // C's alias for %d
    { "%u",      KFMT_ARG_UINT, 42, 0, 0, "42" },
    { "%x",      KFMT_ARG_UINT, 0xdeadbeef, 0, 0, "deadbeef" },
    { "%X",      KFMT_ARG_UINT, 0xdeadbeef, 0, 0, "DEADBEEF" },
    { "%o",      KFMT_ARG_UINT, 0644, 0, 0, "644" },
    { "%c",      KFMT_ARG_CHAR, 'g', 0, 0, "g" },
    { "%s",      KFMT_ARG_STR,  0, 0, "hello", "hello" },
    // A NULL string is "(null)" rather than a fault -- glibc's
    // behaviour, and the one that keeps a log line from taking the
    // machine down with it.
    { "%s",      KFMT_ARG_STR,  0, 0, 0, "(null)" },
    { "%%",      KFMT_ARG_NONE, 0, 0, 0, "%" },
    { "%p",      KFMT_ARG_PTR,  0x1000, 0, 0, "0x1000" },
    { "%p",      KFMT_ARG_PTR,  0, 0, 0, "(nil)" },

    // --- width -------------------------------------------------------
    { "[%5d]",   KFMT_ARG_INT,   42, 0, 0, "[   42]" },
    { "[%-5d]",  KFMT_ARG_INT,   42, 0, 0, "[42   ]" },
    { "[%05d]",  KFMT_ARG_INT,   42, 0, 0, "[00042]" },
    // The '0' flag pads the whole FIELD, sign included.
    { "[%08d]",  KFMT_ARG_INT,   -7, 0, 0, "[-0000007]" },
    { "[%5s]",   KFMT_ARG_STR,   0, 0, "ab", "[   ab]" },
    { "[%-5s]",  KFMT_ARG_STR,   0, 0, "ab", "[ab   ]" },
    // A string longer than its field PUSHES the column rather than
    // being truncated -- truncating would change the value.
    { "[%2s]",   KFMT_ARG_STR,   0, 0, "abcdef", "[abcdef]" },
    { "[%04x]",  KFMT_ARG_UINT, 0x2a, 0, 0, "[002a]" },
    { "[%04X]",  KFMT_ARG_UINT, 0x2a, 0, 0, "[002A]" },
    { "[%-6o]",  KFMT_ARG_UINT, 0644, 0, 0, "[644   ]" },

    // --- precision ---------------------------------------------------
    // A precision on an integer is a MINIMUM DIGIT COUNT. This is the
    // one Doom found.
    { "%.3d",    KFMT_ARG_INT,   33, 0, 0, "033" },
    { "%.8d",    KFMT_ARG_INT,   -7, 0, 0, "-00000007" },
    // C: a precision makes the '0' flag ignored.
    { "[%08.3d]",KFMT_ARG_INT,   42, 0, 0, "[     042]" },
    // ...and an explicit precision of zero prints NOTHING for zero,
    // which is how an optional field disappears when empty.
    { "[%.0d]",  KFMT_ARG_INT,   0, 0, 0, "[]" },
    { "[%.0d]",  KFMT_ARG_INT,   7, 0, 0, "[7]" },

    // --- the sign and alternate flags --------------------------------
    { "%+d",     KFMT_ARG_INT,   42, 0, 0, "+42" },
    // '+' applies to a non-negative value only; a negative one already
    // has its sign.
    { "%+d",     KFMT_ARG_INT,  -42, 0, 0, "-42" },
    { "% d",     KFMT_ARG_INT,   42, 0, 0, " 42" },
    // '+' wins when both are given, which is what C says.
    { "%+ d",    KFMT_ARG_INT,   42, 0, 0, "+42" },
    { "%#x",     KFMT_ARG_UINT, 0x2a, 0, 0, "0x2a" },
    { "%#X",     KFMT_ARG_UINT, 0x2a, 0, 0, "0X2A" },
    // '#' on ZERO adds nothing -- C exempts it, and "0x0" would be
    // wider than the value it describes.
    { "%#x",     KFMT_ARG_UINT, 0, 0, 0, "0" },
    { "%#o",     KFMT_ARG_UINT, 0644, 0, 0, "0644" },

    // --- length modifiers --------------------------------------------
    { "%ld",     KFMT_ARG_LONG,  -1234567890123LL, 0, 0, "-1234567890123" },
    { "%lu",     KFMT_ARG_ULONG, 1234567890123ULL, 0, 0, "1234567890123" },
    { "%lx",     KFMT_ARG_ULONG, 0xfeedfacecafeULL, 0, 0, "feedfacecafe" },
    { "%zu",     KFMT_ARG_ULONG, 4096, 0, 0, "4096" },
    // h and hh are ACCEPTED AND IGNORED: promotion has already widened
    // the argument, so there is nothing narrower to read. What matters
    // is that they are consumed -- see the pairs below.
    { "%hd",     KFMT_ARG_INT,   42, 0, 0, "42" },
    { "%hhd",    KFMT_ARG_INT,   42, 0, 0, "42" },
    { "%hu",     KFMT_ARG_UINT,  42, 0, 0, "42" },

    // --- an UNRECOGNISED conversion ----------------------------------
    // Emitted literally and consuming nothing, which is the documented
    // behaviour and the reason every case below exists.
    { "[%q]",    KFMT_ARG_NONE,  0, 0, 0, "[%q]" },
    // A format ending mid-conversion must stop cleanly rather than run
    // off the end of the string.
    { "abc%",    KFMT_ARG_NONE,  0, 0, 0, "abc%" },

    // --- ARGUMENT CONSUMPTION ----------------------------------------
    //
    // **THE HALF THAT ACTUALLY CATCHES THE BUG.** Each of these puts a
    // second %d after the conversion under test and pins ITS value. A
    // conversion that is not parsed consumes no argument, so the 71
    // below arrives as whatever the first argument was -- which is
    // exactly how /bin/font printed a codepoint where a slot number
    // belonged. A case asserting only the first conversion's rendering
    // passes happily while the rest of the line is wrong.
    { "%d/%d",   KFMT_ARG_INT_INT, 103, 71, 0, "103/71" },
    { "%X/%d",   KFMT_ARG_INT_INT, 0x67, 71, 0, "67/71" },
    { "%o/%d",   KFMT_ARG_INT_INT, 8, 71, 0, "10/71" },
    { "%+d/%d",  KFMT_ARG_INT_INT, 5, 71, 0, "+5/71" },
    { "% d/%d",  KFMT_ARG_INT_INT, 5, 71, 0, " 5/71" },
    { "%#x/%d",  KFMT_ARG_INT_INT, 0x2a, 71, 0, "0x2a/71" },
    { "%hd/%d",  KFMT_ARG_INT_INT, 5, 71, 0, "5/71" },
    { "%hhd/%d", KFMT_ARG_INT_INT, 5, 71, 0, "5/71" },
    { "%.3d/%d", KFMT_ARG_INT_INT, 5, 71, 0, "005/71" },
    { "%-4d/%d", KFMT_ARG_INT_INT, 5, 71, 0, "5   /71" },
    // The unrecognised one consumes NOTHING, so the first argument is
    // what the following %d must print. This is the case that pins the
    // literal path's contract rather than just its output.
    { "%q/%d",   KFMT_ARG_INT_INT, 103, 71, 0, "%q/103" },

    // --- `*`, the width taken from an argument -----------------------
    //
    // How a caller whose column width is computed at runtime writes it.
    // `edit`'s line-number gutter is the real one: the width comes from
    // the file's line count, so it cannot be a literal. Missing, `%*d`
    // went out as the letters "%*d" AND ate the width as if it were the
    // value -- the fourth instance of this formatter's standing failure,
    // found the same way as the second: by looking at the screen.
    { "[%*d]",   KFMT_ARG_STAR_INT, 5, 42, 0, "[   42]" },
    { "[%-*d]",  KFMT_ARG_STAR_INT, 5, 42, 0, "[42   ]" },
    // A NEGATIVE width means left-justify, which is C's rule and the one
    // place `-` can arrive after the flags have been read.
    { "[%*d]",   KFMT_ARG_STAR_INT, -5, 42, 0, "[42   ]" },
    // A width SMALLER than the number does not truncate it.
    { "[%*d]",   KFMT_ARG_STAR_INT, 1, 4242, 0, "[4242]" },
    { "[%*s]",   KFMT_ARG_STAR_STR, 6, 0, "ab", "[    ab]" },
    // ...and it consumes TWO arguments, so whatever follows must still
    // land on the right one. This is the `*` form of the consumption
    // check every other conversion here gets, and the first version of
    // it was itself malformed -- `"%*d/%d"` needs THREE arguments and
    // the runner passed two, so the trailing %d read garbage and the
    // case failed against a correct formatter. A case can be wrong in
    // exactly the way the thing it tests is wrong.
    { "%*d/%s",  KFMT_ARG_STAR_INT_STR, 3, 5, "tail", "  5/tail" },

    // --- the real line that exposed all this -------------------------
    { "U+%04X slot %d", KFMT_ARG_INT_INT, 0x67, 71, 0, "U+0067 slot 71" },
};

const int kfmt_case_count = (int)(sizeof kfmt_cases / sizeof kfmt_cases[0]);

int kfmt_case_run(const struct kfmt_case *c, char *out, int cap) {
    if (cap > 0) out[0] = '\0';

    // The switch is what makes the table honest: each arm passes the
    // argument with its REAL static type, so default promotion happens
    // the way it would at any other call site. A table that stored one
    // widened value and passed it everywhere would be testing the
    // table, not the formatter.
    switch (c->kind) {
    case KFMT_ARG_NONE:
        k_snprintf(out, (size_t)cap, c->fmt);
        break;
    case KFMT_ARG_INT:
        k_snprintf(out, (size_t)cap, c->fmt, (int)c->a);
        break;
    case KFMT_ARG_UINT:
        k_snprintf(out, (size_t)cap, c->fmt, (unsigned int)c->a);
        break;
    case KFMT_ARG_LONG:
        k_snprintf(out, (size_t)cap, c->fmt, (long)c->a);
        break;
    case KFMT_ARG_ULONG:
        k_snprintf(out, (size_t)cap, c->fmt, (unsigned long)c->a);
        break;
    case KFMT_ARG_STR:
        k_snprintf(out, (size_t)cap, c->fmt, c->s);
        break;
    case KFMT_ARG_CHAR:
        k_snprintf(out, (size_t)cap, c->fmt, (int)c->a);
        break;
    case KFMT_ARG_PTR:
        k_snprintf(out, (size_t)cap, c->fmt, (void *)(uintptr_t)c->a);
        break;
    case KFMT_ARG_INT_INT:
        k_snprintf(out, (size_t)cap, c->fmt, (int)c->a, (int)c->b);
        break;
    // `a` is the WIDTH and `b` is the value -- two ints either way, but
    // named apart from KFMT_ARG_INT_INT because what the first one MEANS
    // is the whole point of these cases.
    case KFMT_ARG_STAR_INT:
        k_snprintf(out, (size_t)cap, c->fmt, (int)c->a, (int)c->b);
        break;
    case KFMT_ARG_STAR_STR:
        k_snprintf(out, (size_t)cap, c->fmt, (int)c->a, c->s);
        break;
    case KFMT_ARG_STAR_INT_STR:
        k_snprintf(out, (size_t)cap, c->fmt, (int)c->a, (int)c->b, c->s);
        break;
    }
    return k_strcmp(out, c->want) == 0;
}
