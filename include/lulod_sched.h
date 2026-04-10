#ifndef LULOD_SCHED_H
#define LULOD_SCHED_H

#include <stddef.h>
#include <sys/types.h>

#include "lulo_sched.h"

int lulod_sched_snapshot_gather(LuloSchedSnapshot *snap, int reload,
                                char *err, size_t errlen);
int lulod_sched_focus_update(pid_t pid, unsigned long long start_time,
                             const char *provider, char *err, size_t errlen);
int lulod_sched_apply_tunable_preset(const char *preset_id, LuloSchedSnapshot *snap,
                                     char *err, size_t errlen);

#endif
