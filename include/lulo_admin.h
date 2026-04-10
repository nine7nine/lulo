#ifndef LULO_ADMIN_H
#define LULO_ADMIN_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    char path[320];
    char value[192];
} LuloAdminTuneItem;

typedef struct {
    LuloAdminTuneItem *items;
    int count;
} LuloAdminTunePlan;

void lulo_admin_tune_plan_init(LuloAdminTunePlan *plan);
void lulo_admin_tune_plan_free(LuloAdminTunePlan *plan);
int lulo_admin_tune_plan_append(LuloAdminTunePlan *plan, const char *path, const char *value);
int lulo_admin_tune_plan_write_stream(FILE *fp, const LuloAdminTunePlan *plan,
                                      char *err, size_t errlen);
int lulo_admin_tune_plan_read_stream(FILE *fp, LuloAdminTunePlan *plan,
                                     char *err, size_t errlen);
int lulo_admin_tune_path_allowed(const char *path, char *resolved, size_t len,
                                 char *err, size_t errlen);
int lulo_admin_tune_write_value(const char *path, const char *value,
                                char *err, size_t errlen);

#endif
