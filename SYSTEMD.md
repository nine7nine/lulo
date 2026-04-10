# Lulo Systemd View

The `SYSTEMD` page is the systemd/service inspection and editing surface. It is designed for visibility first, with safe file-backed editing before broader service-management actions are added.

## Subviews

| Subview | Purpose |
| --- | --- |
| `Services` | Loaded system and user service/unit inventory |
| `Deps` | Reverse dependency view for the selected service |
| `Config` | Related config and drop-in files |

## Data Flow

| Layer | Role |
| --- | --- |
| `lulo` | Renders the page and handles interaction |
| `lulod` | Maintains the user-facing snapshot and keeps preview work off the UI thread |
| `lulod-system` | Handles privileged file editing/commit when needed |

This keeps the UI responsive even when systemd inspection is relatively heavy.

## Services View

| Data | Meaning |
| --- | --- |
| unit name | Service/unit identifier |
| scope | User vs system context |
| active state | Main activation state |
| substate | More specific runtime state |
| file state | File-backed unit state |
| enablement / preset | Enablement or preset metadata where available |

The right-hand side can preview the currently opened service, including unit file content and basic metadata.

## Dependency View

`Deps` focuses on reverse dependencies for the selected service. It is meant to answer: “what else depends on this?” before you change or remove a unit.

## Config View

| Content | Notes |
| --- | --- |
| Service files | Unit definitions |
| Config files | systemd-managed configuration |
| Drop-ins | Overrides and related fragments |

## Editing Model

`SYSTEMD` uses the shared external-editor handoff model.

| Step | Result |
| --- | --- |
| Select an item | Choose a service/config file |
| Press `i` | Open `$VISUAL` / `$EDITOR` |
| Save and exit | Return to `lulo` |
| Privileged commit | Performed through the backend path when needed |

This covers both system files and user-writable files without requiring the TUI itself to run as root.

## Current Scope

| What Exists Today | What Is Not the Main Workflow Yet |
| --- | --- |
| Browse units | Broad lifecycle management from inside the TUI |
| Inspect reverse dependencies | Start/stop/restart as a polished end-user workflow |
| Inspect/edit unit and config files | Full systemctl replacement behavior |

## See Also

- [ARCHITECTURE.md](ARCHITECTURE.md)
- [CGROUPS.md](CGROUPS.md)
- [UDEV.md](UDEV.md)
