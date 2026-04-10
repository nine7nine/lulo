#include "lulo_proc.h"

#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int pid;
    int parent_pid;
    int tgid;
    int is_thread;
    int is_kernel;
    char user[LULO_PROC_USER_MAX];
    char state;
    int policy;
    int rt_priority;
    int priority;
    int nice;
    unsigned long long total_ticks;
    unsigned long long time_cs;
    int cpu_tenths;
    int mem_tenths;
    char command[LULO_PROC_LABEL_MAX];
    int parent_index;
    int first_child;
    int last_child;
    int next_sibling;
} ProcNode;

typedef struct {
    ProcNode *items;
    int count;
    int cap;
} ProcNodeList;

struct LuloProcTimeSample {
    int pid;
    int is_thread;
    unsigned long long total_ticks;
};

struct LuloProcCollapsedKey {
    int pid;
    int is_thread;
};

typedef struct {
    LuloProcRow *items;
    int count;
    int cap;
} ProcRowList;

typedef struct {
    int pid;
    int index;
} LeaderRef;

typedef struct {
    uid_t uid;
    char name[LULO_PROC_USER_MAX];
} UidNameCacheEntry;

static const ProcNodeList *proc_sort_nodes;
static const LuloProcState *proc_sort_state;
static UidNameCacheEntry uid_cache[16];
static int uid_cache_count;
static int uid_cache_next;

static int proc_key_cmp(int pid_a, int is_thread_a, int pid_b, int is_thread_b)
{
    if (pid_a != pid_b) return pid_a < pid_b ? -1 : 1;
    if (is_thread_a != is_thread_b) return is_thread_a < is_thread_b ? -1 : 1;
    return 0;
}

static int prev_time_cmp(const void *a, const void *b)
{
    const struct LuloProcTimeSample *pa = a;
    const struct LuloProcTimeSample *pb = b;
    return proc_key_cmp(pa->pid, pa->is_thread, pb->pid, pb->is_thread);
}

static int leader_ref_cmp(const void *a, const void *b)
{
    const LeaderRef *pa = a;
    const LeaderRef *pb = b;
    if (pa->pid != pb->pid) return pa->pid < pb->pid ? -1 : 1;
    return 0;
}

static int proc_node_cmp(const void *a, const void *b)
{
    const ProcNode *pa = a;
    const ProcNode *pb = b;
    return proc_key_cmp(pa->pid, pa->is_thread, pb->pid, pb->is_thread);
}

static int collapsed_key_cmp(const void *a, const void *b)
{
    const struct LuloProcCollapsedKey *pa = a;
    const struct LuloProcCollapsedKey *pb = b;
    return proc_key_cmp(pa->pid, pa->is_thread, pb->pid, pb->is_thread);
}

static int proc_prio_value(const ProcNode *node)
{
    return node->rt_priority > 0 ? node->rt_priority : node->priority;
}

static int proc_field_cmp(const ProcNode *pa, const ProcNode *pb, LuloProcSortKey key)
{
    switch (key) {
    case LULO_PROC_SORT_PID:
        if (pa->pid != pb->pid) return pa->pid < pb->pid ? -1 : 1;
        if (pa->is_thread != pb->is_thread) return pa->is_thread < pb->is_thread ? -1 : 1;
        return 0;
    case LULO_PROC_SORT_USER:
        return strcmp(pa->user, pb->user);
    case LULO_PROC_SORT_POLICY:
        if (pa->policy != pb->policy) return pa->policy < pb->policy ? -1 : 1;
        return 0;
    case LULO_PROC_SORT_PRIORITY: {
        int a = proc_prio_value(pa);
        int b = proc_prio_value(pb);
        if (a != b) return a < b ? -1 : 1;
        return 0;
    }
    case LULO_PROC_SORT_NICE:
        if (pa->nice != pb->nice) return pa->nice < pb->nice ? -1 : 1;
        return 0;
    case LULO_PROC_SORT_CPU:
        if (pa->cpu_tenths != pb->cpu_tenths) return pa->cpu_tenths < pb->cpu_tenths ? -1 : 1;
        return 0;
    case LULO_PROC_SORT_MEM:
        if (pa->mem_tenths != pb->mem_tenths) return pa->mem_tenths < pb->mem_tenths ? -1 : 1;
        return 0;
    case LULO_PROC_SORT_TIME:
        if (pa->time_cs != pb->time_cs) return pa->time_cs < pb->time_cs ? -1 : 1;
        return 0;
    case LULO_PROC_SORT_COMMAND:
        return strcmp(pa->command, pb->command);
    case LULO_PROC_SORT_COUNT:
    default:
        return 0;
    }
}

static int proc_tree_order_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const ProcNode *pa = &proc_sort_nodes->items[ia];
    const ProcNode *pb = &proc_sort_nodes->items[ib];
    int cmp;

    if (proc_sort_state) {
        cmp = proc_field_cmp(pa, pb, proc_sort_state->sort_key);
        if (cmp != 0) return proc_sort_state->sort_desc ? -cmp : cmp;
    }

    cmp = proc_field_cmp(pa, pb, LULO_PROC_SORT_CPU);
    if (cmp != 0) return -cmp;
    cmp = proc_field_cmp(pa, pb, LULO_PROC_SORT_MEM);
    if (cmp != 0) return -cmp;
    cmp = proc_field_cmp(pa, pb, LULO_PROC_SORT_TIME);
    if (cmp != 0) return -cmp;
    if (pa->is_thread != pb->is_thread) return pa->is_thread < pb->is_thread ? -1 : 1;
    if (pa->is_kernel != pb->is_kernel) return pa->is_kernel < pb->is_kernel ? -1 : 1;
    return proc_field_cmp(pa, pb, LULO_PROC_SORT_PID);
}

static int is_numeric_name(const char *s)
{
    if (!s || !*s) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

static int read_text_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    char *end = buf + strlen(buf);
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return 0;
}

static int read_cmdline_joined(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    size_t nread;
    if (!f) return -1;
    nread = fread(buf, 1, len - 1, f);
    fclose(f);
    if (nread == 0) {
        if (len > 0) buf[0] = '\0';
        return -1;
    }
    buf[nread] = '\0';
    for (size_t i = 0; i < nread; i++) {
        if (buf[i] == '\0') buf[i] = ' ';
    }
    while (nread > 0 && buf[nread - 1] == ' ') buf[--nread] = '\0';
    return nread > 0 ? 0 : -1;
}

static void uid_to_name(uid_t uid, char *buf, size_t len)
{
    for (int i = 0; i < uid_cache_count; i++) {
        if (uid_cache[i].uid == uid) {
            snprintf(buf, len, "%s", uid_cache[i].name);
            return;
        }
    }

    {
        struct passwd *pw = getpwuid(uid);
        char resolved[LULO_PROC_USER_MAX];

        if (pw && pw->pw_name && *pw->pw_name) snprintf(resolved, sizeof(resolved), "%s", pw->pw_name);
        else snprintf(resolved, sizeof(resolved), "%u", (unsigned)uid);
        snprintf(buf, len, "%s", resolved);

        if (uid_cache_count < (int)(sizeof(uid_cache) / sizeof(uid_cache[0]))) {
            uid_cache[uid_cache_count].uid = uid;
            snprintf(uid_cache[uid_cache_count].name, sizeof(uid_cache[uid_cache_count].name), "%s", resolved);
            uid_cache_count++;
        } else {
            uid_cache[uid_cache_next].uid = uid;
            snprintf(uid_cache[uid_cache_next].name, sizeof(uid_cache[uid_cache_next].name), "%s", resolved);
            uid_cache_next = (uid_cache_next + 1) % (int)(sizeof(uid_cache) / sizeof(uid_cache[0]));
        }
    }
}

static int parse_proc_stat_line(const char *line, char *comm, size_t comm_len,
                                char *state, int *ppid,
                                int *num_threads,
                                int *policy, int *rt_priority,
                                int *priority, int *nice_value,
                                unsigned long long *total_ticks,
                                long *rss_pages)
{
    const char *open = strchr(line, '(');
    const char *close = strrchr(line, ')');
    char tail[2048];
    char *saveptr = NULL;
    char *tok;
    int field = 0;
    unsigned long long utime = 0, stime = 0;

    if (!open || !close || close <= open) return -1;
    *policy = 0;
    *rt_priority = 0;
    *priority = 0;
    *nice_value = 0;
    *num_threads = 0;
    {
        size_t n = (size_t)(close - open - 1);
        if (n >= comm_len) n = comm_len - 1;
        memcpy(comm, open + 1, n);
        comm[n] = '\0';
    }
    snprintf(tail, sizeof(tail), "%s", close + 2);
    tok = strtok_r(tail, " ", &saveptr);
    while (tok) {
        switch (field) {
        case 0:
            *state = tok[0];
            break;
        case 1:
            *ppid = atoi(tok);
            break;
        case 11:
            utime = strtoull(tok, NULL, 10);
            break;
        case 12:
            stime = strtoull(tok, NULL, 10);
            break;
        case 15:
            *priority = atoi(tok);
            break;
        case 16:
            *nice_value = atoi(tok);
            break;
        case 17:
            *num_threads = atoi(tok);
            break;
        case 37:
            *rt_priority = atoi(tok);
            break;
        case 38:
            *policy = atoi(tok);
            break;
        case 21:
            *rss_pages = strtol(tok, NULL, 10);
            break;
        }
        field++;
        tok = strtok_r(NULL, " ", &saveptr);
    }
    if (field <= 21) return -1;
    *total_ticks = utime + stime;
    return 0;
}

static int read_proc_stat(const char *path, char *comm, size_t comm_len,
                          char *state, int *ppid,
                          int *num_threads,
                          int *policy, int *rt_priority,
                          int *priority, int *nice_value,
                          unsigned long long *total_ticks,
                          long *rss_pages)
{
    char line[4096];
    if (read_text_file(path, line, sizeof(line)) < 0) return -1;
    return parse_proc_stat_line(line, comm, comm_len, state, ppid, num_threads, policy, rt_priority,
                                priority, nice_value,
                                total_ticks, rss_pages);
}

static int ensure_node_cap(ProcNodeList *list, int extra)
{
    ProcNode *items;
    int new_cap;
    if (list->count + extra <= list->cap) return 0;
    new_cap = list->cap ? list->cap * 2 : 256;
    while (new_cap < list->count + extra) new_cap *= 2;
    items = realloc(list->items, (size_t)new_cap * sizeof(*items));
    if (!items) return -1;
    list->items = items;
    list->cap = new_cap;
    return 0;
}

static int ensure_row_cap(ProcRowList *list, int extra)
{
    LuloProcRow *items;
    int new_cap;
    if (list->count + extra <= list->cap) return 0;
    new_cap = list->cap ? list->cap * 2 : 256;
    while (new_cap < list->count + extra) new_cap *= 2;
    items = realloc(list->items, (size_t)new_cap * sizeof(*items));
    if (!items) return -1;
    list->items = items;
    list->cap = new_cap;
    return 0;
}

static int ensure_collapsed_cap(LuloProcState *state, int extra)
{
    struct LuloProcCollapsedKey *items;
    int new_cap;

    if (state->collapsed_count + extra <= state->collapsed_cap) return 0;
    new_cap = state->collapsed_cap ? state->collapsed_cap * 2 : 64;
    while (new_cap < state->collapsed_count + extra) new_cap *= 2;
    items = realloc(state->collapsed, (size_t)new_cap * sizeof(*items));
    if (!items) return -1;
    state->collapsed = items;
    state->collapsed_cap = new_cap;
    return 0;
}

static int collapsed_index_of(const LuloProcState *state, int pid, int is_thread)
{
    struct LuloProcCollapsedKey key;
    struct LuloProcCollapsedKey *found;

    if (!state->collapsed || state->collapsed_count <= 0) return -1;
    key.pid = pid;
    key.is_thread = is_thread;
    found = bsearch(&key, state->collapsed, (size_t)state->collapsed_count,
                    sizeof(*state->collapsed), collapsed_key_cmp);
    if (!found) return -1;
    return (int)(found - state->collapsed);
}

static int proc_is_collapsed(const LuloProcState *state, int pid, int is_thread)
{
    return collapsed_index_of(state, pid, is_thread) >= 0;
}

static int proc_set_collapsed(LuloProcState *state, int pid, int is_thread, int collapsed)
{
    int idx = collapsed_index_of(state, pid, is_thread);

    if (collapsed) {
        struct LuloProcCollapsedKey key;

        if (idx >= 0) return 0;
        if (ensure_collapsed_cap(state, 1) < 0) return -1;
        key.pid = pid;
        key.is_thread = is_thread;
        state->collapsed[state->collapsed_count++] = key;
        qsort(state->collapsed, (size_t)state->collapsed_count,
              sizeof(*state->collapsed), collapsed_key_cmp);
        return 1;
    }
    if (idx < 0) return 0;
    memmove(&state->collapsed[idx], &state->collapsed[idx + 1],
            (size_t)(state->collapsed_count - idx - 1) * sizeof(*state->collapsed));
    state->collapsed_count--;
    return 1;
}

static unsigned long long prev_total_ticks(const LuloProcState *state, int pid, int is_thread)
{
    struct LuloProcTimeSample key, *found;
    if (!state->times || state->time_count <= 0) return 0;
    key.pid = pid;
    key.is_thread = is_thread;
    key.total_ticks = 0;
    found = bsearch(&key, state->times, (size_t)state->time_count, sizeof(*state->times), prev_time_cmp);
    return found ? found->total_ticks : 0;
}

static int append_node(ProcNodeList *list, const ProcNode *node)
{
    if (ensure_node_cap(list, 1) < 0) return -1;
    list->items[list->count++] = *node;
    return 0;
}

static int append_row(ProcRowList *list, const LuloProcRow *row)
{
    if (ensure_row_cap(list, 1) < 0) return -1;
    list->items[list->count++] = *row;
    return 0;
}

static int build_proc_node(ProcNode *node,
                           const LuloProcState *state,
                           int pid,
                           int parent_pid,
                           int tgid,
                           int is_thread,
                           const char *user,
                           const char *command,
                           const char *comm,
                           char proc_state,
                           int policy,
                           int rt_priority,
                           int priority,
                           int nice_value,
                           long rss_pages,
                           unsigned long long total_ticks,
                           unsigned long long mem_total,
                           long page_size,
                           long hz,
                           unsigned long long cpu_total_delta,
                           int logical_cpus,
                           LuloProcCpuMode cpu_mode)
{
    unsigned long long prev_ticks = prev_total_ticks(state, pid, is_thread);
    unsigned long long delta = total_ticks > prev_ticks ? total_ticks - prev_ticks : 0;
    double cpu_pct = 0.0;
    double mem_pct = 0.0;
    unsigned long long rss_bytes = rss_pages > 0 ? (unsigned long long)rss_pages * (unsigned long long)page_size : 0;

    if (cpu_total_delta > 0) {
        double scale = cpu_mode == LULO_PROC_CPU_TOTAL ? 1.0 :
                       (logical_cpus > 0 ? (double)logical_cpus : 1.0);
        cpu_pct = (double)delta * scale * 1000.0 / (double)cpu_total_delta;
    }
    if (mem_total > 0) {
        mem_pct = (double)rss_bytes * 1000.0 / (double)mem_total;
    }

    memset(node, 0, sizeof(*node));
    node->pid = pid;
    node->parent_pid = parent_pid;
    node->tgid = tgid;
    node->is_thread = is_thread;
    node->is_kernel = command[0] == '[';
    node->state = proc_state;
    node->policy = policy;
    node->rt_priority = rt_priority;
    node->priority = priority;
    node->nice = nice_value;
    node->total_ticks = total_ticks;
    node->time_cs = hz > 0 ? total_ticks * 100ULL / (unsigned long long)hz : 0;
    node->cpu_tenths = cpu_pct < 0.0 ? 0 : (int)(cpu_pct + 0.5);
    node->mem_tenths = mem_pct < 0.0 ? 0 : (int)(mem_pct + 0.5);
    snprintf(node->user, sizeof(node->user), "%s", user);
    snprintf(node->command, sizeof(node->command), "%s", command[0] ? command : comm);
    node->parent_index = -1;
    node->first_child = -1;
    node->last_child = -1;
    node->next_sibling = -1;
    return 0;
}

static int gather_threads_for_process(ProcNodeList *nodes,
                                      const LuloProcState *state,
                                      int pid,
                                      const char *user,
                                      int thread_count,
                                      unsigned long long cpu_total_delta,
                                      int logical_cpus,
                                      long hz,
                                      LuloProcCpuMode cpu_mode)
{
    char task_path[128];
    DIR *dir;
    struct dirent *ent;

    if (state && (state->parents_only || proc_is_collapsed(state, pid, 0))) return 0;
    if (thread_count <= 1) return 0;
    snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
    dir = opendir(task_path);
    if (!dir) return 0;
    while ((ent = readdir(dir))) {
        char stat_path[160], comm[128], display[LULO_PROC_LABEL_MAX];
        char state_ch;
        int tid, ppid, policy = 0, rt_priority = 0, priority = 0, nice_value = 0;
        long rss_pages = 0;
        unsigned long long total_ticks = 0;
        ProcNode node;

        if (!is_numeric_name(ent->d_name)) continue;
        tid = atoi(ent->d_name);
        if (tid == pid) continue;

        snprintf(stat_path, sizeof(stat_path), "/proc/%d/task/%d/stat", pid, tid);
        if (read_proc_stat(stat_path, comm, sizeof(comm), &state_ch, &ppid, &thread_count, &policy, &rt_priority,
                           &priority, &nice_value, &total_ticks, &rss_pages) < 0) continue;
        snprintf(display, sizeof(display), "{%s}", comm);
        if (build_proc_node(&node, state, tid, pid, pid, 1, user, display, comm, state_ch,
                            policy, rt_priority, priority, nice_value, 0, total_ticks, 0, 1, hz,
                            cpu_total_delta, logical_cpus, cpu_mode) < 0) {
            closedir(dir);
            return -1;
        }
        if (append_node(nodes, &node) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static int gather_processes(ProcNodeList *nodes,
                            const LuloProcState *state,
                            unsigned long long cpu_total_delta,
                            unsigned long long mem_total,
                            int logical_cpus,
                            LuloProcCpuMode cpu_mode)
{
    DIR *dir = opendir("/proc");
    struct dirent *ent;
    long page_size = sysconf(_SC_PAGESIZE);
    long hz = sysconf(_SC_CLK_TCK);

    if (!dir) return -1;
    if (page_size <= 0) page_size = 4096;
    if (hz <= 0) hz = 100;

    while ((ent = readdir(dir))) {
        char proc_dir[64], stat_path[96], cmdline_path[96];
        char user[LULO_PROC_USER_MAX], comm[128], cmdline[LULO_PROC_LABEL_MAX], display[LULO_PROC_LABEL_MAX];
        struct stat st;
        char state_ch;
        int pid, ppid, num_threads = 0, policy = 0, rt_priority = 0, priority = 0, nice_value = 0;
        long rss_pages = 0;
        unsigned long long total_ticks = 0;
        ProcNode node;

        if (!is_numeric_name(ent->d_name)) continue;
        pid = atoi(ent->d_name);
        snprintf(proc_dir, sizeof(proc_dir), "/proc/%d", pid);
        if (stat(proc_dir, &st) < 0) continue;
        snprintf(stat_path, sizeof(stat_path), "%s/stat", proc_dir);
        if (read_proc_stat(stat_path, comm, sizeof(comm), &state_ch, &ppid, &num_threads, &policy, &rt_priority,
                           &priority, &nice_value, &total_ticks, &rss_pages) < 0) continue;
        uid_to_name(st.st_uid, user, sizeof(user));

        snprintf(cmdline_path, sizeof(cmdline_path), "%s/cmdline", proc_dir);
        if (read_cmdline_joined(cmdline_path, cmdline, sizeof(cmdline)) == 0) {
            snprintf(display, sizeof(display), "%s", cmdline);
        } else {
            snprintf(display, sizeof(display), "[%s]", comm);
        }

        if (build_proc_node(&node, state, pid, ppid, pid, 0, user, display, comm, state_ch,
                            policy, rt_priority, priority, nice_value,
                            rss_pages, total_ticks, mem_total, page_size, hz,
                            cpu_total_delta, logical_cpus, cpu_mode) < 0) {
            closedir(dir);
            return -1;
        }
        if (append_node(nodes, &node) < 0) {
            closedir(dir);
            return -1;
        }
        if (gather_threads_for_process(nodes, state, pid, user, num_threads,
                                       cpu_total_delta, logical_cpus, hz, cpu_mode) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static void link_process_tree(ProcNodeList *nodes)
{
    LeaderRef *leaders;
    int nleaders = 0;

    if (nodes->count <= 0) return;
    qsort(nodes->items, (size_t)nodes->count, sizeof(*nodes->items), proc_node_cmp);

    leaders = calloc((size_t)nodes->count, sizeof(*leaders));
    if (!leaders) return;
    for (int i = 0; i < nodes->count; i++) {
        nodes->items[i].parent_index = -1;
        nodes->items[i].first_child = -1;
        nodes->items[i].last_child = -1;
        nodes->items[i].next_sibling = -1;
        if (!nodes->items[i].is_thread) {
            leaders[nleaders].pid = nodes->items[i].pid;
            leaders[nleaders].index = i;
            nleaders++;
        }
    }
    qsort(leaders, (size_t)nleaders, sizeof(*leaders), leader_ref_cmp);

    for (int i = 0; i < nodes->count; i++) {
        LeaderRef key, *parent;
        int parent_pid = nodes->items[i].is_thread ? nodes->items[i].tgid : nodes->items[i].parent_pid;
        if (parent_pid <= 0) continue;
        key.pid = parent_pid;
        key.index = 0;
        parent = bsearch(&key, leaders, (size_t)nleaders, sizeof(*leaders), leader_ref_cmp);
        if (!parent || parent->index == i) continue;
        nodes->items[i].parent_index = parent->index;
        if (nodes->items[parent->index].first_child < 0) {
            nodes->items[parent->index].first_child = i;
        } else {
            nodes->items[nodes->items[parent->index].last_child].next_sibling = i;
        }
        nodes->items[parent->index].last_child = i;
    }
    free(leaders);
}

static int build_tree_prefix(char *buf, size_t len, const int *has_more, int depth, int is_last,
                             int has_children, int expanded)
{
    size_t used = 0;
    int cols = 0;

    if (len == 0) return 0;
    buf[0] = '\0';
    for (int i = 0; i < depth - 1 && used + 3 < len; i++) {
        const char *seg = has_more[i] ? "│ " : "  ";
        used += (size_t)snprintf(buf + used, len - used, "%s", seg);
        cols += 2;
    }
    if (depth > 0) {
        if (has_children && used + 6 < len) {
            used += (size_t)snprintf(buf + used, len - used, "%s%s ",
                                     is_last ? "└" : "├", expanded ? "▾" : "▸");
            cols += 3;
        } else if (used + 5 < len) {
            used += (size_t)snprintf(buf + used, len - used, "%s", is_last ? "└─ " : "├─ ");
            cols += 3;
        }
    } else if (has_children && used + 4 < len) {
        used += (size_t)snprintf(buf + used, len - used, "%s ", expanded ? "▾" : "▸");
        cols += 2;
    }
    return cols;
}

static int collect_sorted_indices(const ProcNodeList *nodes, const LuloProcState *state,
                                  int first_child, int roots_only,
                                  int **out_indices, int *out_count)
{
    int count = 0;
    int *indices;

    if (!nodes || !out_indices || !out_count) return -1;
    *out_indices = NULL;
    *out_count = 0;

    if (roots_only) {
        for (int i = 0; i < nodes->count; i++) {
            if (nodes->items[i].parent_index < 0) count++;
        }
    } else {
        for (int child = first_child; child >= 0; child = nodes->items[child].next_sibling) count++;
    }
    if (count <= 0) return 0;

    indices = calloc((size_t)count, sizeof(*indices));
    if (!indices) return -1;

    if (roots_only) {
        int pos = 0;
        for (int i = 0; i < nodes->count; i++) {
            if (nodes->items[i].parent_index < 0) indices[pos++] = i;
        }
    } else {
        int pos = 0;
        for (int child = first_child; child >= 0; child = nodes->items[child].next_sibling) {
            indices[pos++] = child;
        }
    }

    proc_sort_nodes = nodes;
    proc_sort_state = state;
    qsort(indices, (size_t)count, sizeof(*indices), proc_tree_order_cmp);
    *out_indices = indices;
    *out_count = count;
    return 0;
}

static int flatten_node_rows(const ProcNodeList *nodes, const LuloProcState *state, ProcRowList *rows,
                             int node_index, int *has_more, int depth, int is_last)
{
    ProcNode node = nodes->items[node_index];
    LuloProcRow row;
    char prefix[128];
    int *children = NULL;
    int child_count = 0;
    int expanded = 1;
    int prefix_cols;

    memset(&row, 0, sizeof(row));
    if (node.first_child >= 0) {
        if (state && state->parents_only) expanded = 0;
        else if (proc_is_collapsed(state, node.pid, node.is_thread)) expanded = 0;
    }
    prefix_cols = build_tree_prefix(prefix, sizeof(prefix), has_more, depth, is_last,
                                    node.first_child >= 0, expanded);
    row.pid = node.pid;
    row.tgid = node.tgid;
    row.parent_pid = node.parent_pid;
    row.is_thread = node.is_thread;
    row.is_kernel = node.is_kernel;
    row.depth = depth;
    row.has_children = node.first_child >= 0;
    row.expanded = row.has_children ? expanded : 0;
    row.state = node.state;
    row.policy = node.policy;
    row.rt_priority = node.rt_priority;
    row.priority = node.priority;
    row.nice = node.nice;
    row.cpu_tenths = node.cpu_tenths;
    row.mem_tenths = node.mem_tenths;
    row.time_cs = node.time_cs;
    row.label_prefix_len = (int)strlen(prefix);
    row.label_prefix_cols = prefix_cols;
    snprintf(row.user, sizeof(row.user), "%s", node.user);
    {
        int prefix_len = (int)strlen(prefix);
        int cmd_len;

        if (prefix_len > (int)sizeof(row.label) - 1) prefix_len = (int)sizeof(row.label) - 1;
        cmd_len = (int)sizeof(row.label) - 1 - prefix_len;
        if (cmd_len < 0) cmd_len = 0;
        snprintf(row.label, sizeof(row.label), "%.*s%.*s",
                 prefix_len, prefix, cmd_len, node.command);
    }
    if (append_row(rows, &row) < 0) return -1;

    if (state && state->parents_only) {
        int visible_child_count = 0;
        int visible_idx = 0;

        if (collect_sorted_indices(nodes, state, node.first_child, 0, &children, &child_count) < 0) return -1;
        for (int i = 0; i < child_count; i++) {
            if (nodes->items[children[i]].first_child >= 0) visible_child_count++;
        }
        for (int i = 0; i < child_count; i++) {
            int child = children[i];

            if (nodes->items[child].first_child < 0) continue;
            visible_idx++;
            has_more[depth] = visible_idx < visible_child_count;
            if (flatten_node_rows(nodes, state, rows, child, has_more, depth + 1,
                                  visible_idx == visible_child_count) < 0) {
                free(children);
                return -1;
            }
        }
        free(children);
        return 0;
    }
    if (!expanded) return 0;
    if (collect_sorted_indices(nodes, state, node.first_child, 0, &children, &child_count) < 0) return -1;
    for (int i = 0; i < child_count; i++) {
        has_more[depth] = i + 1 < child_count;
        if (flatten_node_rows(nodes, state, rows, children[i], has_more, depth + 1,
                              i + 1 == child_count) < 0) {
            free(children);
            return -1;
        }
    }
    free(children);
    return 0;
}

static int flatten_process_rows(const ProcNodeList *nodes, const LuloProcState *state, ProcRowList *rows)
{
    int has_more[64] = {0};
    int *roots = NULL;
    int root_count = 0;

    if (collect_sorted_indices(nodes, state, -1, 1, &roots, &root_count) < 0) return -1;
    for (int i = 0; i < root_count; i++) {
        if (flatten_node_rows(nodes, state, rows, roots[i], has_more, 0,
                              i + 1 == root_count) < 0) {
            free(roots);
            return -1;
        }
    }
    free(roots);
    return 0;
}

LuloProcCpuMode lulo_next_proc_cpu_mode(LuloProcCpuMode mode)
{
    return mode == LULO_PROC_CPU_TOTAL ? LULO_PROC_CPU_PER_CORE : LULO_PROC_CPU_TOTAL;
}

const char *lulo_proc_cpu_mode_name(LuloProcCpuMode mode)
{
    return mode == LULO_PROC_CPU_TOTAL ? "total" : "per-core";
}

static void replace_prev_times(LuloProcState *state, const ProcNodeList *nodes)
{
    int cap = nodes->count > 0 ? nodes->count : 1;
    struct LuloProcTimeSample *times = state->times;

    if (state->time_cap < cap) {
        times = realloc(state->times, (size_t)cap * sizeof(*times));
        if (!times) return;
        state->times = times;
        state->time_cap = cap;
    }
    for (int i = 0; i < nodes->count; i++) {
        times[i].pid = nodes->items[i].pid;
        times[i].is_thread = nodes->items[i].is_thread;
        times[i].total_ticks = nodes->items[i].total_ticks;
    }
    qsort(times, (size_t)nodes->count, sizeof(*times), prev_time_cmp);
    state->time_count = nodes->count;
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void set_selected_by_index(LuloProcState *state, const LuloProcSnapshot *snap, int index)
{
    if (!snap || snap->count <= 0) {
        state->selected = 0;
        state->selected_pid = 0;
        state->selected_is_thread = 0;
        return;
    }
    index = clamp_int(index, 0, snap->count - 1);
    state->selected = index;
    state->selected_pid = snap->rows[index].pid;
    state->selected_is_thread = snap->rows[index].is_thread;
}

void lulo_proc_state_init(LuloProcState *state)
{
    memset(state, 0, sizeof(*state));
    state->sort_key = LULO_PROC_SORT_CPU;
    state->sort_desc = 1;
}

void lulo_proc_state_cleanup(LuloProcState *state)
{
    free(state->times);
    free(state->collapsed);
    memset(state, 0, sizeof(*state));
}

int lulo_proc_snapshot_gather(LuloProcSnapshot *snap, LuloProcState *state,
                              unsigned long long cpu_total_delta,
                              unsigned long long mem_total,
                              int logical_cpus,
                              LuloProcCpuMode cpu_mode)
{
    ProcNodeList nodes = {0};
    ProcRowList rows = {0};
    int rc = -1;

    snap->rows = NULL;
    snap->count = 0;
    if (gather_processes(&nodes, state, cpu_total_delta, mem_total, logical_cpus, cpu_mode) < 0) goto done;
    link_process_tree(&nodes);
    if (flatten_process_rows(&nodes, state, &rows) < 0) goto done;
    replace_prev_times(state, &nodes);
    snap->rows = rows.items;
    snap->count = rows.count;
    rows.items = NULL;
    rows.count = 0;
    rc = 0;

done:
    free(nodes.items);
    free(rows.items);
    return rc;
}

void lulo_proc_snapshot_free(LuloProcSnapshot *snap)
{
    free(snap->rows);
    snap->rows = NULL;
    snap->count = 0;
}

void lulo_proc_view_sync(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows)
{
    int max_scroll;
    int selected = -1;

    if (!snap || snap->count <= 0) {
        state->selected = 0;
        state->scroll = 0;
        state->selected_pid = 0;
        state->selected_is_thread = 0;
        return;
    }
    if (state->selected_pid > 0) {
        for (int i = 0; i < snap->count; i++) {
            if (snap->rows[i].pid == state->selected_pid &&
                snap->rows[i].is_thread == state->selected_is_thread) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0) selected = clamp_int(state->selected, 0, snap->count - 1);
    set_selected_by_index(state, snap, selected);

    if (visible_rows <= 0) {
        state->scroll = 0;
        return;
    }
    max_scroll = snap->count > visible_rows ? snap->count - visible_rows : 0;
    if (state->selected < state->scroll) state->scroll = state->selected;
    if (state->selected >= state->scroll + visible_rows) state->scroll = state->selected - visible_rows + 1;
    state->scroll = clamp_int(state->scroll, 0, max_scroll);
}

void lulo_proc_view_move(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows, int delta)
{
    lulo_proc_view_sync(state, snap, visible_rows);
    set_selected_by_index(state, snap, state->selected + delta);
    lulo_proc_view_sync(state, snap, visible_rows);
}

void lulo_proc_view_page(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows, int pages)
{
    int delta = visible_rows > 0 ? visible_rows * pages : pages;
    lulo_proc_view_move(state, snap, visible_rows, delta);
}

void lulo_proc_view_home(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows)
{
    lulo_proc_view_sync(state, snap, visible_rows);
    set_selected_by_index(state, snap, 0);
    lulo_proc_view_sync(state, snap, visible_rows);
}

void lulo_proc_view_end(LuloProcState *state, const LuloProcSnapshot *snap, int visible_rows)
{
    if (!snap || snap->count <= 0) return;
    set_selected_by_index(state, snap, snap->count - 1);
    lulo_proc_view_sync(state, snap, visible_rows);
}

void lulo_proc_sort_toggle(LuloProcState *state, LuloProcSortKey key)
{
    if (!state || key < 0 || key >= LULO_PROC_SORT_COUNT) return;
    if (state->sort_key == key) {
        state->sort_desc = !state->sort_desc;
    } else {
        state->sort_key = key;
        state->sort_desc = (key == LULO_PROC_SORT_CPU ||
                            key == LULO_PROC_SORT_MEM ||
                            key == LULO_PROC_SORT_TIME);
    }
}

int lulo_proc_toggle_row(LuloProcState *state, const LuloProcSnapshot *snap, int index)
{
    const LuloProcRow *row;
    int changed;

    if (!state || !snap || index < 0 || index >= snap->count) return 0;
    row = &snap->rows[index];
    if (!row->has_children) return 0;
    if (state->parents_only) {
        int keep_depth = row->depth;

        state->parents_only = 0;
        state->collapsed_count = 0;
        for (int i = 0; i < snap->count; i++) {
            const LuloProcRow *candidate = &snap->rows[i];
            int keep = 0;

            if (!candidate->has_children) continue;
            if (i == index) continue;
            if (candidate->depth < keep_depth) {
                for (int j = index - 1; j >= 0; j--) {
                    if (snap->rows[j].depth == candidate->depth) {
                        if (j == i) keep = 1;
                        break;
                    }
                }
            }
            if (keep) continue;
            if (proc_set_collapsed(state, candidate->pid, candidate->is_thread, 1) < 0) {
                return 0;
            }
        }
        state->selected_pid = row->pid;
        state->selected_is_thread = row->is_thread;
        return 1;
    }
    changed = proc_set_collapsed(state, row->pid, row->is_thread, row->expanded);
    if (changed < 0) return 0;
    state->selected_pid = row->pid;
    state->selected_is_thread = row->is_thread;
    return changed;
}

void lulo_proc_expand_all(LuloProcState *state)
{
    if (!state) return;
    state->parents_only = 0;
    state->collapsed_count = 0;
}

int lulo_proc_collapse_all(LuloProcState *state, const LuloProcSnapshot *snap)
{
    if (!state || !snap || snap->count <= 0) return 0;
    if (state->parents_only) return 0;
    state->parents_only = 1;
    state->collapsed_count = 0;
    return 1;
}
