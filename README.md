# lulo

`lulo` is a terminal-based Linux management and observability tool built around a three-process architecture: a Notcurses frontend, a user/session daemon, and a privileged system daemon.

**[View Documentation](https://nine7nine.github.io/lulo/)** &mdash; design, architecture, and per-component technical docs.

---

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

## Documentation

Full documentation is published at **[nine7nine.github.io/lulo](https://nine7nine.github.io/lulo/)**.

### Architecture & Design

| Document | Description |
| --- | --- |
| [Architecture Overview](https://nine7nine.github.io/lulo/architecture.gen.html) | The three processes, the privilege boundary, source layout, and design rules. |
| [Process Model & IPC](https://nine7nine.github.io/lulo/process-model-ipc.gen.html) | The two sockets, wire protocol, the embedded polkit auth agent, the RW lease, edit sessions, and proc tracing. |
| [Scheduler](https://nine7nine.github.io/lulo/scheduler.gen.html) | Sparse-overlay profiles, fnmatch matchers, policy resolution, and continuous syscall-level enforcement. |
| [Focus Providers](https://nine7nine.github.io/lulo/focus-providers.gen.html) | KDE and GNOME focused-window detection over one session-bus contract. |

### TUI Pages

| Document | Description |
| --- | --- |
| [CPU & Process View](https://nine7nine.github.io/lulo/cpu.gen.html) | Decoupled CPU/proc sampling, the per-core heatmap, the arena-built process tree, and tracing. |
| [Disk View](https://nine7nine.github.io/lulo/disk.gen.html) | A daemon-free multi-panel dashboard over `/proc`, `/sys/block`, and the mount tables. |
| [Cgroups View](https://nine7nine.github.io/lulo/cgroups.gen.html) | A live `/sys/fs/cgroup` browser bridged to the systemd units that shape each limit. |
| [Systemd View](https://nine7nine.github.io/lulo/systemd.gen.html) | A merged system + user inventory over sd-bus, with reverse-dependency closures. |
| [Tune View](https://nine7nine.github.io/lulo/tune.gen.html) | A tunables explorer with staged edits, portable `.ltune` bundles, and a privileged apply path. |
| [Udev View](https://nine7nine.github.io/lulo/udev.gen.html) | Rules, hwdb, and live device records read straight from `/run/udev/data`. |

### Build & Operations

| Document | Description |
| --- | --- |
| [Install & Packaging](https://nine7nine.github.io/lulo/install.gen.html) | Build targets, the `/usr` footprint, the systemd units, the GNOME extension step, and packaging. |

The repository root also keeps short Markdown references for quick in-repo reading: [ARCHITECTURE.md](ARCHITECTURE.md), [SCHED.md](SCHED.md), [INSTALL.md](INSTALL.md), [CPU.md](CPU.md), [DISK.md](DISK.md), [CGROUPS.md](CGROUPS.md), [SYSTEMD.md](SYSTEMD.md), [TUNE.md](TUNE.md), [UDEV.md](UDEV.md).

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
