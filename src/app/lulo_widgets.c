#define _GNU_SOURCE

#include "lulo_app.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    LuloRect fs;
    LuloRect dev;
    LuloRect io;
    LuloRect queue;
    int show_dev;
    int show_io;
    int show_queue;
} DiskWidgetLayout;

static uint64_t rgb_channels_local(Rgb fg, Rgb bg)
{
    uint64_t channels = 0;

    ncchannels_set_fg_rgb8(&channels, fg.r, fg.g, fg.b);
    ncchannels_set_bg_rgb8(&channels, bg.r, bg.g, bg.b);
    return channels;
}

int rect_valid(const LuloRect *rect)
{
    return rect && rect->height >= 3 && rect->width >= 12;
}

int rect_inner_rows(const LuloRect *rect)
{
    if (!rect_valid(rect)) return 0;
    return rect->height - 2;
}

int rect_inner_cols(const LuloRect *rect)
{
    if (!rect_valid(rect)) return 0;
    return rect->width - 2;
}

void draw_inner_box(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                    Rgb border, const char *title, Rgb title_color)
{
    if (!rect_valid(rect)) return;
    ncplane_cursor_move_yx(p, rect->row, rect->col);
    ncplane_rounded_box_sized(p, 0, rgb_channels_local(border, theme->bg),
                              (unsigned)rect->height, (unsigned)rect->width, 0);
    if (title && *title) {
        int max_title = rect->width - 6;
        if (max_title > 0) {
            plane_putn(p, rect->row, rect->col + 2, title_color, theme->bg, title, max_title);
        }
    }
}

void draw_inner_meta(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                     const char *text, Rgb fg)
{
    int width;
    int x;

    if (!rect_valid(rect) || !text || !*text) return;
    width = (int)strlen(text);
    x = rect->col + rect->width - width - 2;
    if (x <= rect->col + 2) return;
    plane_putn(p, rect->row, x, fg, theme->bg, text, width);
}

Rgb disk_usage_fill(const Theme *theme, int pct)
{
    if (pct >= 90) return theme->red;
    if (pct >= 75) return theme->mem_fill[4];
    if (pct >= 50) return theme->mem_fill[2];
    if (pct >= 25) return theme->mem_fill[1];
    return theme->mem_fill[0];
}

Rgb disk_usage_text(const Theme *theme, int pct)
{
    if (pct >= 90) return theme->red;
    if (pct >= 75) return theme->orange;
    if (pct >= 50) return theme->yellow;
    if (pct >= 25) return theme->green;
    return theme->cyan;
}

void render_inline_meter(struct ncplane *p, const Theme *theme, int y, int x, int width,
                         int pct, Rgb fill)
{
    int used;
    int free_w;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (width <= 0) return;
    used = pct * width / 100;
    free_w = width - used;
    if (used > 0) plane_fill(p, y, x, used, theme->bg, fill);
    if (free_w > 0) plane_fill(p, y, x + used, free_w, theme->bg, theme->mem_free_bg);
}

static void plane_clear_inner(struct ncplane *p, const Theme *theme, int rows, int cols)
{
    for (int y = 1; y < rows - 1; y++) {
        plane_fill(p, y, 1, cols - 2, theme->bg, theme->bg);
    }
}

static void format_bytes_local(unsigned long long bytes, char *buf, size_t len)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(buf, len, "%.0f %s", value, units[unit]);
    else if (value >= 100.0) snprintf(buf, len, "%.0f %s", value, units[unit]);
    else snprintf(buf, len, "%.1f %s", value, units[unit]);
}

static Rgb disk_fs_color(const Theme *theme, int idx)
{
    static const int palette[] = { 0, 1, 2, 4, 5, 3 };
    int slot = palette[idx % (int)(sizeof(palette) / sizeof(palette[0]))];

    return theme->mem_fill[slot];
}

static Rgb disk_total_fill(const Theme *theme)
{
    if (theme->mono) return theme->mem_fill[3];
    return (Rgb){ 112, 82, 184 };
}

static Rgb disk_total_text(const Theme *theme)
{
    if (theme->mono) return theme->white;
    return (Rgb){ 212, 188, 255 };
}

static void build_disk_widget_layout(const Ui *ui, DiskWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_h;
    int inner_w;
    int reserve_bottom;
    int lower_row;
    int lower_h;

    memset(layout, 0, sizeof(*layout));
    if (!ui->disk) return;
    ncplane_dim_yx(ui->disk, &rows, &cols);
    if (rows < 5 || cols < 24) return;

    inner_h = (int)rows - 2;
    inner_w = (int)cols - 2;
    layout->fs.row = 1;
    layout->fs.col = 1;
    layout->fs.width = inner_w;
    layout->fs.height = inner_h;

    if (inner_h < 10) return;

    reserve_bottom = inner_w >= 96 ? 9 : inner_w >= 72 ? 8 : 6;
    layout->fs.height = clamp_int(inner_h - reserve_bottom - 1, 6, 10);
    if (layout->fs.height >= inner_h - 3) return;

    lower_row = layout->fs.row + layout->fs.height + 1;
    lower_h = (int)rows - 1 - lower_row;
    if (lower_h < 4) {
        layout->fs.height = inner_h;
        return;
    }

    if (inner_w >= 96) {
        int left_w = clamp_int(inner_w * 11 / 20, 32, inner_w - 34);
        int right_w = inner_w - left_w - 1;

        layout->dev.row = lower_row;
        layout->dev.col = 1;
        layout->dev.width = left_w;
        layout->dev.height = lower_h;
        layout->show_dev = rect_valid(&layout->dev);

        layout->io.col = left_w + 2;
        layout->io.width = right_w;
        if (lower_h >= 10) {
            int io_h = clamp_int(lower_h / 2, 4, lower_h - 5);
            int queue_h = lower_h - io_h - 1;

            layout->io.row = lower_row;
            layout->io.height = io_h;
            layout->queue.row = lower_row + io_h + 1;
            layout->queue.col = layout->io.col;
            layout->queue.width = right_w;
            layout->queue.height = queue_h;
            layout->show_queue = rect_valid(&layout->queue);
        } else {
            layout->io.row = lower_row;
            layout->io.height = lower_h;
        }
        layout->show_io = rect_valid(&layout->io);
        return;
    }

    if (lower_h >= 12) {
        int dev_h = clamp_int(lower_h / 3 + 1, 4, lower_h - 8);
        int io_h = clamp_int((lower_h - dev_h - 1) / 2, 4, lower_h - dev_h - 5);
        int queue_h = lower_h - dev_h - io_h - 2;

        layout->dev.row = lower_row;
        layout->dev.col = 1;
        layout->dev.width = inner_w;
        layout->dev.height = dev_h;
        layout->show_dev = rect_valid(&layout->dev);

        layout->io.row = lower_row + dev_h + 1;
        layout->io.col = 1;
        layout->io.width = inner_w;
        layout->io.height = io_h;
        layout->show_io = rect_valid(&layout->io);

        layout->queue.row = layout->io.row + io_h + 1;
        layout->queue.col = 1;
        layout->queue.width = inner_w;
        layout->queue.height = queue_h;
        layout->show_queue = rect_valid(&layout->queue);
        return;
    }

    layout->dev.row = lower_row;
    layout->dev.col = 1;
    layout->dev.width = inner_w;
    layout->dev.height = clamp_int(lower_h / 2, 4, lower_h);
    layout->show_dev = rect_valid(&layout->dev);

    layout->io.row = lower_row + layout->dev.height + 1;
    layout->io.col = 1;
    layout->io.width = inner_w;
    layout->io.height = lower_h - layout->dev.height - 1;
    layout->show_io = rect_valid(&layout->io);
}

int disk_visible_rows(const Ui *ui)
{
    DiskWidgetLayout layout;

    if (!ui->disk) return 1;
    build_disk_widget_layout(ui, &layout);
    return clamp_int(rect_inner_rows(&layout.fs) - 1, 1, 1024);
}

static void render_disk_filesystems(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                    const LuloDizkSnapshot *snap, const LuloDizkState *state)
{
    char rows_buf[48];
    int total_row = 1;
    int visible_rows;
    int start;
    int dev_w;
    int mount_w;
    int value_w;
    int bar_w;
    int pct_x;
    int value_x;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Filesystems ", theme->white);

    visible_rows = clamp_int(rect_inner_rows(rect) - total_row, 0, 1024);
    start = state ? clamp_int(state->scroll, 0, snap && snap->fs_count > visible_rows ? snap->fs_count - visible_rows : 0) : 0;
    if (snap && snap->fs_count > 0) {
        snprintf(rows_buf, sizeof(rows_buf), "%d-%d/%d",
                 start + 1,
                 clamp_int(start + visible_rows, 1, snap->fs_count),
                 snap->fs_count);
        draw_inner_meta(p, theme, rect, rows_buf, theme->cyan);
    }

    if (!snap || snap->fs_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no mounted filesystems", rect->width - 4);
        return;
    }

    dev_w = rect->width >= 84 ? 12 : 8;
    mount_w = rect->width >= 84 ? 16 : 12;
    value_w = rect->width >= 96 ? 18 : rect->width >= 72 ? 14 : 11;
    bar_w = rect_inner_cols(rect) - dev_w - mount_w - value_w - 8;
    if (bar_w < 8) {
        mount_w = clamp_int(mount_w - (8 - bar_w), 8, mount_w);
        bar_w = rect_inner_cols(rect) - dev_w - mount_w - value_w - 8;
    }
    if (bar_w < 8) bar_w = 8;
    pct_x = rect->col + rect->width - value_w - 6;
    value_x = rect->col + rect->width - value_w - 1;

    if (snap && snap->fs_count > 0) {
        unsigned long long total_used = 0;
        unsigned long long total_size = 0;
        int y = rect->row + 1;
        int x = rect->col + 1;
        int pct = 0;
        char pctbuf[8];
        char used_buf[20];
        char total_buf[20];
        char value[40];
        int value_len;
        Rgb total_fill = disk_total_fill(theme);
        Rgb total_text = disk_total_text(theme);

        for (int i = 0; i < snap->fs_count; i++) {
            total_used += snap->filesystems[i].used_bytes;
            total_size += snap->filesystems[i].total_bytes;
        }
        pct = total_size > 0 ? clamp_int((int)((total_used * 100ULL) / total_size), 0, 100) : 0;

        plane_putn(p, y, x, total_text, theme->bg, "total", dev_w);
        x += dev_w + 1;
        plane_putn(p, y, x, total_text, theme->bg, "all mounts", mount_w);
        x += mount_w + 1;
        render_inline_meter(p, theme, y, x, bar_w, pct, total_fill);
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", pct);
        plane_putn(p, y, pct_x, total_text, theme->bg, pctbuf, 4);
        format_bytes_local(total_used, used_buf, sizeof(used_buf));
        format_bytes_local(total_size, total_buf, sizeof(total_buf));
        snprintf(value, sizeof(value), "%s/%s", used_buf, total_buf);
        value_len = (int)strlen(value);
        plane_putn(p, y, value_x + value_w - value_len, total_text, theme->bg, value, value_len);
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 1 + total_row + i;
        int x = rect->col + 1;
        char pctbuf[8];
        char value[40];
        int value_len;
        Rgb color;

        if (idx >= snap->fs_count) break;
        color = disk_fs_color(theme, idx);
        plane_putn(p, y, x, color, theme->bg, snap->filesystems[idx].device, dev_w);
        x += dev_w + 1;
        plane_putn(p, y, x, theme->white, theme->bg, snap->filesystems[idx].mount, mount_w);
        x += mount_w + 1;
        render_inline_meter(p, theme, y, x, bar_w, snap->filesystems[idx].pct, color);
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", snap->filesystems[idx].pct);
        plane_putn(p, y, pct_x, color, theme->bg, pctbuf, 4);
        if (value_w >= 14) snprintf(value, sizeof(value), "%s/%s",
                                    snap->filesystems[idx].used, snap->filesystems[idx].total);
        else snprintf(value, sizeof(value), "%s", snap->filesystems[idx].used);
        value_len = (int)strlen(value);
        plane_putn(p, y, value_x + value_w - value_len, color, theme->bg, value, value_len);
    }
}

static void render_disk_devices(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                const LuloDizkSnapshot *snap)
{
    int visible_rows;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Devices ", theme->white);
    if (snap && snap->blockdev_count > 0) {
        char meta[16];
        snprintf(meta, sizeof(meta), "%d", snap->blockdev_count);
        draw_inner_meta(p, theme, rect, meta, theme->green);
    }

    visible_rows = rect_inner_rows(rect);
    if (!snap || snap->blockdev_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no block devices", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        int right_w;
        const LuloDizkBlockRow *row;
        char kind[48];

        if (i >= snap->blockdev_count) break;
        row = &snap->blockdevs[i];
        snprintf(kind, sizeof(kind), "%s %s", row->type, row->transport);
        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, 10);
        x += 11;
        plane_putn(p, y, x, theme->white, theme->bg, row->size, 10);
        x += 11;
        plane_putn(p, y, x, theme->green, theme->bg, kind, 10);
        x += 11;
        right_w = rect->col + rect->width - 1 - x;
        if (right_w > 0) plane_putn(p, y, x, theme->white, theme->bg, row->model, right_w);
    }
}

static void render_disk_io(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                           const LuloDizkSnapshot *snap)
{
    int visible_rows;
    int name_w;
    int bar_w;
    int data_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " I/O ", theme->white);
    if (snap && snap->iostat_count > 0) {
        char meta[16];
        snprintf(meta, sizeof(meta), "%d", snap->iostat_count);
        draw_inner_meta(p, theme, rect, meta, theme->green);
    }

    visible_rows = rect_inner_rows(rect);
    if (!snap || snap->iostat_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no disk activity data", rect->width - 4);
        return;
    }

    name_w = rect->width >= 38 ? 10 : 8;
    data_w = rect->width >= 46 ? 9 : 7;
    bar_w = rect_inner_cols(rect) - name_w - data_w * 2 - 8;
    if (bar_w < 6) bar_w = 6;

    for (int i = 0; i < visible_rows; i++) {
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        char pctbuf[8];
        const LuloDizkIoRow *row;

        if (i >= snap->iostat_count) break;
        row = &snap->iostats[i];
        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, name_w);
        x += name_w + 1;
        render_inline_meter(p, theme, y, x, bar_w, row->util_pct, disk_usage_fill(theme, row->util_pct));
        x += bar_w + 1;
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", row->util_pct);
        plane_putn(p, y, x, disk_usage_text(theme, row->util_pct), theme->bg, pctbuf, 4);
        x += 5;
        plane_putn(p, y, x, theme->green, theme->bg, row->rd_bytes, data_w);
        x += data_w + 1;
        plane_putn(p, y, x, theme->orange, theme->bg, row->wr_bytes, data_w);
    }
}

static void render_disk_queue(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                              const LuloDizkSnapshot *snap)
{
    int visible_rows;
    int tunable_rows;
    int swap_rows;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Queue / Swap ", theme->white);
    if (snap) {
        char meta[32];
        snprintf(meta, sizeof(meta), "q %d  s %d", snap->tunable_count, snap->swap_count);
        draw_inner_meta(p, theme, rect, meta, theme->green);
    }

    visible_rows = rect_inner_rows(rect);
    tunable_rows = snap ? snap->tunable_count : 0;
    if (snap && snap->swap_count > 0 && visible_rows >= 3) {
        swap_rows = clamp_int(snap->swap_count, 1, visible_rows / 2);
        tunable_rows = clamp_int(tunable_rows, 0, visible_rows - swap_rows);
    } else {
        swap_rows = 0;
        tunable_rows = clamp_int(tunable_rows, 0, visible_rows);
    }

    if ((!snap || snap->tunable_count <= 0) && (!snap || snap->swap_count <= 0)) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no queue or swap data", rect->width - 4);
        return;
    }

    for (int i = 0; i < tunable_rows; i++) {
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        int tail_w;
        char buf[64];
        const LuloDizkTunableRow *row = &snap->tunables[i];

        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, 10);
        x += 11;
        plane_putn(p, y, x, theme->green, theme->bg, row->scheduler, 10);
        x += 11;
        snprintf(buf, sizeof(buf), "%s %d/%d", row->cache, row->read_ahead_kb, row->nr_requests);
        plane_putn(p, y, x, theme->yellow, theme->bg, buf, 12);
        x += 13;
        tail_w = rect->col + rect->width - 1 - x;
        if (tail_w > 0) plane_putn(p, y, x, theme->white, theme->bg, row->state, tail_w);
    }

    for (int i = 0; i < swap_rows; i++) {
        int y = rect->row + 1 + tunable_rows + i;
        int x = rect->col + 1;
        int bar_w = clamp_int(rect_inner_cols(rect) - 28, 8, 20);
        char pctbuf[8];
        char usedbuf[40];
        const LuloDizkSwapRow *row = &snap->swaps[i];

        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, 10);
        x += 11;
        render_inline_meter(p, theme, y, x, bar_w, row->pct, disk_usage_fill(theme, row->pct));
        x += bar_w + 1;
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", row->pct);
        plane_putn(p, y, x, disk_usage_text(theme, row->pct), theme->bg, pctbuf, 4);
        x += 5;
        snprintf(usedbuf, sizeof(usedbuf), "%s/%s", row->used, row->size);
        plane_putn(p, y, x, theme->white, theme->bg, usedbuf, rect->col + rect->width - 1 - x);
    }
}

void render_disk_widget(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state)
{
    DiskWidgetLayout layout;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->disk) return;
    ncplane_dim_yx(ui->disk, &rows, &cols);
    plane_clear_inner(ui->disk, ui->theme, (int)rows, (int)cols);
    build_disk_widget_layout(ui, &layout);
    render_disk_filesystems(ui->disk, ui->theme, &layout.fs, snap, state);
    if (layout.show_dev) render_disk_devices(ui->disk, ui->theme, &layout.dev, snap);
    if (layout.show_io) render_disk_io(ui->disk, ui->theme, &layout.io, snap);
    if (layout.show_queue) render_disk_queue(ui->disk, ui->theme, &layout.queue, snap);
}

void render_disk_status(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state)
{
    char buf[160];
    int visible_rows;

    if (!ui->load) return;
    plane_reset(ui->load, ui->theme);
    visible_rows = disk_visible_rows(ui);
    if (!snap) return;
    snprintf(buf, sizeof(buf),
             "fs %d  dev %d  io %d  queue %d  swap %d  fstab %d  fs-scroll %d-%d/%d",
             snap->fs_count, snap->blockdev_count, snap->iostat_count,
             snap->tunable_count, snap->swap_count, snap->fstab_count,
             snap->fs_count > 0 ? state->scroll + 1 : 0,
             snap->fs_count > 0 ? clamp_int(state->scroll + visible_rows, 1, snap->fs_count) : 0,
             snap->fs_count > 0 ? snap->fs_count : 0);
    plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
}
