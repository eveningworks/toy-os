#include "apps.h"
#include "kapi.h"
#include "shell.h"
#include "gui.h"

// To add a new app: write apps/foo.c with a `void foo_main(void)` entry
// point (and apps/foo.h declaring it, by convention), then add one line
// to this table. That's the whole integration -- nothing else in the
// kernel needs to know foo.c exists. See apps/README.md.
const struct app_info app_registry[] = {
    { "shell", "Command-line shell",     shell_main },
    { "gui",   "Graphical desktop demo", gui_main   },
    { "gui3",  "Graphical desktop (ring 3)", gui3_main },
};
const int app_registry_count = sizeof(app_registry) / sizeof(app_registry[0]);

int app_run(const char *name) {
    for (int i = 0; i < app_registry_count; i++) {
        if (k_strcmp(app_registry[i].name, name) == 0) {
            app_registry[i].entry();
            return 1;
        }
    }
    return 0;
}

void app_list(void (*cb)(const char *name, const char *description)) {
    for (int i = 0; i < app_registry_count; i++) {
        cb(app_registry[i].name, app_registry[i].description);
    }
}

void apps_start(void) {
    app_run("shell");
}
