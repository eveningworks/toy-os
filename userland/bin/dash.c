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
int dash_main(int argc, char **argv);

int main(int argc, char **argv) {
    return dash_main(argc, argv);
}
