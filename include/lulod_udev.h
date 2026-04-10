#ifndef LULOD_UDEV_H
#define LULOD_UDEV_H

#include "lulo_udev.h"

int lulod_udev_snapshot_gather(LuloUdevSnapshot *snap, const LuloUdevState *state);
int lulod_udev_snapshot_refresh_active(LuloUdevSnapshot *snap, const LuloUdevState *state);

#endif
