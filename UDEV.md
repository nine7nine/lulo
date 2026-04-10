# Lulo Udev View

The `UDEV` page is the udev inspection and configuration surface. It is designed to give visibility into rule files, hwdb data, and live device state from one place.

## Subviews

| Subview | Purpose |
| --- | --- |
| `Rules` | Installed udev rule files |
| `Hwdb` | Installed hwdb files |
| `Devices` | Live device data gathered from udev runtime state |

## Rules View

| Focus | Notes |
| --- | --- |
| Rule browsing | Inspect file-backed udev rules |
| Editor integration | Open rules in the shared external-editor flow |
| Config visibility | See how device handling is defined |

## Hwdb View

| Focus | Notes |
| --- | --- |
| hwdb browsing | Inspect installed hardware database files |
| Editor integration | Edit hwdb content through the same workflow |
| Device mapping context | Understand how database content informs device handling |

## Devices View

| Focus | Notes |
| --- | --- |
| Runtime state | Inspect live udev-recorded device data |
| Visibility | See what udev currently knows about devices |
| Record inspection | View runtime records rather than only config files |

## Editing Model

`UDEV` follows the same file-backed external editor handoff used by `SCHED` and `SYSTEMD`.

| Step | Result |
| --- | --- |
| Select a rule or hwdb file | Choose the file-backed item |
| Press `i` | Open `$VISUAL` / `$EDITOR` |
| Save and exit | Return to `lulo` |
| Privileged commit | Handled by the backend path when required |

## Current Scope

| What Exists Today | What Is Not Yet the Main Workflow |
| --- | --- |
| Browse udev rules | Rule creation/deletion as a polished TUI-first flow |
| Browse hwdb files | High-level device action tooling beyond visibility and config editing |
| Inspect live device data | Full udevadm replacement behavior |
| Edit rule and hwdb files |  |

## Relation to Other Pages

| Related Page | Why it matters |
| --- | --- |
| [SYSTEMD.md](SYSTEMD.md) | Service/unit-oriented management |
| [CGROUPS.md](CGROUPS.md) | Hierarchy/control views |
| [TUNE.md](TUNE.md) | Kernel and pseudo-file tuning |

Together these pages provide configuration visibility across several major Linux subsystems without changing the basic edit model.

## See Also

- [SYSTEMD.md](SYSTEMD.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
- [INSTALL.md](INSTALL.md)
