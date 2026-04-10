# Lulo Scheduler Config

The scheduler uses a compact directory-based config instead of a large DSL.

## Layout

| Path | Purpose |
| --- | --- |
| `scheduler.conf` | Global scheduler settings |
| `profiles.d/*.conf` | Profile definitions |
| `rules.d/*.conf` | Rule definitions |

## Matcher Support

| Matcher | Purpose |
| --- | --- |
| `comm` | Process command name |
| `exe` | Executable path |
| `cmdline` | Full command line |
| `unit` | systemd unit |
| `slice` | systemd slice |
| `cgroup` | cgroup path |

## Profile Support

| Field | Meaning |
| --- | --- |
| `nice` | Linux nice level |
| `policy` | Linux scheduling policy |
| `rt_priority` | Real-time priority |
| `io_class` | Linux I/O priority class |
| `io_priority` | Linux I/O priority level |

## Rule Support

| Field | Meaning |
| --- | --- |
| `enabled` | Enables or disables the rule |
| `exclude` | Stops further assignment when matched |
| `profile` | Target profile |
| `pattern` | Match pattern |

## Global Scheduler Settings

| Setting | Purpose |
| --- | --- |
| `watcher_interval_ms` | `/proc` rescan interval |
| `focus_enabled` | Enables focused-app policy |
| `focus_profile` | Profile assigned to the active app |
| `background_enabled` | Enables background fallback |
| `background_profile` | Fallback profile for lower-priority app-scope processes |
| `background_match_app_slice` | Match app-scope slices |
| `background_match_background_slice` | Match background slices |
| `background_match_app_unit_prefix` | Match app-style unit prefixes |

## Dynamic Policy

| Behavior | Notes |
| --- | --- |
| `exclude=true` | Stops further assignment |
| Focus policy | Focused app is assigned `focus_profile` when enabled |
| Focus precedence | Focused assignment outranks ordinary static app buckets |
| Explicit rules | Applied after exclusions and before background fallback |
| Background fallback | Controlled by `background_match_*` settings and shown as `(background)` in `SCHED -> Rules` |
| Focus visibility | Focus policy is shown as `(focus)` in `SCHED -> Rules` |

This keeps the watcher compact while still supporting session-aware focus boosting, systemd-aware matching, and per-profile I/O tuning without changing the overall service architecture.
