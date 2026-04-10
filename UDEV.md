# Lulo Udev View

The `UDEV` page is the udev inspection and configuration surface.
It is designed to give visibility into rule files, hwdb data, and live device
state from one place.

## Subviews

The page has three subviews:

- `Rules`
  - installed udev rule files
- `Hwdb`
  - installed hwdb files
- `Devices`
  - live device data gathered from udev runtime state

## Rules View

`Rules` is for browsing the file-backed udev rules that shape device handling.
These files can be opened in the external editor workflow for modification.

## Hwdb View

`Hwdb` provides the same browse/edit model for udev hwdb content.
This makes it easier to inspect or adjust device database entries without
leaving the TUI.

## Devices View

`Devices` focuses on live runtime state rather than config files.
It is intended for visibility into what udev currently knows about devices and
how they have been materialized into runtime records.

## Editing Model

`UDEV` follows the same file-backed external editor handoff used by `SCHED` and
`SYSTEMD`:

- select a rule or hwdb file
- press `i`
- edit in `$VISUAL` / `$EDITOR`
- return to `lulo` and refresh the view

Privileged commits are handled by the backend path when required.

## Current Scope

What exists today:

- browse udev rules
- browse hwdb files
- inspect live device data
- edit rule and hwdb files

What is not yet the main workflow:

- rule creation/deletion as a polished TUI-first flow
- high-level device action tooling beyond visibility and config editing

## Relation to Other Pages

`UDEV` fits alongside the other system-management pages:

- `SYSTEMD` for service/unit-oriented management
- `CGROUPS` for hierarchy/control views
- `TUNE` for kernel and pseudo-file tuning

Together they provide configuration visibility across several major Linux
subsystems without changing the basic edit model.

## See Also

- [SYSTEMD.md](SYSTEMD.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
- [INSTALL.md](INSTALL.md)
