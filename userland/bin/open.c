// open -- start the program that Handles= a file's type, from the
// shell. The resolver is lib/uopen.h (the File Manager's double-click
// goes through the same one); this program adds the management verbs
// for the user's override file, which is what makes /etc/mimeapps.conf
// editable without an editor.
#include "lib/uopen.h"
#include "lib/uconf.h"
#include "lib/cmd.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        // The raw file IS the listing: one ".ext=entry" per line.
        FILE *f = fopen(UOPEN_CONF, "r");
        if (!f) { printf("no overrides (%s)\n", UOPEN_CONF); return 0; }
        char line[96];
        while (fgets(line, sizeof line, f)) fputs(line, stdout);
        fclose(f);
        return 0;
    }

    if (argc == 4 && strcmp(argv[1], "-s") == 0) {
        char ext[16];
        snprintf(ext, sizeof ext, "%s", argv[2]);
        lower(ext);
        if (ext[0] != '.' || !ext[1]) {
            fprintf(stderr, "open: '%s' is not an extension (need '.ext')\n",
                    argv[2]);
            return 1;
        }
        // `-` clears: the resolver treats it as absent, so the
        // declaration takes over again. Written rather than deleted
        // because the config rewriter has no remove verb.
        if (!uconf_set(UOPEN_CONF, ext, argv[3])) {
            cmd_fail("open", UOPEN_CONF);
            return 1;
        }
        return 0;
    }

    if (argc != 2 || argv[1][0] == '-') {
        cmd_usage("open <path> | open -l | open -s .ext <entry|/path|->");
        return 1;
    }

    if (uopen_spawn(argv[1]) < 0) {
        fprintf(stderr, "open: nothing opens %s\n", argv[1]);
        return 1;
    }
    return 0; // spawned, not waited for -- see lib/uopen.h
}
