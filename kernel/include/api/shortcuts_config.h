#ifndef API_SHORTCUTS_CONFIG_H
#define API_SHORTCUTS_CONFIG_H

// The desktop's global keyboard shortcuts. See shortcuts_config.c for
// why they are registry settings and why the compositor owns matching.
//
// The ACTION TABLE is readable from ring 3 as well: the compositor needs
// each action's command, and it must be the same table the settings were
// registered from, or a binding could name a program nothing launches.

struct shortcut_action {
    const char *name;      // the /etc key, and the setting's name
    const char *label;     // what System Settings calls it
    const char *desc;      // the sentence under that label
    const char *command;   // what the compositor spawns
    const char *fallback;  // the factory binding, when /etc says nothing
};

int shortcut_action_count(void);
const struct shortcut_action *shortcut_action_at(int i);

// Is `value` a legal binding -- every comma-separated combination
// parses? An EMPTY value is legal and means unbound.
int shortcut_value_valid(const char *value);

void shortcuts_setting_register(void);

#endif
