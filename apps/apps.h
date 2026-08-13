#ifndef APPS_H
#define APPS_H

// An "app" in toy-os is a self-contained program: one C file, one entry
// point, registered here. There's no process isolation yet (everything
// still runs in kernel space, one address space) -- this is a *source*
// boundary, not a security one. It exists so:
//   - apps only ever depend on kernel/include/api/kapi.h, never on driver
//     internals directly, so drivers can be refactored without touching
//     app code
//   - adding a new app is "write apps/foo.c + foo_main(), add one line to
//     apps/apps.c" -- nothing else needs to change
//
// See apps/README.md for the full walkthrough.

typedef void (*app_entry_fn)(void);

struct app_info {
    const char *name;        // what the shell's `run <name>` / built-in
                              // commands use to launch it
    const char *description; // shown by the shell's `apps` command
    app_entry_fn entry;      // called with no arguments; returns when done
};

extern const struct app_info app_registry[];
extern const int app_registry_count;

// Looks up an app by name and runs it. Returns 1 if found (and now
// finished running), 0 if no app with that name is registered.
int app_run(const char *name);

// Calls `cb` once per registered app -- used by the shell's `apps` command.
void app_list(void (*cb)(const char *name, const char *description));

// The very first app the kernel hands control to after hardware bring-up.
void apps_start(void);

#endif
