#ifndef WEEK_START_CONFIG_H
#define WEEK_START_CONFIG_H

// Registers `desktop.week_start` (monday | sunday). Persist-only: the
// calendar popup that reads it is drawn by a ring-3 process. See the .c
// file.
void week_start_setting_register(void);

#endif
