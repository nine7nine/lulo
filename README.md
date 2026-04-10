# lulo

`lulo` is a terminal-based Linux management and observability tool built around three processes:

- `lulo`: the Notcurses TUI frontend
- `lulod`: the user/session daemon
- `lulod-system`: the privileged system daemon

Current areas include:

- CPU, process, and disk inspection
- scheduler policy management
- systemd inspection and editing
- tunables management
- cgroup inspection and editing
- udev inspection and editing

## Docs

- [INSTALL.md](INSTALL.md): build, install, update, and packaging flow
- [ARCHITECTURE.md](ARCHITECTURE.md): process split, source layout, and design notes
- [CPU.md](CPU.md): CPU sampling and process-tree behavior
- [DISK.md](DISK.md): filesystem usage view
- [SCHED.md](SCHED.md): scheduler model, config layout, and focused-app behavior
- [CGROUPS.md](CGROUPS.md): cgroup hierarchy, files, and related config
- [SYSTEMD.md](SYSTEMD.md): service, dependency, and config inspection
- [TUNE.md](TUNE.md): tunables explorer, snapshots, and presets
- [UDEV.md](UDEV.md): udev rules, hwdb, and live device inspection

## Build

```bash
make PREFIX=/usr
```

Useful build variants:

- `make strict PREFIX=/usr`
- `make analyze PREFIX=/usr`
- `make asan PREFIX=/usr`

## Install

```bash
sudo make install PREFIX=/usr
```

Then start or restart the daemons and run the UI:

```bash
sudo systemctl restart lulod-system.service
systemctl --user restart lulod.service
/usr/bin/lulo -i nc
```

See [INSTALL.md](INSTALL.md) for the full setup and migration steps.
