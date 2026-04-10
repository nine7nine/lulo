#ifndef LULOD_FOCUS_H
#define LULOD_FOCUS_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int enabled;
    int fd;
    pid_t child_pid;
    long long restart_due_ms;
    int last_pid;
    unsigned long long last_start_time;
    size_t line_len;
    char provider[32];
    char linebuf[256];
} LulodFocusMonitor;

void lulod_focus_init(LulodFocusMonitor *monitor);
void lulod_focus_stop(LulodFocusMonitor *monitor);
int lulod_focus_poll_fd(LulodFocusMonitor *monitor, long long now_ms);
int lulod_focus_handle(LulodFocusMonitor *monitor, short revents, long long now_ms,
                       pid_t *pid, unsigned long long *start_time, const char **provider,
                       char *err, size_t errlen);

#endif
