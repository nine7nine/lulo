#ifndef LULO_PROC_META_H
#define LULO_PROC_META_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    pid_t pid;
    unsigned long long start_time;
    char comm[96];
    char exe[192];
    char cmdline[512];
    char cgroup[320];
    char unit[128];
    char slice[128];
} LuloProcMeta;

int lulo_proc_meta_collect(pid_t pid, LuloProcMeta *meta);
int lulo_proc_meta_read_basic(pid_t pid, char *comm, size_t comm_len,
                              unsigned long long *start_time);
int lulo_proc_meta_read_exe(pid_t pid, char *buf, size_t len);
int lulo_proc_meta_read_cmdline(pid_t pid, char *buf, size_t len);
int lulo_proc_meta_read_cgroup(pid_t pid, char *buf, size_t len);
void lulo_proc_meta_derive_unit(const char *cgroup_path, char *buf, size_t len);
void lulo_proc_meta_derive_slice(const char *cgroup_path, char *buf, size_t len);

#endif
