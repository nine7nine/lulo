CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -Iinclude
NC_CFLAGS := $(shell $(PKG_CONFIG) --cflags notcurses)
NC_LIBS := $(shell $(PKG_CONFIG) --libs notcurses)
SYSTEMD_LIBS := $(shell $(PKG_CONFIG) --libs libsystemd)

SRC_APP := src/app
SRC_ADMIN := src/admin
SRC_CORE := src/core
SRC_DAEMON := src/daemon
SRC_SHARED := src/shared

LULO_SRCS := \
	$(SRC_APP)/lulo.c \
	$(SRC_APP)/lulo_input.c \
	$(SRC_CORE)/lulo_model.c \
	$(SRC_CORE)/lulo_proc.c \
	$(SRC_CORE)/lulo_dizk.c \
	$(SRC_CORE)/lulo_systemd.c \
	$(SRC_CORE)/lulo_systemd_backend.c \
	$(SRC_CORE)/lulo_tune.c \
	$(SRC_CORE)/lulo_tune_backend.c \
	$(SRC_SHARED)/lulod_ipc.c

LULO_ADMIN_SRCS := \
	$(SRC_ADMIN)/lulo_admin.c \
	$(SRC_SHARED)/lulo_admin.c

LULOD_SRCS := \
	$(SRC_DAEMON)/lulod.c \
	$(SRC_DAEMON)/lulod_admin.c \
	$(SRC_CORE)/lulo_systemd.c \
	$(SRC_DAEMON)/lulod_systemd.c \
	$(SRC_CORE)/lulo_tune.c \
	$(SRC_DAEMON)/lulod_tune.c \
	$(SRC_SHARED)/lulo_admin.c \
	$(SRC_SHARED)/lulod_ipc.c

NC_INPUT_PROBE_SRCS := $(SRC_APP)/nc_input_probe.c

.PHONY: all clean install-lulod-user-service restart-lulod-user-service status-lulod-user-service install-lulo-admin-pkexec

all: lulo lulod lulo-admin nc_input_probe

lulo: $(LULO_SRCS) include/lulo_model.h include/lulo_proc.h include/lulo_dizk.h include/lulo_systemd.h include/lulo_systemd_backend.h include/lulo_tune.h include/lulo_tune_backend.h include/lulod_ipc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NC_CFLAGS) -o $@ $(LULO_SRCS) $(NC_LIBS) -lm -lpthread

lulod: $(LULOD_SRCS) include/lulo_systemd.h include/lulo_tune.h include/lulod_systemd.h include/lulod_tune.h include/lulod_ipc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LULOD_SRCS) $(SYSTEMD_LIBS) -lm

lulo-admin: $(LULO_ADMIN_SRCS) include/lulo_admin.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(LULO_ADMIN_SRCS) -lm

nc_input_probe: $(NC_INPUT_PROBE_SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NC_CFLAGS) -o $@ $(NC_INPUT_PROBE_SRCS) $(NC_LIBS)

clean:
	rm -f lulo lulod lulo-admin nc_input_probe

install-lulod-user-service: lulod
	chmod +x ./install-lulod-user-service.sh
	./install-lulod-user-service.sh

install-lulo-admin-pkexec: lulo-admin
	chmod +x ./install-lulo-admin-pkexec.sh
	./install-lulo-admin-pkexec.sh

restart-lulod-user-service:
	systemctl --user daemon-reload
	systemctl --user restart lulod.service

status-lulod-user-service:
	systemctl --user status lulod.service --no-pager --lines=20
