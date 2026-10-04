// /bin/dash -- the entry point for the vendored Almquist shell.
//
// **A SHIM, because the program's main() is upstream's.**
// userland/ports/dash/src/main.c has the real one, and it is compiled
// with -Dmain=dash_main so that this file can be the ELF's entry
// without either copy being edited. The vendored tree stays byte for
// byte what upstream ships (see its README), and the rename happens in
// the build rather than in the source.
//
// Same shape as userland/gui/apps/doom.c, and for the same reason: a
// program target has to live in a USERLAND_PROGRAM_DIRS directory, and
// a port's sources do not.
#include <stdlib.h>

int dash_main(int argc, char **argv);

int main(int argc, char **argv) {
    // **LINE EDITING IS ON BY DEFAULT HERE, and that is a deliberate
    // divergence from upstream.** dash leaves `-E` clear and expects
    // `set -o emacs`, which is reasonable where a shell without an
    // editor is merely plain. It is not plain on this system: an arrow
    // key in a canonical read lands in the line (it once arrived as a
    // byte the terminal drew as a blank -- an arrow "typed spaces").
    // /bin/tosh has no such mode and neither should this.
    //
    // Injected as an ARGUMENT rather than poked into dash's optlist, so
    // it goes through the same parsing any user's `-E` would: `set +E`
    // turns it back off, and an explicit `-V` later on the line still
    // wins (upstream's own ksh hack clears one when the other is set).
    char **av = malloc((size_t)(argc + 2) * sizeof *av);
    if (!av) return dash_main(argc, argv);   // out of memory: upstream's default
    av[0] = argv[0];
    av[1] = (char *)"-E";
    for (int i = 1; i < argc; i++) av[i + 1] = argv[i];
    av[argc + 1] = 0;
    return dash_main(argc + 1, av);
}
