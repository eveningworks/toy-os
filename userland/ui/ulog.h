#ifndef ULOG_H
#define ULOG_H

// Toolkit diagnostics: one line to the app's stderr, which the kernel
// routes to its log and to a QMP test's console (CLAUDE.md's "diagnostics
// go to stderr"). Every ring-3 app hand-rolled this -- a `logf_`/
// `log_line` wrapping sys_eprint, and four apps additionally re-rolled an
// int->decimal formatter only because they avoided vsnprintf. Centralised
// so a test's log grammar has ONE writer and an app declares none.
//
// TWO calls on purpose. `ulog()` is a pre-formatted line, and drags in
// nothing; `ulogf()` pulls vsnprintf. --gc-sections drops whichever an
// app does not use, so a lean app that only calls ulog() never links the
// formatter -- which is why the split matters here rather than being one
// varargs call.
//
// THE CALLER ENDS THE LINE WITH '\n'. Neither call adds one, and a line
// without it is glued to whatever the log prints next -- so a test
// waiting for it never sees it.
void ulog(const char *s);
void ulogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
