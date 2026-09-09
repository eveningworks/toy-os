#ifndef TARGET_H
#define TARGET_H

// The BOOT TARGET: what this machine is for, and therefore what init
// starts (docs/init-design.md's R7a, stage 2).
//
// Two values today -- `text` and `graphical` -- persisted as
// /etc/toyos.conf's `default_target` key and reachable from ring 3 as
// the registered setting `system.default_target`. init reads it through
// SYS_SETTING and starts the services in /etc/services.d whose
// `Target=` matches; nothing in the kernel acts on it.
//
// This is systemd's `default.target` with the parts that need a unit
// dependency graph left out: there are two targets, they are not
// ordered, and one does not "pull in" the other. What is copied is the
// shape that matters -- the machine's PURPOSE is a persisted setting a
// boot flag can override, not something a shell command decides.
//
// THE OVERRIDE IS A LIVE VALUE, NOT A SECOND SOURCE OF TRUTH.
// `target=text` on the GRUB line changes what target_get() answers and
// does NOT write the file, so a desktop that faults on boot never costs
// you the machine and never silently rewrites your configuration
// either. The registry already models exactly this: `config diff` shows
// the file's value against the live one, so an overridden boot is
// visible rather than mysterious. The cost, stated because it is real:
// `config reload` re-reads the file and therefore DISCARDS the
// override -- an explicit user action, and the one place the two can
// diverge.

// The valid targets, in registry-choice order.
#define TARGET_TEXT      "text"
#define TARGET_GRAPHICAL "graphical"

// RECOVERY. Starts NO services at all, so the KERNEL's own shell owns
// the console -- which is the point: it is the shell that works when the
// filesystem is too broken for /bin/tosh to load, and it carries the
// kernel introspection commands nothing in ring 3 has. systemd's
// rescue.target, and reached the same way: `target=rescue` on the GRUB
// line, or `config set system.default_target rescue`.
#define TARGET_RESCUE    "rescue"

// Called from kernel_main() after fs_init(), beside tz_init() and the
// other /etc readers -- and BEFORE the init process is spawned, since
// init asks for this value as its first act. Loads `default_target`
// from /etc/toyos.conf (defaulting to `graphical`), then applies a
// `target=<name>` word from the kernel command line over the top
// without persisting it.
void target_init(void);

// The effective target -- the override if there was one, else the file's
// value, else the default. Never NULL, always one of the names above.
const char *target_get(void);

// Whether the command line overrode the persisted value this boot.
// Reported by `dmesg`, so a machine that came up in the "wrong" target
// says why.
int target_overridden(void);

// Announces this setting to the registry (setting.h). Called from
// settings_init().
void target_setting_register(void);

#endif
