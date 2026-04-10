CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra
NC_CFLAGS := $(shell $(PKG_CONFIG) --cflags notcurses)
NC_LIBS := $(shell $(PKG_CONFIG) --libs notcurses)
SYSTEMD_LIBS := $(shell $(PKG_CONFIG) --libs libsystemd)

.PHONY: all clean install-lulod-user-service restart-lulod-user-service status-lulod-user-service

all: lulo lulod nc_input_probe

lulo: lulo.c lulo_model.c lulo_model.h lulo_proc.c lulo_proc.h lulo_dizk.c lulo_dizk.h lulo_systemd.c lulo_systemd.h lulo_systemd_backend.c lulo_systemd_backend.h lulod_ipc.c lulod_ipc.h
	$(CC) $(CFLAGS) $(NC_CFLAGS) -o $@ lulo.c lulo_model.c lulo_proc.c lulo_dizk.c lulo_systemd.c lulo_systemd_backend.c lulod_ipc.c $(NC_LIBS) -lm -lpthread

lulod: lulod.c lulo_systemd.c lulo_systemd.h lulod_systemd.c lulod_systemd.h lulod_ipc.c lulod_ipc.h
	$(CC) $(CFLAGS) -o $@ lulod.c lulo_systemd.c lulod_systemd.c lulod_ipc.c $(SYSTEMD_LIBS) -lm

nc_input_probe: nc_input_probe.c
	$(CC) $(CFLAGS) $(NC_CFLAGS) -o $@ nc_input_probe.c $(NC_LIBS)

clean:
	rm -f lulo lulod nc_input_probe

install-lulod-user-service: lulod
	chmod +x ./install-lulod-user-service.sh
	./install-lulod-user-service.sh

restart-lulod-user-service:
	systemctl --user daemon-reload
	systemctl --user restart lulod.service

status-lulod-user-service:
	systemctl --user status lulod.service --no-pager --lines=20
