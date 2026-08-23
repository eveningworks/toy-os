#ifndef WALLPAPER_CONFIG_H
#define WALLPAPER_CONFIG_H

// The desktop WALLPAPER and its placement MODE, as registry descriptors.
// See kernel/lib/wallpaper_config.c -- the same split cursor themes make:
// the kernel owns the DESCRIPTION of the setting (name, label, legal
// values, which file), the ring-3 desktop owns the behaviour, and the
// descriptors are persist-only because a ring-3 process cannot supply
// the `apply` function pointer the registry would call.
void wallpaper_setting_register(void);

#endif
