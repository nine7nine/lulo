#define _GNU_SOURCE

#include <locale.h>
#include <notcurses/notcurses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void)
{
    FILE *log = fopen("/tmp/nc_input_probe.log", "w");
    struct notcurses *nc;
    struct ncplane *std;
    notcurses_options opts = {
        .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_INHIBIT_SETLOCALE,
    };

    setlocale(LC_ALL, "");
    nc = notcurses_init(&opts, NULL);
    if (!nc) {
        fprintf(stderr, "init failed\n");
        if (log) fclose(log);
        return 1;
    }
    std = notcurses_stdplane(nc);
    notcurses_mice_enable(nc, NCMICE_BUTTON_EVENT);
    ncplane_putstr_yx(std, 0, 0, "nc_input_probe: press keys, click, or wheel. q exits.");
    ncplane_putstr_yx(std, 1, 0, "logs: /tmp/nc_input_probe.log");
    notcurses_render(nc);
    if (log) {
        fprintf(log, "probe-start term=%s\n", getenv("TERM"));
        fflush(log);
    }

    for (;;) {
        ncinput ni;
        uint32_t id;

        memset(&ni, 0, sizeof(ni));
        errno = 0;
        id = notcurses_get_blocking(nc, &ni);
        if (id == (uint32_t)-1) {
            if (errno == 0) {
                if (log) {
                    fprintf(log, "minus1 errno=0\n");
                    fflush(log);
                }
                continue;
            }
            ncplane_erase(std);
            ncplane_printf_yx(std, 0, 0, "notcurses_get_blocking() error errno=%d", errno);
            notcurses_render(nc);
            if (log) {
                fprintf(log, "error errno=%d\n", errno);
                fflush(log);
            }
            break;
        }

        if (log) {
            fprintf(log, "id=%u evtype=%d y=%d x=%d mods=%u utf8='%s'\n",
                    id, (int)ni.evtype, ni.y, ni.x, ni.modifiers, ni.utf8);
            fflush(log);
        }
        ncplane_erase(std);
        ncplane_printf_yx(std, 0, 0,
                          "id=%u evtype=%d y=%d x=%d mods=%u utf8='%s'",
                          id, (int)ni.evtype, ni.y, ni.x, ni.modifiers, ni.utf8);
        ncplane_putstr_yx(std, 1, 0, "press q to exit");
        notcurses_render(nc);
        if (id == NCKEY_EOF) break;
        if (id == 'q' || id == 'Q') break;
    }

    notcurses_stop(nc);
    if (log) fclose(log);
    return 0;
}
