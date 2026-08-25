// POSIX regular expressions -- a Thompson NFA, compiled and simulated.
//
// See userland/include/regex.h for the API and for what this
// deliberately does not implement. This file is the how.
//
// THE SHAPE, in three parts:
//
//   1. A recursive-descent parser over the pattern emits a PROGRAM --
//      a flat array of instructions, the same idea as a bytecode.
//      Jumps are RELATIVE, which is what makes `{n,m}` a memcpy of an
//      instruction range rather than a rewrite of every target in it.
//   2. regexec() runs that program as a Pike VM: a list of threads,
//      each a program counter plus capture slots, all advanced one
//      input byte at a time. A state already in the list is never
//      added twice, which is the entire reason this cannot blow up --
//      the thread list is bounded by the program length, so the work
//      is O(pattern x text) whatever the input.
//   3. Character classes are 256-bit bitmaps, so `[a-z0-9_]` and
//      `[[:alpha:]]` and `.` all cost one array lookup.
//
// WHY NOT BACKTRACKING, which is shorter to write: `(a*)*b` against
// thirty `a`s takes a backtracking engine longer than the age of the
// universe, and patterns arrive here from files and command lines. The
// NFA simulation has no such input. This is Thompson's 1968
// construction and Pike's simulation of it, which is also what RE2 and
// Go's regexp use and what Rob Pike wrote for sam.
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

// Bounds. A pattern that needs more than this is REFUSED rather than
// truncated -- a truncated regex silently matches the wrong thing.
// MAX_PROG bounds both the program and, because a thread list cannot
// hold two threads at one pc, the simulation's memory.
#define MAX_PROG   2048
#define MAX_GROUPS 9      // \1..\9 territory; POSIX requires at least 9
#define MAX_SLOTS  (2 * (MAX_GROUPS + 1))
#define MAX_CLASS  64     // distinct bracket expressions in one pattern

enum {
    I_CHAR,   // match one byte, ch
    I_CLASS,  // match one byte in classes[cls]
    I_SPLIT,  // try pc+x first, then pc+y
    I_JMP,    // pc += x
    I_SAVE,   // record the current position in slot x
    I_BOL,    // assert start of string (or line, under REG_NEWLINE)
    I_EOL,    // assert end of string (or line)
    I_MATCH,
};

struct inst {
    uint8_t  op;
    uint8_t  ch;
    uint16_t cls;
    int32_t  x, y;   // RELATIVE offsets; see the top comment
};

struct prog {
    struct inst *code;
    int len;
    uint8_t (*classes)[32];  // 256-bit bitmaps
    int nclass;
    int ngroups;
    int cflags;
};

// ---- parser ---------------------------------------------------------

struct parser {
    const char *p;      // cursor
    const char *end;
    struct prog *prog;
    int ngroups;
    int err;
    int ere;            // REG_EXTENDED
    int icase;
};

static int emit(struct parser *ps, uint8_t op, uint8_t ch, uint16_t cls) {
    if (ps->prog->len >= MAX_PROG) { ps->err = REG_ESPACE; return -1; }
    int at = ps->prog->len++;
    struct inst *i = &ps->prog->code[at];
    i->op = op; i->ch = ch; i->cls = cls; i->x = 0; i->y = 0;
    return at;
}

static void bit_set(uint8_t *bm, int c) { bm[(c >> 3) & 31] |= (uint8_t)(1 << (c & 7)); }
static int  bit_get(const uint8_t *bm, int c) { return (bm[(c >> 3) & 31] >> (c & 7)) & 1; }

static int new_class(struct parser *ps) {
    if (ps->prog->nclass >= MAX_CLASS) { ps->err = REG_ESPACE; return -1; }
    int idx = ps->prog->nclass++;
    memset(ps->prog->classes[idx], 0, 32);
    return idx;
}

// Case folding is applied AT COMPILE TIME, by widening the class or by
// emitting a two-element class for a literal. Doing it in the VM would
// mean a tolower() per byte per thread; doing it here costs nothing at
// match time and keeps the simulation loop honest.
static void class_add_folded(struct parser *ps, uint8_t *bm, int c) {
    bit_set(bm, c);
    if (ps->icase) {
        if (isalpha((unsigned char)c)) {
            bit_set(bm, tolower(c));
            bit_set(bm, toupper(c));
        }
    }
}

static const struct { const char *name; int (*fn)(int); } NAMED[] = {
    { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum },
    { "space", isspace }, { "upper", isupper }, { "lower", islower },
    { "punct", ispunct }, { "print", isprint }, { "graph", isgraph },
    { "cntrl", iscntrl }, { "blank", NULL },    { "xdigit", isxdigit },
};

// `[...]`. The cursor is just past '['. POSIX's awkward corners are all
// here: a ']' first in the set is a literal, a '-' first or last is a
// literal, and '^' first negates.
static int parse_bracket(struct parser *ps) {
    int idx = new_class(ps);
    if (idx < 0) return -1;
    uint8_t *bm = ps->prog->classes[idx];

    int negate = 0;
    if (ps->p < ps->end && *ps->p == '^') { negate = 1; ps->p++; }

    int first = 1;
    for (;;) {
        if (ps->p >= ps->end) { ps->err = REG_EBRACK; return -1; }
        if (*ps->p == ']' && !first) { ps->p++; break; }
        first = 0;

        // [[:name:]]
        if (*ps->p == '[' && ps->p + 1 < ps->end && ps->p[1] == ':') {
            const char *s = ps->p + 2;
            const char *e = s;
            while (e < ps->end && *e != ':') e++;
            if (e + 1 >= ps->end || e[1] != ']') { ps->err = REG_ECTYPE; return -1; }
            size_t n = (size_t)(e - s);
            int found = 0;
            for (size_t k = 0; k < sizeof NAMED / sizeof NAMED[0]; k++) {
                if (strlen(NAMED[k].name) != n || memcmp(NAMED[k].name, s, n) != 0) continue;
                found = 1;
                for (int c = 0; c < 256; c++) {
                    int in = NAMED[k].fn ? NAMED[k].fn(c) : (c == ' ' || c == '\t');
                    if (in) bit_set(bm, c);
                }
                break;
            }
            if (!found) { ps->err = REG_ECTYPE; return -1; }
            ps->p = e + 2;
            continue;
        }

        int lo = (unsigned char)*ps->p++;
        // A backslash inside a bracket is LITERAL in POSIX. GNU treats
        // \] and \\ as escapes and enough patterns rely on it that
        // refusing would be unhelpful; both readings agree on
        // everything else.
        if (lo == '\\' && ps->p < ps->end) lo = (unsigned char)*ps->p++;

        if (ps->p + 1 < ps->end && *ps->p == '-' && ps->p[1] != ']') {
            ps->p++;
            int hi = (unsigned char)*ps->p++;
            if (hi == '\\' && ps->p < ps->end) hi = (unsigned char)*ps->p++;
            if (hi < lo) { ps->err = REG_ERANGE; return -1; }
            for (int c = lo; c <= hi; c++) class_add_folded(ps, bm, c);
        } else {
            class_add_folded(ps, bm, lo);
        }
    }

    if (negate) {
        for (int i = 0; i < 32; i++) bm[i] = (uint8_t)~bm[i];
        // A negated class never matches a newline under REG_NEWLINE --
        // POSIX says so, and it is what makes a line-oriented tool work
        // on a buffer that still has its separators in it.
        if (ps->prog->cflags & REG_NEWLINE) bm[('\n' >> 3) & 31] &= (uint8_t)~(1 << ('\n' & 7));
    }
    return idx;
}

static int parse_alt(struct parser *ps);

// Is `c` an operator in the current syntax? In BRE the repetition and
// grouping operators are spelled with a backslash, so this is the one
// place the two dialects actually differ.
static int at_group_open(struct parser *ps) {
    if (ps->ere) return *ps->p == '(';
    return *ps->p == '\\' && ps->p + 1 < ps->end && ps->p[1] == '(';
}
static int at_group_close(struct parser *ps) {
    if (ps->p >= ps->end) return 0;
    if (ps->ere) return *ps->p == ')';
    return *ps->p == '\\' && ps->p + 1 < ps->end && ps->p[1] == ')';
}
static int at_alternation(struct parser *ps) {
    if (ps->p >= ps->end) return 0;
    if (ps->ere) return *ps->p == '|';
    return *ps->p == '\\' && ps->p + 1 < ps->end && ps->p[1] == '|';
}
static int at_brace_open(struct parser *ps) {
    if (ps->p >= ps->end) return 0;
    if (ps->ere) return *ps->p == '{';
    return *ps->p == '\\' && ps->p + 1 < ps->end && ps->p[1] == '{';
}

// One atom, leaving its code at [start, prog->len).
static int parse_atom(struct parser *ps) {
    if (ps->p >= ps->end) return ps->prog->len;
    int start = ps->prog->len;

    if (at_group_open(ps)) {
        ps->p += ps->ere ? 1 : 2;
        if (ps->ngroups >= MAX_GROUPS) { ps->err = REG_ESPACE; return -1; }
        int g = ++ps->ngroups;
        if (emit(ps, I_SAVE, 0, 0) < 0) return -1;
        ps->prog->code[ps->prog->len - 1].x = 2 * g;
        if (parse_alt(ps) < 0) return -1;
        if (!at_group_close(ps)) { ps->err = REG_EPAREN; return -1; }
        ps->p += ps->ere ? 1 : 2;
        if (emit(ps, I_SAVE, 0, 0) < 0) return -1;
        ps->prog->code[ps->prog->len - 1].x = 2 * g + 1;
        return start;
    }

    char c = *ps->p;

    if (c == '[') {
        ps->p++;
        int idx = parse_bracket(ps);
        if (idx < 0) return -1;
        if (emit(ps, I_CLASS, 0, (uint16_t)idx) < 0) return -1;
        return start;
    }

    if (c == '.') {
        ps->p++;
        int idx = new_class(ps);
        if (idx < 0) return -1;
        memset(ps->prog->classes[idx], 0xFF, 32);
        if (ps->prog->cflags & REG_NEWLINE) {
            ps->prog->classes[idx][('\n' >> 3) & 31] &= (uint8_t)~(1 << ('\n' & 7));
        }
        if (emit(ps, I_CLASS, 0, (uint16_t)idx) < 0) return -1;
        return start;
    }

    if (c == '\\') {
        if (ps->p + 1 >= ps->end) { ps->err = REG_EESCAPE; return -1; }
        char e = ps->p[1];
        // A back-reference is what an NFA fundamentally cannot do -- it
        // is the feature that makes matching NP-hard. Refused by name
        // rather than silently treated as a literal digit.
        if (e >= '1' && e <= '9') { ps->err = REG_BADPAT; return -1; }
        ps->p += 2;
        int ch;
        switch (e) {
        case 'n': ch = '\n'; break;
        case 't': ch = '\t'; break;
        case 'r': ch = '\r'; break;
        case 'f': ch = '\f'; break;
        case 'v': ch = '\v'; break;
        default:  ch = (unsigned char)e; break;
        }
        if (ps->icase && isalpha(ch)) {
            int idx = new_class(ps);
            if (idx < 0) return -1;
            class_add_folded(ps, ps->prog->classes[idx], ch);
            if (emit(ps, I_CLASS, 0, (uint16_t)idx) < 0) return -1;
        } else if (emit(ps, I_CHAR, (uint8_t)ch, 0) < 0) return -1;
        return start;
    }

    if (c == '^') {
        // ERE: always an anchor. BRE: only at the start of the pattern
        // or right after \( -- see regex.h's note.
        ps->p++;
        if (emit(ps, I_BOL, 0, 0) < 0) return -1;
        return start;
    }
    if (c == '$') {
        ps->p++;
        if (emit(ps, I_EOL, 0, 0) < 0) return -1;
        return start;
    }

    ps->p++;
    if (ps->icase && isalpha((unsigned char)c)) {
        int idx = new_class(ps);
        if (idx < 0) return -1;
        class_add_folded(ps, ps->prog->classes[idx], (unsigned char)c);
        if (emit(ps, I_CLASS, 0, (uint16_t)idx) < 0) return -1;
    } else if (emit(ps, I_CHAR, (uint8_t)(unsigned char)c, 0) < 0) return -1;
    return start;
}

// Copy [from, to) to the end of the program. Legal only because jumps
// are RELATIVE -- with absolute targets this would need every jump in
// the range rewritten, which is the bug `{n,m}` implementations are
// famous for.
static int dup_range(struct parser *ps, int from, int to) {
    int n = to - from;
    if (ps->prog->len + n > MAX_PROG) { ps->err = REG_ESPACE; return -1; }
    memcpy(&ps->prog->code[ps->prog->len], &ps->prog->code[from],
           (size_t)n * sizeof(struct inst));
    ps->prog->len += n;
    return 0;
}

// Wrap [start, len) in the repetition `c` describes.
static int apply_repeat(struct parser *ps, int start, char kind) {
    int n = ps->prog->len - start;
    if (n <= 0) { ps->err = REG_BADRPT; return -1; }

    if (kind == '?') {
        // SPLIT over the atom.
        if (ps->prog->len + 1 > MAX_PROG) { ps->err = REG_ESPACE; return -1; }
        memmove(&ps->prog->code[start + 1], &ps->prog->code[start],
                (size_t)n * sizeof(struct inst));
        ps->prog->len++;
        struct inst *sp = &ps->prog->code[start];
        sp->op = I_SPLIT; sp->ch = 0; sp->cls = 0;
        sp->x = 1;        // into the atom
        sp->y = n + 1;    // past it
        return 0;
    }
    if (kind == '*') {
        if (ps->prog->len + 2 > MAX_PROG) { ps->err = REG_ESPACE; return -1; }
        memmove(&ps->prog->code[start + 1], &ps->prog->code[start],
                (size_t)n * sizeof(struct inst));
        ps->prog->len += 2;
        struct inst *sp = &ps->prog->code[start];
        sp->op = I_SPLIT; sp->ch = 0; sp->cls = 0;
        sp->x = 1; sp->y = n + 2;
        struct inst *jp = &ps->prog->code[start + 1 + n];
        jp->op = I_JMP; jp->ch = 0; jp->cls = 0;
        jp->x = -(n + 1); jp->y = 0;
        return 0;
    }
    if (kind == '+') {
        // atom, then SPLIT back.
        if (emit(ps, I_SPLIT, 0, 0) < 0) return -1;
        struct inst *sp = &ps->prog->code[ps->prog->len - 1];
        sp->x = -n; sp->y = 1;
        return 0;
    }
    ps->err = REG_BADRPT;
    return -1;
}

// `{n,m}` / `{n,}` / `{n}` by duplication.
static int apply_interval(struct parser *ps, int start) {
    ps->p += ps->ere ? 1 : 2;   // past '{' or '\{'
    int lo = 0, hi = -1, digits = 0;
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
        lo = lo * 10 + (*ps->p++ - '0');
        digits++;
        if (lo > 255) { ps->err = REG_BADBR; return -1; }
    }
    if (!digits) { ps->err = REG_BADBR; return -1; }
    if (ps->p < ps->end && *ps->p == ',') {
        ps->p++;
        if (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
            hi = 0;
            while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
                hi = hi * 10 + (*ps->p++ - '0');
                if (hi > 255) { ps->err = REG_BADBR; return -1; }
            }
        }
    } else {
        hi = lo;
    }
    int closed = 0;
    if (ps->ere) {
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; closed = 1; }
    } else if (ps->p + 1 < ps->end && *ps->p == '\\' && ps->p[1] == '}') {
        ps->p += 2; closed = 1;
    }
    if (!closed) { ps->err = REG_EBRACE; return -1; }
    if (hi >= 0 && hi < lo) { ps->err = REG_BADBR; return -1; }

    int n = ps->prog->len - start;
    if (n <= 0) { ps->err = REG_BADRPT; return -1; }

    // Keep one pristine copy of the atom to duplicate from. It is at
    // [start, start+n) and the program is rebuilt after it.
    struct inst *atom = malloc((size_t)n * sizeof(struct inst));
    if (!atom) { ps->err = REG_ESPACE; return -1; }
    memcpy(atom, &ps->prog->code[start], (size_t)n * sizeof(struct inst));
    ps->prog->len = start;

    int rc = 0;
    for (int i = 0; i < lo && !rc; i++) {
        if (ps->prog->len + n > MAX_PROG) { ps->err = REG_ESPACE; rc = -1; break; }
        memcpy(&ps->prog->code[ps->prog->len], atom, (size_t)n * sizeof(struct inst));
        ps->prog->len += n;
    }
    if (!rc && hi < 0) {
        // {n,} -- one more copy under a star.
        int s = ps->prog->len;
        if (ps->prog->len + n > MAX_PROG) { ps->err = REG_ESPACE; rc = -1; }
        else {
            memcpy(&ps->prog->code[ps->prog->len], atom, (size_t)n * sizeof(struct inst));
            ps->prog->len += n;
            rc = apply_repeat(ps, s, '*');
        }
    } else if (!rc) {
        for (int i = lo; i < hi && !rc; i++) {
            int s = ps->prog->len;
            if (ps->prog->len + n > MAX_PROG) { ps->err = REG_ESPACE; rc = -1; break; }
            memcpy(&ps->prog->code[ps->prog->len], atom, (size_t)n * sizeof(struct inst));
            ps->prog->len += n;
            rc = apply_repeat(ps, s, '?');
        }
    }
    free(atom);
    (void)dup_range; // kept for readability of the relative-jump argument
    return rc;
}

static int parse_piece(struct parser *ps) {
    // A repetition operator with nothing to repeat. POSIX calls this
    // undefined in ERE; treating it as a literal (which is what falling
    // through to parse_atom does) means `*abc` silently searches for a
    // string containing an asterisk. Refuse instead -- a parser rejects
    // rather than guesses.
    //
    // BRE is the exception and it is not an inconsistency: there, `*`
    // at the start of an expression or right after `\(` IS a literal
    // by specification, so nothing is being guessed.
    if (ps->ere && ps->p < ps->end &&
        (*ps->p == '*' || *ps->p == '+' || *ps->p == '?')) {
        ps->err = REG_BADRPT;
        return -1;
    }
    int start = parse_atom(ps);
    if (start < 0) return -1;
    for (;;) {
        if (ps->p >= ps->end) return 0;
        char c = *ps->p;
        if (ps->ere && (c == '*' || c == '+' || c == '?')) {
            ps->p++;
            if (apply_repeat(ps, start, c) < 0) return -1;
            continue;
        }
        if (!ps->ere && c == '*') {
            ps->p++;
            if (apply_repeat(ps, start, '*') < 0) return -1;
            continue;
        }
        if (at_brace_open(ps)) {
            if (apply_interval(ps, start) < 0) return -1;
            continue;
        }
        return 0;
    }
}

static int parse_concat(struct parser *ps) {
    while (ps->p < ps->end && !at_alternation(ps) && !at_group_close(ps)) {
        if (parse_piece(ps) < 0) return -1;
    }
    return 0;
}

static int parse_alt(struct parser *ps) {
    int start = ps->prog->len;
    if (parse_concat(ps) < 0) return -1;
    while (at_alternation(ps)) {
        ps->p += ps->ere ? 1 : 2;
        int left = ps->prog->len - start;
        // SPLIT before the left arm, JMP after it, then the right arm.
        if (ps->prog->len + 2 > MAX_PROG) { ps->err = REG_ESPACE; return -1; }
        memmove(&ps->prog->code[start + 1], &ps->prog->code[start],
                (size_t)left * sizeof(struct inst));
        ps->prog->len++;
        struct inst *sp = &ps->prog->code[start];
        sp->op = I_SPLIT; sp->ch = 0; sp->cls = 0;
        sp->x = 1; sp->y = left + 2;
        int jat = emit(ps, I_JMP, 0, 0);
        if (jat < 0) return -1;
        int rstart = ps->prog->len;
        if (parse_concat(ps) < 0) return -1;
        ps->prog->code[jat].x = ps->prog->len - jat;
        (void)rstart;
    }
    return 0;
}

// ---- the VM ---------------------------------------------------------

struct thread { int pc; int caps[MAX_SLOTS]; };

struct tlist {
    struct thread *t;
    int n;
    int *seen;     // pc -> generation, for O(1) dedup
    int gen;
};

struct vm {
    const struct prog *prog;
    const char *s;
    size_t len;
    int eflags;
    int nslots;
    int *stack;    // explicit, so epsilon closure is not recursive
};

// Adds `pc` and everything reachable from it through epsilon
// transitions. ITERATIVE ON PURPOSE: the recursive form is the obvious
// one and its depth is the program length, against a 2 KiB ring-3 frame
// budget -- a 2000-instruction pattern would walk off the stack.
static void addthread(struct vm *vm, struct tlist *l, int pc, int *caps, size_t sp) {
    int top = 0;
    vm->stack[top++] = pc;
    int save_slot = -1, save_old = 0;
    (void)save_slot; (void)save_old;

    while (top > 0) {
        int cur = vm->stack[--top];
        if (cur < 0 || cur >= vm->prog->len) continue;
        if (l->seen[cur] == l->gen) continue;
        l->seen[cur] = l->gen;

        const struct inst *in = &vm->prog->code[cur];
        switch (in->op) {
        case I_JMP:
            vm->stack[top++] = cur + in->x;
            break;
        case I_SPLIT:
            // y pushed first so x is popped first -- x is the preferred
            // branch, and preference is what makes `a*` greedy.
            vm->stack[top++] = cur + in->y;
            vm->stack[top++] = cur + in->x;
            break;
        case I_SAVE: {
            if (in->x < vm->nslots) {
                int old = caps[in->x];
                caps[in->x] = (int)sp;
                // Follow through with the slot set, then restore -- the
                // caller's array is shared with the sibling branches.
                int save_top = top;
                vm->stack[top++] = cur + 1;
                while (top > save_top) {
                    int nxt = vm->stack[--top];
                    if (nxt < 0 || nxt >= vm->prog->len) continue;
                    if (l->seen[nxt] == l->gen) continue;
                    l->seen[nxt] = l->gen;
                    const struct inst *ni = &vm->prog->code[nxt];
                    if (ni->op == I_JMP) { vm->stack[top++] = nxt + ni->x; continue; }
                    if (ni->op == I_SPLIT) {
                        vm->stack[top++] = nxt + ni->y;
                        vm->stack[top++] = nxt + ni->x;
                        continue;
                    }
                    if (ni->op == I_SAVE) {
                        if (ni->x < vm->nslots) caps[ni->x] = (int)sp;
                        vm->stack[top++] = nxt + 1;
                        continue;
                    }
                    if (ni->op == I_BOL) {
                        int bol = (sp == 0 && !(vm->eflags & REG_NOTBOL)) ||
                                  ((vm->prog->cflags & REG_NEWLINE) && sp > 0 &&
                                   vm->s[sp - 1] == '\n');
                        if (bol) vm->stack[top++] = nxt + 1;
                        continue;
                    }
                    if (ni->op == I_EOL) {
                        int eol = (sp == vm->len && !(vm->eflags & REG_NOTEOL)) ||
                                  ((vm->prog->cflags & REG_NEWLINE) && sp < vm->len &&
                                   vm->s[sp] == '\n');
                        if (eol) vm->stack[top++] = nxt + 1;
                        continue;
                    }
                    struct thread *th = &l->t[l->n++];
                    th->pc = nxt;
                    memcpy(th->caps, caps, sizeof(int) * (size_t)vm->nslots);
                }
                caps[in->x] = old;
            } else {
                vm->stack[top++] = cur + 1;
            }
            break;
        }
        case I_BOL: {
            int bol = (sp == 0 && !(vm->eflags & REG_NOTBOL)) ||
                      ((vm->prog->cflags & REG_NEWLINE) && sp > 0 && vm->s[sp - 1] == '\n');
            if (bol) vm->stack[top++] = cur + 1;
            break;
        }
        case I_EOL: {
            int eol = (sp == vm->len && !(vm->eflags & REG_NOTEOL)) ||
                      ((vm->prog->cflags & REG_NEWLINE) && sp < vm->len && vm->s[sp] == '\n');
            if (eol) vm->stack[top++] = cur + 1;
            break;
        }
        default: {
            struct thread *th = &l->t[l->n++];
            th->pc = cur;
            memcpy(th->caps, caps, sizeof(int) * (size_t)vm->nslots);
            break;
        }
        }
    }
}

// ---- the API --------------------------------------------------------

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) return REG_BADPAT;
    preg->re_prog = NULL;
    preg->re_nsub = 0;

    struct prog *pr = calloc(1, sizeof *pr);
    if (!pr) return REG_ESPACE;
    pr->code = calloc(MAX_PROG, sizeof(struct inst));
    pr->classes = calloc(MAX_CLASS, 32);
    if (!pr->code || !pr->classes) {
        free(pr->code); free(pr->classes); free(pr);
        return REG_ESPACE;
    }
    pr->cflags = cflags;

    struct parser ps = {
        .p = pattern, .end = pattern + strlen(pattern), .prog = pr,
        .ngroups = 0, .err = 0,
        .ere = (cflags & REG_EXTENDED) != 0,
        .icase = (cflags & REG_ICASE) != 0,
    };

    // Slot 0/1 are the whole match.
    if (emit(&ps, I_SAVE, 0, 0) >= 0) pr->code[pr->len - 1].x = 0;
    if (!ps.err) parse_alt(&ps);
    if (!ps.err && ps.p < ps.end) {
        // Left over input means an unbalanced ')' -- parse_concat stops
        // at one and nothing above it consumed it.
        ps.err = REG_EPAREN;
    }
    if (!ps.err) {
        if (emit(&ps, I_SAVE, 0, 0) >= 0) pr->code[pr->len - 1].x = 1;
    }
    if (!ps.err) emit(&ps, I_MATCH, 0, 0);

    if (ps.err) {
        free(pr->code); free(pr->classes); free(pr);
        return ps.err;
    }

    pr->ngroups = ps.ngroups;
    preg->re_nsub = (size_t)ps.ngroups;
    preg->re_prog = pr;
    return 0;
}

void regfree(regex_t *preg) {
    if (!preg || !preg->re_prog) return;
    struct prog *pr = preg->re_prog;
    free(pr->code);
    free(pr->classes);
    free(pr);
    preg->re_prog = NULL;
    preg->re_nsub = 0;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags) {
    if (!preg || !preg->re_prog || !string) return REG_NOMATCH;
    const struct prog *pr = preg->re_prog;

    int nslots = 2 * (pr->ngroups + 1);
    if (nslots > MAX_SLOTS) nslots = MAX_SLOTS;

    size_t len = strlen(string);
    struct tlist a = {0}, b = {0};
    a.t = calloc((size_t)pr->len, sizeof(struct thread));
    b.t = calloc((size_t)pr->len, sizeof(struct thread));
    a.seen = calloc((size_t)pr->len, sizeof(int));
    b.seen = calloc((size_t)pr->len, sizeof(int));
    int *stack = calloc((size_t)pr->len * 2 + 8, sizeof(int));
    int caps[MAX_SLOTS];
    if (!a.t || !b.t || !a.seen || !b.seen || !stack) {
        free(a.t); free(b.t); free(a.seen); free(b.seen); free(stack);
        return REG_ESPACE;
    }

    struct vm vm = { .prog = pr, .s = string, .len = len,
                     .eflags = eflags, .nslots = nslots, .stack = stack };

    int matched = 0;
    int best[MAX_SLOTS];
    for (int i = 0; i < MAX_SLOTS; i++) best[i] = -1;

    struct tlist *clist = &a, *nlist = &b;
    clist->n = 0; clist->gen = 1;
    nlist->gen = 1;

    for (size_t sp = 0; ; sp++) {
        // A new start position is only worth trying while nothing has
        // matched yet. Once something has, every later start is further
        // right and cannot be leftmost -- which is what makes the
        // overall match LEFTMOST, and stopping here rather than at the
        // first MATCH is what keeps it LONGEST.
        if (!matched) {
            for (int i = 0; i < MAX_SLOTS; i++) caps[i] = -1;
            addthread(&vm, clist, 0, caps, sp);
        }
        if (clist->n == 0 && matched) break;
        if (clist->n == 0 && sp > len) break;

        nlist->n = 0;
        nlist->gen++;
        int ch = (sp < len) ? (unsigned char)string[sp] : -1;

        for (int i = 0; i < clist->n; i++) {
            struct thread *th = &clist->t[i];
            const struct inst *in = &pr->code[th->pc];
            switch (in->op) {
            case I_CHAR:
                if (ch >= 0 && (uint8_t)ch == in->ch)
                    addthread(&vm, nlist, th->pc + 1, th->caps, sp + 1);
                break;
            case I_CLASS:
                if (ch >= 0 && bit_get(pr->classes[in->cls], ch))
                    addthread(&vm, nlist, th->pc + 1, th->caps, sp + 1);
                break;
            case I_MATCH:
                // Leftmost-longest: a match starting earlier always
                // wins; at the same start, a longer one does.
                if (!matched || th->caps[0] < best[0] ||
                    (th->caps[0] == best[0] && th->caps[1] > best[1])) {
                    memcpy(best, th->caps, sizeof(int) * (size_t)nslots);
                    matched = 1;
                }
                break;
            default:
                break;
            }
        }

        struct tlist *tmp = clist; clist = nlist; nlist = tmp;
        if (sp >= len) break;
    }

    free(a.t); free(b.t); free(a.seen); free(b.seen); free(stack);

    if (!matched) return REG_NOMATCH;
    if (pmatch && nmatch > 0 && !(pr->cflags & REG_NOSUB)) {
        for (size_t i = 0; i < nmatch; i++) {
            int so = (2 * i < (size_t)nslots) ? best[2 * i] : -1;
            int eo = (2 * i + 1 < (size_t)nslots) ? best[2 * i + 1] : -1;
            pmatch[i].rm_so = so;
            pmatch[i].rm_eo = eo;
        }
    }
    return 0;
}

static const char *ERRTEXT[] = {
    [0]            = "success",
    [REG_NOMATCH]  = "no match",
    [REG_BADPAT]   = "invalid regular expression",
    [REG_ECOLLATE] = "invalid collating element",
    [REG_ECTYPE]   = "invalid character class name",
    [REG_EESCAPE]  = "trailing backslash",
    [REG_ESUBREG]  = "invalid back reference",
    [REG_EBRACK]   = "unmatched [",
    [REG_EPAREN]   = "unmatched ( or )",
    [REG_EBRACE]   = "unmatched { or }",
    [REG_BADBR]    = "invalid repetition count",
    [REG_ERANGE]   = "invalid range end",
    [REG_ESPACE]   = "regex too big",
    [REG_BADRPT]   = "repetition operator with nothing to repeat",
};

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *msg = "unknown error";
    if (errcode >= 0 && errcode < (int)(sizeof ERRTEXT / sizeof ERRTEXT[0]) && ERRTEXT[errcode])
        msg = ERRTEXT[errcode];
    size_t need = strlen(msg) + 1;
    if (errbuf && errbuf_size > 0) {
        size_t n = need <= errbuf_size ? need - 1 : errbuf_size - 1;
        memcpy(errbuf, msg, n);
        errbuf[n] = '\0';
    }
    return need;
}
