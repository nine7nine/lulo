CC ?= gcc
CXX ?= g++
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS ?= -Iinclude
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
LULO_HELPERDIR ?= $(LIBEXECDIR)/lulo
DATADIR ?= $(PREFIX)/share
LULO_DATADIR ?= $(DATADIR)/lulo
SYSCONFDIR ?= /etc
LULO_SYSCONFDIR ?= $(SYSCONFDIR)/lulo
SCHED_CONFIG_DIR ?= $(LULO_SYSCONFDIR)/scheduler
SYSTEMD_USER_UNITDIR ?= /usr/lib/systemd/user
SYSTEMD_USER_UNITDIR ?= /usr/lib/systemd/user
SYSTEMD_SYSTEM_UNITDIR ?= /usr/lib/systemd/system
POLKIT_ACTIONSDIR ?= /usr/share/polkit-1/actions
LULO_PATH_CPPFLAGS := -DLULO_HELPERDIR=\"$(LULO_HELPERDIR)\" -DLULO_DATADIR=\"$(LULO_DATADIR)\" -DLULO_SYSCONFDIR=\"$(SYSCONFDIR)\"
NC_CFLAGS := $(shell $(PKG_CONFIG) --cflags notcurses-core)
NC_LIBS := $(shell $(PKG_CONFIG) --libs notcurses-core)
SYSTEMD_LIBS := $(shell $(PKG_CONFIG) --libs libsystemd)
STRICT_WARNINGS := -Wall -Wextra -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wpointer-arith -Wcast-qual -Wwrite-strings -Wundef -Wvla
STRICT_CXXWARNINGS := -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith -Wcast-qual -Wwrite-strings -Wundef -Wvla
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
QT6_HOST_LIBEXECS := $(shell qmake6 -query QT_HOST_LIBEXECS 2>/dev/null)
FOCUS_KDE_MOC := $(if $(QT6_HOST_LIBEXECS),$(QT6_HOST_LIBEXECS)/moc,moc)
FOCUS_KDE_CFLAGS := $(shell $(PKG_CONFIG) --cflags Qt6Core Qt6DBus)
FOCUS_KDE_LIBS := $(shell $(PKG_CONFIG) --libs Qt6Core Qt6DBus)
FOCUS_KDE_PIC := -fPIC

SRC_APP := src/app
SRC_ADMIN := src/admin
SRC_CORE := src/core
SRC_DAEMON := src/daemon
SRC_SHARED := src/shared

LULO_SRCS := \
	$(SRC_APP)/lulo.c \
	$(SRC_APP)/lulo_input.c \
	$(SRC_APP)/lulo_widgets.c \
	$(SRC_APP)/lulo_sched_page.c \
	$(SRC_APP)/lulo_cgroups_page.c \
	$(SRC_APP)/lulo_udev_page.c \
	$(SRC_APP)/lulo_systemd_page.c \
	$(SRC_APP)/lulo_tune_page.c \
	$(SRC_CORE)/lulo_model.c \
	$(SRC_CORE)/lulo_proc.c \
	$(SRC_CORE)/lulo_dizk.c \
	$(SRC_CORE)/lulo_edit.c \
	$(SRC_CORE)/lulo_cgroups.c \
	$(SRC_CORE)/lulo_cgroups_backend.c \
	$(SRC_CORE)/lulo_udev.c \
	$(SRC_CORE)/lulo_udev_backend.c \
	$(SRC_CORE)/lulo_sched.c \
	$(SRC_CORE)/lulo_sched_backend.c \
	$(SRC_CORE)/lulo_systemd.c \
	$(SRC_CORE)/lulo_systemd_backend.c \
	$(SRC_CORE)/lulo_tune.c \
	$(SRC_CORE)/lulo_tune_backend.c \
	$(SRC_SHARED)/lulod_system_ipc.c \
	$(SRC_SHARED)/lulod_ipc.c

LULO_ADMIN_SRCS := \
	$(SRC_ADMIN)/lulo_admin.c \
	$(SRC_SHARED)/lulo_admin.c

LULOD_SRCS := \
	$(SRC_DAEMON)/lulod.c \
	$(SRC_DAEMON)/lulod_focus.c \
	$(SRC_DAEMON)/lulod_admin.c \
	$(SRC_CORE)/lulo_cgroups.c \
	$(SRC_CORE)/lulo_udev.c \
	$(SRC_DAEMON)/lulod_sched.c \
	$(SRC_DAEMON)/lulod_cgroups.c \
	$(SRC_DAEMON)/lulod_udev.c \
	$(SRC_CORE)/lulo_sched.c \
	$(SRC_CORE)/lulo_systemd.c \
	$(SRC_DAEMON)/lulod_systemd.c \
	$(SRC_CORE)/lulo_tune.c \
	$(SRC_DAEMON)/lulod_tune.c \
	$(SRC_SHARED)/lulo_admin.c \
	$(SRC_SHARED)/lulo_proc_meta.c \
	$(SRC_SHARED)/lulod_system_ipc.c \
	$(SRC_SHARED)/lulod_ipc.c

LULOD_SYSTEM_SRCS := \
	$(SRC_DAEMON)/lulod_system.c \
	$(SRC_DAEMON)/lulod_system_edit.c \
	$(SRC_DAEMON)/lulod_system_sched.c \
	$(SRC_CORE)/lulo_sched.c \
	$(SRC_SHARED)/lulo_proc_meta.c \
	$(SRC_SHARED)/lulod_system_ipc.c

NC_INPUT_PROBE_SRCS := $(SRC_APP)/nc_input_probe.c
LULOD_FOCUS_KDE_SRCS := $(SRC_DAEMON)/lulod_focus_kde.cpp
LULOD_FOCUS_KDE_MOC := $(SRC_DAEMON)/lulod_focus_kde.moc

.PHONY: all clean strict analyze asan install install-bin install-data install-units install-policy install-config print-install-paths install-lulod-user-service restart-lulod-user-service status-lulod-user-service install-lulo-admin-pkexec install-lulod-system-service reset-lulod-system-config

all: lulo lulod lulod-system lulo-admin nc_input_probe lulod-focus-kde

strict:
	$(MAKE) -B CFLAGS="-O2 $(STRICT_WARNINGS)" CXXFLAGS="-O2 $(STRICT_CXXWARNINGS)" all

analyze:
	$(MAKE) -B CFLAGS="-O0 -Wall -Wextra -fanalyzer" CXXFLAGS="-O0 -Wall -Wextra" all

asan: lulo-asan lulod-asan lulod-system-asan lulo-admin-asan nc_input_probe-asan lulod-focus-kde-asan

lulo: $(LULO_SRCS) include/lulo_edit.h include/lulo_model.h include/lulo_proc.h include/lulo_dizk.h include/lulo_cgroups.h include/lulo_cgroups_backend.h include/lulo_sched.h include/lulo_sched_backend.h include/lulo_systemd.h include/lulo_systemd_backend.h include/lulo_tune.h include/lulo_tune_backend.h include/lulo_udev.h include/lulo_udev_backend.h include/lulod_ipc.h include/lulod_system_ipc.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(CFLAGS) $(NC_CFLAGS) -o $@ $(LULO_SRCS) $(NC_LIBS) -lm -lpthread

lulod: $(LULOD_SRCS) include/lulo_cgroups.h include/lulo_sched.h include/lulo_systemd.h include/lulo_tune.h include/lulo_udev.h include/lulod_cgroups.h include/lulod_sched.h include/lulod_systemd.h include/lulod_tune.h include/lulod_udev.h include/lulod_system_ipc.h include/lulod_ipc.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(CFLAGS) -o $@ $(LULOD_SRCS) $(SYSTEMD_LIBS) -lm

lulod-system: $(LULOD_SYSTEM_SRCS) include/lulo_sched.h include/lulod_system_edit.h include/lulod_system_ipc.h include/lulod_system_sched.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(CFLAGS) -o $@ $(LULOD_SYSTEM_SRCS) -lm

lulo-admin: $(LULO_ADMIN_SRCS) include/lulo_admin.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(CFLAGS) -o $@ $(LULO_ADMIN_SRCS) -lm

nc_input_probe: $(NC_INPUT_PROBE_SRCS)
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(CFLAGS) $(NC_CFLAGS) -o $@ $(NC_INPUT_PROBE_SRCS) $(NC_LIBS)

$(LULOD_FOCUS_KDE_MOC): $(LULOD_FOCUS_KDE_SRCS)
	$(FOCUS_KDE_MOC) $< -o $@

lulod-focus-kde: $(LULOD_FOCUS_KDE_SRCS) $(LULOD_FOCUS_KDE_MOC)
	$(CXX) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(CXXFLAGS) $(FOCUS_KDE_PIC) $(FOCUS_KDE_CFLAGS) -o $@ $(LULOD_FOCUS_KDE_SRCS) $(FOCUS_KDE_LIBS)

lulo-asan: $(LULO_SRCS) include/lulo_edit.h include/lulo_model.h include/lulo_proc.h include/lulo_dizk.h include/lulo_cgroups.h include/lulo_cgroups_backend.h include/lulo_sched.h include/lulo_sched_backend.h include/lulo_systemd.h include/lulo_systemd_backend.h include/lulo_tune.h include/lulo_tune_backend.h include/lulo_udev.h include/lulo_udev_backend.h include/lulod_ipc.h include/lulod_system_ipc.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(STRICT_WARNINGS) $(ASAN_FLAGS) $(NC_CFLAGS) -o $@ $(LULO_SRCS) $(NC_LIBS) -lm -lpthread

lulod-asan: $(LULOD_SRCS) include/lulo_cgroups.h include/lulo_sched.h include/lulo_systemd.h include/lulo_tune.h include/lulo_udev.h include/lulod_cgroups.h include/lulod_sched.h include/lulod_systemd.h include/lulod_tune.h include/lulod_udev.h include/lulod_system_ipc.h include/lulod_ipc.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(STRICT_WARNINGS) $(ASAN_FLAGS) -o $@ $(LULOD_SRCS) $(SYSTEMD_LIBS) -lm

lulod-system-asan: $(LULOD_SYSTEM_SRCS) include/lulo_sched.h include/lulod_system_edit.h include/lulod_system_ipc.h include/lulod_system_sched.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(STRICT_WARNINGS) $(ASAN_FLAGS) -o $@ $(LULOD_SYSTEM_SRCS) -lm

lulo-admin-asan: $(LULO_ADMIN_SRCS) include/lulo_admin.h
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(STRICT_WARNINGS) $(ASAN_FLAGS) -o $@ $(LULO_ADMIN_SRCS) -lm

nc_input_probe-asan: $(NC_INPUT_PROBE_SRCS)
	$(CC) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(STRICT_WARNINGS) $(ASAN_FLAGS) $(NC_CFLAGS) -o $@ $(NC_INPUT_PROBE_SRCS) $(NC_LIBS)

lulod-focus-kde-asan: $(LULOD_FOCUS_KDE_SRCS) $(LULOD_FOCUS_KDE_MOC)
	$(CXX) $(CPPFLAGS) $(LULO_PATH_CPPFLAGS) $(STRICT_CXXWARNINGS) $(ASAN_FLAGS) $(FOCUS_KDE_PIC) $(FOCUS_KDE_CFLAGS) -o $@ $(LULOD_FOCUS_KDE_SRCS) $(FOCUS_KDE_LIBS)

print-install-paths:
	@printf 'PREFIX=%s\n' "$(PREFIX)"
	@printf 'BINDIR=%s\n' "$(BINDIR)"
	@printf 'LULO_HELPERDIR=%s\n' "$(LULO_HELPERDIR)"
	@printf 'LULO_DATADIR=%s\n' "$(LULO_DATADIR)"
	@printf 'SYSTEMD_USER_UNITDIR=%s\n' "$(SYSTEMD_USER_UNITDIR)"
	@printf 'SYSTEMD_SYSTEM_UNITDIR=%s\n' "$(SYSTEMD_SYSTEM_UNITDIR)"
	@printf 'POLKIT_ACTIONSDIR=%s\n' "$(POLKIT_ACTIONSDIR)"
	@printf 'SCHED_CONFIG_DIR=%s\n' "$(SCHED_CONFIG_DIR)"

install: all install-bin install-data install-units install-policy install-config

install-bin: all
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LULO_HELPERDIR)"
	install -m755 lulo lulod lulod-system "$(DESTDIR)$(BINDIR)/"
	install -m755 lulo-admin lulod-focus-kde "$(DESTDIR)$(LULO_HELPERDIR)/"

install-data:
	install -d "$(DESTDIR)$(LULO_DATADIR)/kwin" \
		"$(DESTDIR)$(LULO_DATADIR)/examples/scheduler/profiles.d" \
		"$(DESTDIR)$(LULO_DATADIR)/examples/scheduler/rules.d"
	install -m644 share/lulo/kwin/lulod_focus_kde.js "$(DESTDIR)$(LULO_DATADIR)/kwin/"
	install -m644 examples/scheduler/README.md "$(DESTDIR)$(LULO_DATADIR)/examples/scheduler/"
	install -m644 examples/scheduler/scheduler.conf "$(DESTDIR)$(LULO_DATADIR)/examples/scheduler/"
	for f in examples/scheduler/profiles.d/*.conf; do install -m644 "$$f" "$(DESTDIR)$(LULO_DATADIR)/examples/scheduler/profiles.d/"; done
	for f in examples/scheduler/rules.d/*.conf; do install -m644 "$$f" "$(DESTDIR)$(LULO_DATADIR)/examples/scheduler/rules.d/"; done

install-units:
	install -d "$(DESTDIR)$(SYSTEMD_USER_UNITDIR)" "$(DESTDIR)$(SYSTEMD_SYSTEM_UNITDIR)"
	sed \
		-e "s|@EXEC_START@|$(BINDIR)/lulod|g" \
		-e "s|@WORKDIR@|$(LULO_DATADIR)|g" \
		lulod.service.in >"$(DESTDIR)$(SYSTEMD_USER_UNITDIR)/lulod.service"
	sed \
		-e "s|@EXEC_START@|$(BINDIR)/lulod-system|g" \
		-e "s|@WORKDIR@|$(LULO_DATADIR)|g" \
		lulod-system.service.in >"$(DESTDIR)$(SYSTEMD_SYSTEM_UNITDIR)/lulod-system.service"

install-policy:
	install -d "$(DESTDIR)$(POLKIT_ACTIONSDIR)"
	sed -e "s|@EXEC_START@|$(LULO_HELPERDIR)/lulo-admin|g" \
		lulo-admin.policy.in >"$(DESTDIR)$(POLKIT_ACTIONSDIR)/io.lulo.admin.policy"

install-config:
	install -d "$(DESTDIR)$(SCHED_CONFIG_DIR)/profiles.d" "$(DESTDIR)$(SCHED_CONFIG_DIR)/rules.d"
	@if [ ! -e "$(DESTDIR)$(SCHED_CONFIG_DIR)/scheduler.conf" ]; then \
		install -m644 examples/scheduler/scheduler.conf "$(DESTDIR)$(SCHED_CONFIG_DIR)/scheduler.conf"; \
	fi
	@for f in examples/scheduler/profiles.d/*.conf; do \
		base="$$(basename "$$f")"; \
		dst="$(DESTDIR)$(SCHED_CONFIG_DIR)/profiles.d/$$base"; \
		if [ ! -e "$$dst" ]; then install -m644 "$$f" "$$dst"; fi; \
	done
	@for f in examples/scheduler/rules.d/*.conf; do \
		base="$$(basename "$$f")"; \
		dst="$(DESTDIR)$(SCHED_CONFIG_DIR)/rules.d/$$base"; \
		if [ ! -e "$$dst" ]; then install -m644 "$$f" "$$dst"; fi; \
	done

clean:
	rm -f lulo lulod lulod-system lulo-admin nc_input_probe lulod-focus-kde lulo-asan lulod-asan lulod-system-asan lulo-admin-asan nc_input_probe-asan lulod-focus-kde-asan $(LULOD_FOCUS_KDE_MOC)

install-lulod-user-service: lulod
	chmod +x ./install-lulod-user-service.sh
	./install-lulod-user-service.sh

install-lulo-admin-pkexec: lulo-admin
	chmod +x ./install-lulo-admin-pkexec.sh
	./install-lulo-admin-pkexec.sh

install-lulod-system-service: lulod-system
	chmod +x ./install-lulod-system-service.sh
	./install-lulod-system-service.sh

reset-lulod-system-config:
	chmod +x ./reset-lulod-system-config.sh
	./reset-lulod-system-config.sh

restart-lulod-user-service:
	systemctl --user daemon-reload
	systemctl --user restart lulod.service

status-lulod-user-service:
	systemctl --user status lulod.service --no-pager --lines=20
