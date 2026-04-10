#ifndef LULO_PROC_H
#define LULO_PROC_H

#define LULO_PROC_USER_MAX 16
#define LULO_PROC_LABEL_MAX 256

typedef enum {
    LULO_PROC_SORT_PID = 0,
    LULO_PROC_SORT_USER,
    LULO_PROC_SORT_POLICY,
    LULO_PROC_SORT_PRIORITY,
    LULO_PROC_SORT_NICE,
    LULO_PROC_SORT_CPU,
    LULO_PROC_SORT_MEM,
    LULO_PROC_SORT_TIME,
    LULO_PROC_SORT_IO,
    LULO_PROC_SORT_COMMAND,
    LULO_PROC_SORT_COUNT
} LuloProcSortKey;

typedef enum {
    LULO_PROC_CPU_PER_CORE = 0,
    LULO_PROC_CPU_TOTAL,
    LULO_PROC_CPU_MODE_COUNT
} LuloProcCpuMode;

typedef struct {
    int pid;
    int tgid;
    int parent_pid;
    int is_thread;
    int is_kernel;
    int depth;
    int has_children;
    int expanded;
    char user[LULO_PROC_USER_MAX];
    char state;
    int policy;
    int rt_priority;
    int priority;
    int nice;
    int cpu_tenths;
    int mem_tenths;
    unsigned long long time_cs;
    int io_class;
    int io_priority;
    int label_prefix_len;
    int label_prefix_cols;
    char label[LULO_PROC_LABEL_MAX];
} LuloProcRow;

typedef struct {
    LuloProcRow *rows;
    int count;
} LuloProcSnapshot;

struct LuloProcTimeSample;
struct LuloProcCollapsedKey;
typedef struct {
    struct LuloProcTimeSample *times;
    int time_count;
    int time_cap;
    struct LuloProcCollapsedKey *collapsed;
    int collapsed_count;
    int collapsed_cap;
    int selected;
    int scroll;
    int selected_pid;
    int selected_is_thread;
    int parents_only;
    int x_scroll;
    LuloProcSortKey sort_key;
    int sort_desc;
} LuloProcState;

void lulo_proc_state_init(LuloProcState *state);
void lulo_proc_state_cleanup(LuloProcState *state);

int lulo_proc_snapshot_gather(LuloProcSnapshot *snap, LuloProcState *state,
                              unsigned long long cpu_total_delta,
                              unsigned long long mem_total,
                              int logical_cpus,
                              LuloProcCpuMode cpu_mode);
void lulo_proc_snapshot_free(LuloProcSnapshot *snap);

void lulo_proc_view_sync(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows);
void lulo_proc_view_move(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows, int delta);
void lulo_proc_view_page(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows, int pages);
void lulo_proc_view_home(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows);
void lulo_proc_view_end(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows);

void lulo_proc_sort_toggle(LuloProcState *state, LuloProcSortKey key);
int lulo_proc_toggle_row(LuloProcState *state, const LuloProcSnapshot *snap, int index);
void lulo_proc_expand_all(LuloProcState *state);
int lulo_proc_collapse_all(LuloProcState *state, const LuloProcSnapshot *snap);
LuloProcCpuMode lulo_next_proc_cpu_mode(LuloProcCpuMode mode);
const char *lulo_proc_cpu_mode_name(LuloProcCpuMode mode);

#endif
