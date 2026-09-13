#ifndef SHELL_CONFIG_H
#define SHELL_CONFIG_H

// system.shell -- which shell an interactive session starts. See
// kernel/lib/shell_config.c, including why the CONSOLE shell is
// deliberately not this setting.
#include <stdint.h>

// Writes the configured shell's path, or "/bin/tosh" when nothing is
// set. Never fails: a caller always gets a path it can spawn.
void shell_setting_get(char *out, uint32_t cap);

void shell_setting_register(void);

#endif
