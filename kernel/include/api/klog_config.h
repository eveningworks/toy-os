#ifndef KLOG_CONFIG_H
#define KLOG_CONFIG_H

// `system.loglevel` -- what the kernel log still puts on the console.
// See klog_config.c; the mechanism is api/klog.h.
void klog_config_init(void);
void klog_config_setting_register(void);

#endif
