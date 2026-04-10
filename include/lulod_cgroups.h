#ifndef LULOD_CGROUPS_H
#define LULOD_CGROUPS_H

#include "lulo_cgroups.h"

int lulod_cgroups_snapshot_gather(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state);
int lulod_cgroups_snapshot_refresh_active(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state);

#endif
