#ifndef SCREENSAVER_CONFIG_H
#define SCREENSAVER_CONFIG_H

// The screensaver's two persisted settings. The compositor reads them
// (userland/wm/wm_idle.h) and does everything else -- see the .c for
// why the registry only validates and writes.

// Where the savers are. A program in here is a choice in System
// Settings, because the setting's choice list IS this directory.
#define SCREENSAVER_DIR "/bin/wm/savers"

// A saver's OPTIONS: what it declares, and what they are set to. Two
// directories rather than one because the halves have different
// lifetimes -- the descriptors ship with the programs and are
// read-only, the values are written by whoever changes them. The pair
// is lib/usaver.h's; nothing in the kernel reads either.
//
// THE DESCRIPTORS ARE NOT IN SCREENSAVER_DIR, and that is not tidiness:
// the setting's choice list IS that directory, so a `starfield.saver`
// sitting beside `starfield` would become a saver you could select and
// which would fail to start.
#define SCREENSAVER_DESC_DIR "/usr/wm/savers"
#define SCREENSAVER_CONF_DIR "/etc/savers"

// Nothing running, which is a real state rather than an absence: the
// timeout is what turns the feature on.
#define SCREENSAVER_DEFAULT "starfield"

// Minutes. ZERO MEANS NEVER, which is why there is no separate enable
// -- an "off" that disagreed with a timeout is a combination this
// cannot express. The ceiling is two hours, past which a machine that
// has been idle that long is not waiting for a screensaver.
#define SCREENSAVER_IDLE_DEFAULT 10
#define SCREENSAVER_IDLE_MAX 120

void screensaver_setting_register(void);

#endif
