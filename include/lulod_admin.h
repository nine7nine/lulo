#ifndef LULOD_ADMIN_H
#define LULOD_ADMIN_H

#include <stddef.h>

#include "lulo_admin.h"

int lulod_admin_apply_tune_plan(const LuloAdminTunePlan *plan, char *err, size_t errlen);

#endif
