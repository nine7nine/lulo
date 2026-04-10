#ifndef LULOD_SYSTEMD_H
#define LULOD_SYSTEMD_H

#include "lulo_systemd.h"

int lulod_systemd_snapshot_gather(LuloSystemdSnapshot *snap, const LuloSystemdState *state);
int lulod_systemd_snapshot_refresh_active(LuloSystemdSnapshot *snap, const LuloSystemdState *state);

#endif
