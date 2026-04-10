# Lulo Architecture

`lulo` is split into a TUI frontend, a user/session daemon, and a privileged system daemon.

## Process Model

| Process | Scope | Responsibilities |
| --- | --- | --- |
| `lulo` | UI | Notcurses rendering, input handling, editor handoff, page orchestration |
| `lulod` | User/session | Session-facing cache/state, focus integration, frontend IPC, page snapshots |
| `lulod-system` | Privileged/system | Scheduler enforcement, privileged edit/apply operations, long-lived system policy |

### Related Helpers

| Helper | Purpose |
| --- | --- |
| `lulo-admin` | Narrow privileged helper used by the current polkit/pkexec path |
| `lulod-focus-kde` | KDE/Qt focus helper that reports the currently focused window PID |

## High-Level Data Flow

```text
lulo <-> lulod <-> lulod-system
  |        |          |
  |        |          +-> scheduler enforcement
  |        |          +-> privileged file edits
  |        |          +-> cgroup/sysfs/procfs writes
  |        |
  |        +-> session bus / focus integration
  |        +-> cached snapshots for sched/systemd/tune/cgroups/udev
  |
  +-> Notcurses UI
  +-> external editor handoff
```

## IPC Boundaries

| Boundary | Transport | Used For |
| --- | --- | --- |
| `lulo <-> lulod` | Unix socket under the user runtime dir | `SYSTEMD`, `TUNE`, `CGROUPS`, `UDEV`, scheduler snapshots, focus/session-facing state |
| `lulod <-> lulod-system` | Unix socket under `/run` | Scheduler reload/scan state, focus updates, privileged edits, file apply/delete operations |

Shared IPC code:

- `src/shared/lulod_ipc.c`
- `src/shared/lulod_system_ipc.c`

## Source Layout

| Path | Purpose |
| --- | --- |
| `src/app` | TUI shell, input handling, page rendering, widget drawing, help overlay |
| `src/core` | Shared page models and user-daemon client backends |
| `src/daemon` | `lulod`, `lulod-system`, focus helpers, privileged edit/scheduler code |
| `src/shared` | IPC, proc metadata, and shared helpers |
| `src/admin` | Narrow admin helper entrypoint |
| `include` | Public/internal headers |

## Frontend Structure

Main frontend entrypoint:

- `src/app/lulo.c`

### Page Modules

| Page | App Module(s) | Core Module(s) | Subviews |
| --- | --- | --- | --- |
| CPU / proc | main app shell | `src/core/lulo_model.c`, `src/core/lulo_proc.c` | main CPU + proc tree |
| DISK | `src/app/lulo_widgets.c` | `src/core/lulo_dizk.c` | n/a |
| SCHED | `src/app/lulo_sched_page.c` | `src/core/lulo_sched.c`, `src/core/lulo_sched_backend.c` | `Profiles`, `Rules`, `Live` |
| CGROUPS | `src/app/lulo_cgroups_page.c` | `src/core/lulo_cgroups.c`, `src/core/lulo_cgroups_backend.c` | `Tree`, `Files`, `Config` |
| SYSTEMD | `src/app/lulo_systemd_page.c` | `src/core/lulo_systemd.c`, `src/core/lulo_systemd_backend.c` | `Services`, `Deps`, `Config` |
| UDEV | `src/app/lulo_udev_page.c` | `src/core/lulo_udev.c`, `src/core/lulo_udev_backend.c` | `Rules`, `Hwdb`, `Devices` |
| TUNE | `src/app/lulo_tune_page.c` | `src/core/lulo_tune.c`, `src/core/lulo_tune_backend.c` | `Explore`, `Snapshots`, `Presets` |

Additional frontend files:

| File | Purpose |
| --- | --- |
| `src/app/lulo_input.c` | Input decoding and input-mode-specific handling |
| `include/lulo_app.h` | Shared frontend types and app state |

## Daemon Responsibilities

### `lulod`

Key files:

- `src/daemon/lulod.c`
- `src/daemon/lulod_systemd.c`
- `src/daemon/lulod_tune.c`
- `src/daemon/lulod_cgroups.c`
- `src/daemon/lulod_udev.c`
- `src/daemon/lulod_sched.c`
- `src/daemon/lulod_focus.c`

Responsibilities:

| Responsibility | Notes |
| --- | --- |
| Maintain user-facing snapshots | Covers `SCHED`, `SYSTEMD`, `TUNE`, `CGROUPS`, and `UDEV` |
| Focus integration | Currently KDE focus reporting |
| Proxy system work | For scheduler/focus requests and privileged operations |
| Support editor flows | Handles file-backed page editing before privileged commit |
| Keep the UI responsive | Moves blocking work out of `lulo` |

### `lulod-system`

Key files:

- `src/daemon/lulod_system.c`
- `src/daemon/lulod_system_sched.c`
- `src/daemon/lulod_system_edit.c`

Responsibilities:

| Responsibility | Notes |
| --- | --- |
| Load scheduler config | Reads `/etc/lulo/scheduler` |
| Enforce scheduler policy | Continuous `/proc` scan and policy application |
| Apply focused profile | Consumes focused PID updates from `lulod` |
| Perform privileged edit sessions | Direct writes, deletes, and commit back to system files |
| Handle system-managed files | Covers systemd, cgroup, udev, and related system config |

## Scheduler Design

The scheduler intentionally mixes file-backed policy with dynamic built-ins.

### File-Backed State

| Path | Purpose |
| --- | --- |
| `/etc/lulo/scheduler/profiles.d` | Profile definitions |
| `/etc/lulo/scheduler/rules.d` | Rule definitions |
| `/etc/lulo/scheduler/scheduler.conf` | Global scheduler settings |

### Dynamic Behavior

| Built-In | Purpose |
| --- | --- |
| `focused` | Runtime-detected focused app override |
| `background` | Runtime-applied fallback for unhandled app-scope processes |

Current scheduler state includes:

- Linux scheduling policy
- RT priority
- nice
- I/O priority
- focused/background dynamic behavior

## Editing Model

| Pattern | Used For |
| --- | --- |
| Inline value editing | Mainly `TUNE -> Explore` and pseudo-files that are not normal config documents |
| External editor handoff | Scheduler config, systemd config, service files, udev rules, hwdb files, tune snapshots/presets, and other file-backed content |

Privileged file edits go through `lulod-system`, so the TUI never writes system files directly.

## Runtime and Installed Paths

| Path | Contents |
| --- | --- |
| `/usr/bin` | `lulo`, `lulod`, `lulod-system` |
| `/usr/libexec/lulo` | Helper binaries |
| `/usr/share/lulo` | Shipped data and examples |
| `/usr/lib/systemd/user` | `lulod.service` |
| `/usr/lib/systemd/system` | `lulod-system.service` |
| `/usr/share/polkit-1/actions` | `lulo` policy files |
| `/etc/lulo` | System config |

The build also supports running directly from the repo checkout for development.

## Design Rules

- Keep rendering and terminal handling in `lulo`.
- Keep session integration and cache orchestration in `lulod`.
- Keep privileged writes and long-lived enforcement in `lulod-system`.
- Prefer visible, editable policy over hidden hardcoded behavior.
- Use file-backed config plus runtime dynamic application, not opaque daemon-only state.
