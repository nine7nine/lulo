# Lulo Scheduler

`lulo`'s scheduler is a policy layer for Linux process priority management. It combines file-backed config with a long-running watcher so policy can be both editable and continuously enforced.

## Process Ownership

| Process | Scheduler Role |
| --- | --- |
| `lulo` | Shows the `SCHED` page and opens config files in `$VISUAL` / `$EDITOR` |
| `lulod` | Serves scheduler snapshots to the TUI and forwards focused PID updates |
| `lulod-system` | Loads config, scans `/proc`, applies nice/policy/RT/I/O priority, and enforces policy over time |

## Config Layout

| Path | Purpose |
| --- | --- |
| `/etc/lulo/scheduler/scheduler.conf` | Global scheduler settings |
| `/etc/lulo/scheduler/profiles.d/*.conf` | Profile definitions |
| `/etc/lulo/scheduler/rules.d/*.conf` | Rule definitions |
| `/usr/share/lulo/examples/scheduler` | Shipped example config |

The config is intentionally directory-based instead of using one large DSL.

## Profiles

Profiles describe how a matched process should be treated.

### Profile Fields

| Field | Meaning |
| --- | --- |
| `name` | Profile identifier |
| `enabled` | Enables or disables the profile |
| `nice` | Linux nice level |
| `policy` | Linux scheduling policy |
| `rt_priority` | Real-time priority for RT policies |
| `io_class` | Linux I/O priority class |
| `io_priority` | Linux I/O priority level |

### Common Profiles

| Profile | Intended Use |
| --- | --- |
| `audio-rt` | High-priority audio processes |
| `desktop-rt` | Compositor-critical desktop processes such as `kwin_wayland` |
| `focused` | Whichever app currently owns focus |
| `background` | Fallback for lower-priority app-scope processes |
| `idle` | Build jobs and other work that should yield aggressively |

## Rules

Rules decide which profile should be applied to which processes.

### Rule Fields

| Field | Meaning |
| --- | --- |
| `enabled` | Enables or disables the rule |
| `match` | Matcher type |
| `pattern` | Match pattern |
| `profile` | Target profile |
| `exclude` | Stops further assignment when matched |

### Supported Matchers

| Matcher | Notes |
| --- | --- |
| `comm` | Process command name |
| `exe` | Executable path |
| `cmdline` | Full command line |
| `unit` | systemd unit |
| `slice` | systemd slice |
| `cgroup` | cgroup path |

### Where Matches Come From

| Matcher Group | Source | Typical Use |
| --- | --- | --- |
| `comm`, `exe`, `cmdline` | `/proc` process metadata | App and binary matching |
| `unit`, `slice` | systemd/cgroup placement | Service and session classification |
| `cgroup` | Full cgroup path | Fine-grained subtree matching |

This is what lets the scheduler combine process identity, systemd layout, and cgroup structure in one policy model.

### How Policy Resolves

| Order | Step |
| --- | --- |
| 1 | Exclusions stop further assignment |
| 2 | Focused-app override applies when enabled and a focus provider is active |
| 3 | Explicit rules match on process, unit, slice, and cgroup metadata |
| 4 | Background fallback handles remaining app-scope processes |

## Dynamic Policy

Two built-in dynamic policies are also exposed in `SCHED -> Rules`.
They are configured through `scheduler.conf`, not separate rule files.

| Built-In | Purpose |
| --- | --- |
| `(focus)` | Assigns `focus_profile` to the currently focused app |
| `(background)` | Applies `background_profile` to app-scope processes not handled by stronger rules |

### Global Settings

| Setting | Purpose |
| --- | --- |
| `watcher_interval_ms` | `/proc` rescan interval |
| `focus_enabled` | Enables focused-app policy |
| `focus_profile` | Profile assigned to the active app |
| `background_enabled` | Enables background fallback policy |
| `background_profile` | Profile assigned by the background fallback |
| `background_match_app_slice` | Match app-scope slices |
| `background_match_background_slice` | Match background slices |
| `background_match_app_unit_prefix` | Match app-style unit prefixes |

## Focused / Active App Scheduling

The `focused` profile is a runtime override for the app that currently owns window focus.
It is intentionally dynamic rather than file-backed to avoid permanently hardcoding one app into a high-priority bucket.

### Current Behavior

| Behavior | Notes |
| --- | --- |
| Focus tracking enabled | Focused app gets `focus_profile` |
| Rules view exposure | Shown as `(focus)` in `SCHED -> Rules` |
| Live confirmation | Resolved state appears in `SCHED -> Live` |
| Full profile support | Focused profile can use nice, RT policy, and I/O tuning |

### Current Limitation

| Item | Status |
| --- | --- |
| KDE/Plasma focus provider | Implemented |
| `lulod-focus-kde` | Current backend |
| GNOME provider | Planned |
| sway provider | Planned |
| Other compositor-specific providers | Planned |

The scheduler core is desktop-agnostic. Only active-window detection is currently limited to environments with a supported focus provider.

## Enforcement Model

`lulod-system` applies policy continuously rather than only once.

### Enforcement Loop

| Step | Action |
| --- | --- |
| 1 | Load globals, profiles, and rules |
| 2 | Scan `/proc` on the configured watcher interval |
| 3 | Classify processes using explicit rules plus dynamic focus/background policy |
| 4 | Apply nice, Linux scheduling policy, RT priority, and Linux I/O priority |
| 5 | Report live state back to the TUI |

This makes the scheduler closer to a compact policy daemon than a one-shot `chrt` / `renice` wrapper.

## TUI Model

The `SCHED` page has three subviews.

| Subview | Purpose |
| --- | --- |
| `Profiles` | Shows loaded profiles and configured priority settings |
| `Rules` | Shows explicit rules plus `(focus)` and `(background)` |
| `Live` | Shows current process state and resolved scheduler policy |

### Live Columns

| Field | Meaning |
| --- | --- |
| PID | Process id |
| command | Command name |
| profile | Resolved profile |
| policy | Linux scheduling policy |
| nice | Nice level |
| RT | RT priority |
| I/O | Linux I/O priority |
| status | Apply/result state |

## Editing Workflow

`lulo` does not try to be a text editor. It uses structured navigation plus external editor handoff.

| Action | Effect |
| --- | --- |
| `i` | Edit the selected profile/rule file |
| `i` on `(focus)` / `(background)` | Opens `scheduler.conf` |
| `n` | Create a new profile or rule file and open it in the editor |
| `d` | Delete the selected file-backed profile or rule |
| `R` | Reload scheduler config |

System files are committed through the privileged path, so editing `/etc/lulo` config does not require the TUI itself to run as root.

## Relation to Other Pages

| Page | Why it matters to SCHED |
| --- | --- |
| `CPU` | Shows the live processes being managed |
| `CGROUPS` | Exposes the cgroup layout the scheduler can match against |
| `SYSTEMD` | Exposes units and slices useful for `unit` / `slice` matching |
| `TUNE` / `UDEV` | Follows the same file-backed edit model |

## See Also

- [ARCHITECTURE.md](ARCHITECTURE.md)
- [INSTALL.md](INSTALL.md)
- `examples/scheduler/README.md`
