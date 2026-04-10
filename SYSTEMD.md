# Lulo Systemd View

The `SYSTEMD` page is the systemd/service inspection and editing surface.
It is designed for visibility first, with safe file-backed editing before more
active service-management actions are added.

## Subviews

The page has three subviews:

- `Services`
  - loaded system and user service/unit inventory
- `Deps`
  - reverse dependency view for the selected service
- `Config`
  - related config and drop-in files

## Data Flow

`lulo` does not shell out to `systemctl` directly.
The page is fed by `lulod`, which maintains the user-facing snapshot and keeps
preview work out of the UI thread.

This keeps the UI responsive even when systemd inspection is relatively heavy.

## Services View

`Services` is the main inventory view.
It shows:

- unit name
- scope
- active state
- substate
- file state
- enablement / preset data where available

The right-hand side can preview the currently opened service, including its unit
file content and basic metadata.

## Dependency View

`Deps` focuses on reverse dependencies for the selected service.
That makes it easier to understand what would be affected by changing or
removing a unit.

## Config View

`Config` shows file-backed configuration related to systemd, including:

- service files
- config files under systemd-managed paths
- drop-ins and related overrides

These are editable through the shared external-editor workflow.

## Editing Model

`SYSTEMD` uses the same editor handoff model as other file-backed pages:

- select an item
- press `i`
- `lulo` opens `$VISUAL` / `$EDITOR`
- on save/exit, the change is committed back through the privileged path when needed

This covers both system files and user-writable files without requiring the TUI
itself to run as root.

## Current Scope

What exists today:

- browse units
- inspect reverse dependencies
- inspect/edit unit and config files

What is intentionally not the primary focus yet:

- broad service lifecycle management from inside the TUI
- start/stop/restart enable/disable as a polished end-user workflow

The page is currently stronger as an inspection/configuration surface than as a
full service-control panel.

## See Also

- [ARCHITECTURE.md](ARCHITECTURE.md)
- [CGROUPS.md](CGROUPS.md)
- [UDEV.md](UDEV.md)
