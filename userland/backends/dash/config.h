/* dash's config.h, hand-written for toy-os.
 *
 * **THIS IS THE PORT, ALMOST IN ITS ENTIRETY.** dash configures itself
 * through autoconf, and every probe it runs lands here as one #define --
 * so porting it is mostly deciding what to answer, not writing code.
 * An ABSENT HAVE_* is not an oversight: it is how dash is told to take
 * its own fallback, which is why most of what the roadmap once listed
 * as requirements turned out not to be.
 *
 * OURS, not upstream's, which is why it lives in userland/backends/
 * rather than in the vendored tree -- the same boundary the Doom port
 * draws. tools/dash_gap.py reads THIS file, so the measurement and the
 * build cannot disagree about what was answered.
 */
#define HAVE_ALIAS_ATTRIBUTE 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_DECL_ISBLANK 1
#define _PATH_BSHELL "/bin/sh"
#define _PATH_DEVNULL "/dev/null"
#define _PATH_TTY "/dev/tty"
#define SIZEOF_INTMAX_T 8
#define SIZEOF_LONG_LONG_INT 8
#define PRIdMAX "lld"
#define USE_TEE 0
#define USE_MEMFD_CREATE 0
#define HAVE_F_DUPFD_CLOEXEC 0
#define SMALL 1
#define WITH_LINENO 1
/* tolibc has these, so dash must NOT declare its own and collide. */
#define HAVE_STRSIGNAL 1
#define HAVE_STRTOD 1
#define HAVE_BSEARCH 1
#define HAVE_STRTOIMAX 1
#define HAVE_STRTOUMAX 1
#define HAVE_ISALPHA 1
// dash's waitpid fallback is written with FOUR arguments and has
// evidently never been compiled; wait3 is the path that works.
#define HAVE_WAIT3 1
/* dash reaches for the 64-bit names; map them onto the plain ones. */
#define stat64 stat
#define fstat64 fstat
#define lstat64 lstat
#define open64 open
#define readdir64 readdir
#define dirent64 dirent
