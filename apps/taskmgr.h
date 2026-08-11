#ifndef TASKMGR_H
#define TASKMGR_H

struct window;

// Task Manager: a read-only view of what's running -- open windows
// (from apps/wm/wm.h's wm_window_count()/wm_get_window()) and system
// memory (physical frames via pmm_*, kernel heap via heap_*, both
// already exposed through kapi.h). No CPU column yet -- GUI apps
// aren't scheduled processes in this kernel, so there's no real
// per-app CPU accounting to show (see docs/decisions.md). Redraws
// every tick like the clock in the taskbar, so the numbers stay live
// without needing a manual refresh.
void taskmgr_default_size(int *w, int *h);
void taskmgr_open(struct window *win);
void taskmgr_draw(struct window *win);

#endif
