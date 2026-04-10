#ifndef LULO_DIZK_H
#define LULO_DIZK_H

#define LULO_DIZK_MAX_FS 32
#define LULO_DIZK_MAX_BLOCKDEVS 16
#define LULO_DIZK_MAX_IOSTATS 16
#define LULO_DIZK_MAX_TUNABLES 16
#define LULO_DIZK_MAX_SWAP 16

typedef struct {
    char device[24];
    char mount[24];
    char fstype[12];
    char used[16];
    char avail[16];
    char total[16];
    unsigned long long used_bytes;
    unsigned long long total_bytes;
    int pct;
} LuloDizkFsRow;

typedef struct {
    char name[16];
    char size[16];
    char type[8];
    char transport[12];
    char model[56];
} LuloDizkBlockRow;

typedef struct {
    char name[16];
    char rd_bytes[16];
    char wr_bytes[16];
    unsigned long long rd_ios;
    unsigned long long wr_ios;
    int util_pct;
} LuloDizkIoRow;

typedef struct {
    char name[16];
    char scheduler[16];
    char cache[8];
    char state[32];
    char numa_node[8];
    int read_ahead_kb;
    int nr_requests;
    int max_sectors_kb;
} LuloDizkTunableRow;

typedef struct {
    char name[24];
    char type[12];
    char size[16];
    char used[16];
    int priority;
    int pct;
} LuloDizkSwapRow;

typedef struct {
    int count;
    int fs_count;
    LuloDizkFsRow filesystems[LULO_DIZK_MAX_FS];
    int blockdev_count;
    LuloDizkBlockRow blockdevs[LULO_DIZK_MAX_BLOCKDEVS];
    int iostat_count;
    LuloDizkIoRow iostats[LULO_DIZK_MAX_IOSTATS];
    int tunable_count;
    LuloDizkTunableRow tunables[LULO_DIZK_MAX_TUNABLES];
    int swap_count;
    LuloDizkSwapRow swaps[LULO_DIZK_MAX_SWAP];
    int fstab_count;
} LuloDizkSnapshot;

typedef struct {
    int scroll;
    int show_all;
    int show_mount_opts;
} LuloDizkState;

void lulo_dizk_state_init(LuloDizkState *state);
void lulo_dizk_state_cleanup(LuloDizkState *state);

int lulo_dizk_snapshot_gather(LuloDizkSnapshot *snap, const LuloDizkState *state, int width);
void lulo_dizk_snapshot_free(LuloDizkSnapshot *snap);

void lulo_dizk_view_sync(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows);
void lulo_dizk_view_move(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows, int delta);
void lulo_dizk_view_page(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows, int pages);
void lulo_dizk_view_home(LuloDizkState *state);
void lulo_dizk_view_end(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows);

#endif
