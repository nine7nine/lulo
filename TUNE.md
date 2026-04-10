# Lulo Tune View

The `TUNE` page is the tunables explorer and preset/snapshot surface. It is designed around browsing real kernel/sysfs/procfs state, editing values, and saving reusable bundles.

## Subviews

| Subview | Purpose |
| --- | --- |
| `Explore` | Live filesystem-style browser for tunable sources |
| `Snapshots` | Saved point-in-time bundles |
| `Presets` | Named reusable bundles intended for repeated application |

## Explore View

`Explore` is the live browser for tunable paths.

| Source | Why it matters |
| --- | --- |
| `/proc/sys` | Kernel sysctl-style settings |
| `/sys` | Kernel and device-facing tunables |
| `/sys/fs/cgroup` | Cgroup-related tunables and controls |

The page is path-oriented rather than hardcoded around a fixed list of known knobs.

## Editing Model

`TUNE` uses two different editing styles depending on the type of item.

| Edit Style | Used For |
| --- | --- |
| Inline value editing | Pseudo-files and direct tunable values |
| External editor handoff | File-backed snapshot and preset bundles |

This split is intentional: kernel pseudo-files are not normal config documents, so inline editing is the right UX there.

## Snapshots and Presets

Tune bundles are file-backed and manageable from the TUI.

| Operation | Supported |
| --- | --- |
| Create | Yes |
| Edit in `$VISUAL` / `$EDITOR` | Yes |
| Rename displayed metadata | Yes |
| Delete | Yes |
| Apply | Yes |

### Bundle Roles

| Bundle Type | Purpose |
| --- | --- |
| `Snapshots` | Saved state captured from exploration work |
| `Presets` | Named reusable bundles intended for repeated application |

## Privileged Apply Model

| Layer | Responsibility |
| --- | --- |
| `lulo` | Editing, staging, and invoking apply |
| `lulod` | Snapshot and bundle-facing state |
| `lulod-system` | Privileged mutation when writes require elevated access |

## Relation to Other Pages

| Related Page | Why it matters |
| --- | --- |
| [CGROUPS.md](CGROUPS.md) | Cgroup filesystem and config-level work |
| [SCHED.md](SCHED.md) | Process scheduling policy |
| [SYSTEMD.md](SYSTEMD.md) / [UDEV.md](UDEV.md) | Service/device-related config editing |

## See Also

- [CGROUPS.md](CGROUPS.md)
- [SCHED.md](SCHED.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
