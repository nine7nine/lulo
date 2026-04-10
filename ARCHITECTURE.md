# Lulo Architecture

`lulo` is split into a TUI frontend, a user/session daemon, and a privileged system daemon.

## Process Model

- `lulo`
  - Notcurses TUI frontend.
  - Renders pages, handles input, launches external editors, and talks to the backends.
- `lulod`
  - Unprivileged user daemon.
  - Owns session-facing cache/state, focus integration, and frontend IPC.
  - Proxies privileged or system-wide operations toward `lulod-system`.
- `lulod-system`
  - Privileged system daemon.
  - Owns root-level scheduling enforcement, privileged edit/apply operations, and long-lived system policy.

Related helper binaries:

- `lulo-admin`
  - Narrow privileged helper used by the current polkit/pkexec path.
  - Still present for specific privileged operations, but the long-term system-management role belongs to `lulod-system`.
- `lulod-focus-kde`
  - KDE/Qt focus helper used by `lulod` to detect the currently focused window and report its PID.

## High-Level Data Flow

```text
lulo <-> lulod <-> lulod-system
  |        |          |
  |        |          +-> scheduler enforcement
  |        |          +-> privileged file edits
  |        |          +-> cgroup/sysfs/procfs writes
  |        |
  |        +-> session bus / focus integration
  |        +-> cached snapshots for pages
  |
  +-> Notcurses UI
  +-> external editor handoff
```

General model:

- `lulo` should stay UI-only.
- `lulod` should stay unprivileged and session-oriented.
- `lulod-system` should own privileged and long-running system behavior.

## IPC Boundaries

- `lulo <-> lulod`
  - Unix socket under the user runtime dir.
  - Used for `SYSTEMD`, `TUNE`, `CGROUPS`, scheduler snapshots, and focus/session-facing state.
- `lulod <-> lulod-system`
  - Unix socket under `/run`.
  - Used for scheduler reload/scan state, focus updates, privileged edits, and file apply/delete operations.

Shared IPC code lives in:

- `src/shared/lulod_ipc.c`
- `src/shared/lulod_system_ipc.c`

## Source Layout

- `src/app`
  - TUI shell, input handling, page rendering, widget drawing.
- `src/core`
  - Shared page models and user-daemon client backends.
- `src/daemon`
  - `lulod`, `lulod-system`, focus helpers, privileged edit/scheduler code.
- `src/shared`
  - IPC, proc metadata, and helper code shared across binaries.
- `src/admin`
  - Narrow admin helper entrypoint.
- `include`
  - Public/internal headers for all modules.

## Frontend Structure

Main frontend entrypoint:

- `src/app/lulo.c`

Page modules:

- CPU / proc view: main app shell + `src/core/lulo_model.c` + `src/core/lulo_proc.c`
- DISK: `src/app/lulo_widgets.c` + `src/core/lulo_dizk.c`
- SCHED: `src/app/lulo_sched_page.c`
- CGROUPS: `src/app/lulo_cgroups_page.c`
- SYSTEMD: `src/app/lulo_systemd_page.c`
- TUNE: `src/app/lulo_tune_page.c`

Input handling:

- `src/app/lulo_input.c`

Shared frontend types:

- `include/lulo_app.h`

## Daemon Responsibilities

### `lulod`

Key files:

- `src/daemon/lulod.c`
- `src/daemon/lulod_systemd.c`
- `src/daemon/lulod_tune.c`
- `src/daemon/lulod_cgroups.c`
- `src/daemon/lulod_sched.c`
- `src/daemon/lulod_focus.c`

Responsibilities:

- maintain user-facing snapshots for UI pages
- integrate with KDE focus reporting
- proxy scheduler/focus requests to `lulod-system`
- keep the frontend responsive by moving blocking work out of `lulo`

### `lulod-system`

Key files:

- `src/daemon/lulod_system.c`
- `src/daemon/lulod_system_sched.c`
- `src/daemon/lulod_system_edit.c`

Responsibilities:

- load scheduler config from `/etc/lulo/scheduler`
- continuously scan and enforce scheduler policy
- receive focused-PID updates and apply the `focused` profile dynamically
- perform privileged edit sessions and direct file writes/deletes

## Scheduler Design

The scheduler is intentionally split into:

- file-backed profiles and rules
- dynamic built-ins

File-backed state:

- profiles in `/etc/lulo/scheduler/profiles.d`
- rules in `/etc/lulo/scheduler/rules.d`
- globals in `/etc/lulo/scheduler/scheduler.conf`

Dynamic behavior:

- `focused`
  - always runtime-detected
  - target is whichever app currently owns focus
- `background`
  - runtime-applied fallback
  - classifier is configurable and exposed in the UI/config

Current scheduler state includes:

- Linux scheduling policy
- RT priority
- nice
- I/O priority
- focused/background dynamic behavior

## Editing Model

There are two editing patterns:

- inline value editing
  - mainly for `TUNE -> Explore`
  - used for pseudo-files that are not normal config documents
- external editor handoff
  - used for scheduler config, systemd config, service files, snapshots/presets, and other file-backed content
  - honors `$VISUAL` / `$EDITOR`

Privileged file edits go through `lulod-system` so the TUI never writes system files directly.

## Runtime and Installed Paths

Installed `/usr` layout:

- `/usr/bin`: `lulo`, `lulod`, `lulod-system`
- `/usr/libexec/lulo`: helper binaries
- `/usr/share/lulo`: shipped data and examples
- `/etc/lulo`: system config

The build also still supports running from the repo checkout for development.

## Design Rules

- Keep rendering and terminal handling in `lulo`.
- Keep session integration and cache orchestration in `lulod`.
- Keep privileged writes and long-lived enforcement in `lulod-system`.
- Prefer visible, editable policy over hidden hardcoded behavior.
- Use file-backed config plus runtime dynamic application, not opaque daemon-only state.
