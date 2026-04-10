#ifndef LULOD_SYSTEM_SCHED_H
#define LULOD_SYSTEM_SCHED_H

#include <stddef.h>
#include <sys/types.h>

#include "lulo_sched.h"

int lulod_system_sched_default_config_root(char *buf, size_t len);
int lulod_system_sched_reload(LuloSchedSnapshot *snap, const char *config_root,
                              char *err, size_t errlen);
int lulod_system_sched_set_focus(LuloSchedSnapshot *snap, pid_t pid, unsigned long long start_time,
                                 const char *provider);
int lulod_system_sched_scan(LuloSchedSnapshot *snap, char *err, size_t errlen);

#endif
