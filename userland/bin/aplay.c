// aplay -- play an audio file, or say what one contains.
//
// The CLI half of lib/usnd.h, and its smallest complete caller: open
// the sink, hand it a path, wait, drain. Everything about formats,
// resampling and mixing is the library's, which is what makes this and
// the GUI player the same program plus a window.
#include "lib/usnd.h"
#include "lib/cmd.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define USAGE "aplay [-i] [-v pct] <file>"

int main(int argc, char **argv) {
    int info_only = 0, volume = -1;
    const char *path = 0;

    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "-i") == 0) info_only = 1;
        else if (strcmp(argv[a], "-v") == 0 && a + 1 < argc) volume = atoi(argv[++a]);
        else if (argv[a][0] == '-' || path) { cmd_usage(USAGE); return 1; }
        else path = argv[a];
    }
    if (!path) { cmd_usage(USAGE); return 1; }

    // The header is read whether or not we play: "what is this file"
    // is the question -i asks, and printing it first means a refusal
    // says which file and why rather than just failing to make noise.
    struct usnd_info in;
    if (usnd_load_info(path, &in) != 0) {
        fprintf(stderr, "aplay: %s: %s\n", path, usnd_last_error());
        return 1;
    }
    printf("%s: %s, %s", path, in.format, in.detail);
    if (in.ms) printf(", %u.%03u s", in.ms / 1000, in.ms % 1000);
    printf("\n");
    if (info_only) return 0;

    if (usnd_init() != 0) {
        fprintf(stderr, "aplay: %s\n", usnd_last_error());
        return 1;
    }
    if (volume >= 0) usnd_set_volume(volume);

    if (usnd_play(path) != 0) {
        fprintf(stderr, "aplay: %s: %s\n", path, usnd_last_error());
        usnd_shutdown();
        return 1;
    }

    // Polled rather than blocked on a completion: there is no such call
    // here, and the thing worth waiting on is the position anyway.
    while (usnd_playing()) sys_sleep_ms(50);
    usnd_drain();
    usnd_shutdown();
    return 0;
}
