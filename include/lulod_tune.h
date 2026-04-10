#ifndef LULOD_TUNE_H
#define LULOD_TUNE_H

#include <stddef.h>

#include "lulo_admin.h"
#include "lulo_tune.h"

int lulod_tune_snapshot_gather(LuloTuneSnapshot *snap, const LuloTuneState *state);
int lulod_tune_snapshot_refresh_active(LuloTuneSnapshot *snap, const LuloTuneState *state);
int lulod_tune_snapshot_save_current(const LuloTuneSnapshot *snap, const LuloTuneState *state, int preset);
int lulod_tune_snapshot_apply_current(const LuloTuneSnapshot *snap, const LuloTuneState *state,
                                      char *err, size_t errlen);

#endif
