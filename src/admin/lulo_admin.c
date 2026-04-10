#define _GNU_SOURCE

#include "lulo_admin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int apply_tune_plan(FILE *fp)
{
    LuloAdminTunePlan plan;
    char err[256];

    lulo_admin_tune_plan_init(&plan);
    if (lulo_admin_tune_plan_read_stream(fp, &plan, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "failed to read apply plan");
        lulo_admin_tune_plan_free(&plan);
        return 1;
    }
    for (int i = 0; i < plan.count; i++) {
        if (lulo_admin_tune_write_value(plan.items[i].path, plan.items[i].value, err, sizeof(err)) < 0) {
            fprintf(stderr, "%s\n", err[0] ? err : "failed to apply tunable");
            lulo_admin_tune_plan_free(&plan);
            return 1;
        }
    }
    lulo_admin_tune_plan_free(&plan);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        fprintf(stderr,
                "Usage: lulo-admin apply-tune < plan\n"
                "Run via pkexec. Applies validated tunable writes under /proc/sys, /sys, and /sys/fs/cgroup.\n");
        return 0;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "lulo-admin must run as root via pkexec\n");
        return 126;
    }
    if (argc < 2 || strcmp(argv[1], "apply-tune") != 0) {
        fprintf(stderr, "usage: lulo-admin apply-tune\n");
        return 2;
    }
    return apply_tune_plan(stdin);
}
