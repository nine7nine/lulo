# lulo

`lulo` is a terminal-based Linux management and observability tool built around a three-process architecture: a Notcurses frontend, a user/session daemon, and a privileged system daemon.

## Components

| Component | Role |
| --- | --- |
| `lulo` | Notcurses TUI frontend |
| `lulod` | User/session daemon for cached snapshots, focus integration, and frontend IPC |
| `lulod-system` | Privileged system daemon for scheduler enforcement and privileged file/apply operations |

## Feature Areas

| Area | What it covers |
| --- | --- |
| CPU | CPU history, per-core state, process tree, process signaling |
| DISK | Mounted filesystem usage dashboard |
| SCHED | Scheduler profiles, rules, live state, focused/background policy |
| CGROUPS | Cgroup hierarchy, control files, related config |
| SYSTEMD | Units, reverse dependencies, config and unit editing |
| TUNE | Tunables explorer, snapshots, presets, privileged apply |
| UDEV | Rules, hwdb, live device data, config editing |

## Docs

| Doc | Purpose |
| --- | --- |
| [INSTALL.md](INSTALL.md) | Build, install, update, and packaging flow |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Process split, source layout, IPC, and design rules |
| [CPU.md](CPU.md) | CPU sampling and process-tree behavior |
| [DISK.md](DISK.md) | Filesystem usage view |
| [SCHED.md](SCHED.md) | Scheduler model, config layout, and focused-app behavior |
| [CGROUPS.md](CGROUPS.md) | Cgroup hierarchy, files, and related config |
| [SYSTEMD.md](SYSTEMD.md) | Service, dependency, and config inspection |
| [TUNE.md](TUNE.md) | Tunables explorer, snapshots, and presets |
| [UDEV.md](UDEV.md) | Udev rules, hwdb, and live device inspection |

## Build

```bash
make PREFIX=/usr
```

Useful variants:

| Target | Purpose |
| --- | --- |
| `make strict PREFIX=/usr` | Build with stricter warnings |
| `make analyze PREFIX=/usr` | Static analysis pass |
| `make asan PREFIX=/usr` | Sanitizer build |

## Install

```bash
sudo make install PREFIX=/usr
sudo systemctl restart lulod-system.service
systemctl --user restart lulod.service
/usr/bin/lulo -i nc
```

For full setup, migration, and packaging details, see [INSTALL.md](INSTALL.md).
