#ifndef ULIB_UARGS_H
#define ULIB_UARGS_H

// A /bin program's arguments from a DECLARED TABLE: one table is both
// what the parser accepts and what -h/--help prints, so the help cannot
// list an option the program does not take (clap's, argparse's and git's
// parse-options shape). The page is GNU coreutils' flat one: Usage, a
// summary, Commands, Options, notes, "Full manual: doc NAME".
//
// A bad argument is reported as GNU does -- "NAME: unknown option '--x'"
// then "Try 'NAME --help' for more information." on stderr -- and the
// program exits UARGS_USAGE.
//
// tools/check_docs.py reads the table: the page in docs/commands must
// carry the usage line and name every option and command.

#define UARGS_USAGE 2   // the exit status of a usage error

struct uargs_opt {
    const char *name;      // long name without "--", or NULL
    char shortc;           // 0 for none
    const char *arg;       // the value's placeholder ("CFG"); NULL for a flag
    const char *help;      // one line
    // A flag is set to the POSITION of its last occurrence (1-based), 0
    // when absent -- true or false for most callers, and "which came
    // last" for a pair like ls's -1 and -C.
    int *flag;
    const char **value;    // an option with `arg`: where its value goes
};

struct uargs_cmd {
    const char *name;      // NULL: `help` is a heading for the rows after it
    const char *args;      // the command's arguments ("ENTRY TITLE"), or NULL
    const char *help;      // one line
};

struct uargs_prog {
    const char *name;
    // What follows the name on the Usage line; each '\n' starts another
    // form, printed as GNU's "  or:  NAME ...".
    const char *usage;
    const char *summary;           // under Usage; may be several lines
    const struct uargs_opt *opts;  // ends with a zeroed entry; NULL for none
    const struct uargs_cmd *cmds;  // ditto; NULL for a program with no commands
    const char *notes;             // after Options, verbatim
    const char *(*more)(void);     // printed last, NULL for none (the pager's keys)
    // The first operand ENDS the options, so everything after it is the
    // operand's own: `strace ls -l` gives -l to ls. A program that runs
    // another program needs it -- GNU getopt's leading '+', env's and
    // sudo's shape.
    int first_operand_ends_options;
};

struct uargs {
    int argc;              // the positional arguments, in order --
    char **argv;           // with `cmds`, argv[0] is the command
    int status;            // the exit code when uargs_parse() returns 1
};

// Parse, set the table's flags and values, and collect the positionals
// in `argv` (rewritten in place). Returns 0 to carry on, or 1 when the
// program should return a->status at once: help was printed (0), or a
// usage error was reported (UARGS_USAGE).
//
// `--` ends the options. -h is --help unless an option claims 'h'.
// WITH `cmds`, SHORT OPTIONS STOP AT THE COMMAND: a command's arguments
// may begin with '-' (bootcfg's "-nokaslr"); long ones are read anywhere.
// A first positional that names no command is refused, with a "did you
// mean" when one is close.
int uargs_parse(struct uargs *a, const struct uargs_prog *p, int argc, char **argv);

// The help page, on stdout.
void uargs_help(const struct uargs_prog *p);

// "NAME: <message>" and the Try line on stderr; returns UARGS_USAGE.
int uargs_error(const struct uargs_prog *p, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
