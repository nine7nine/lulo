# Lulo Scheduler

`lulo`'s scheduler is a policy layer for Linux process priority management.
It combines file-backed config with a long-running watcher so policy can be
both editable and continuously enforced.

## Process Ownership

The scheduler is split across the three main processes:

- `lulo`
  - shows the `SCHED` page and opens config files in `$VISUAL` / `$EDITOR`
- `lulod`
  - serves scheduler snapshots to the TUI
  - tracks the focused window and forwards focused PID updates
- `lulod-system`
  - loads scheduler config from `/etc/lulo/scheduler`
  - scans `/proc`
  - applies nice, scheduling policy, RT priority, and I/O priority
  - keeps those settings enforced over time

## Config Layout

System scheduler config lives in:

- `/etc/lulo/scheduler/scheduler.conf`
- `/etc/lulo/scheduler/profiles.d/*.conf`
- `/etc/lulo/scheduler/rules.d/*.conf`

Shipped examples live under:

- `/usr/share/lulo/examples/scheduler`

The config is intentionally directory-based instead of using one large DSL.
That keeps overrides, editing, and packaging simple.

## Profiles

Profiles describe how a matched process should be treated.

Current profile fields:

- `name`
- `enabled`
- `nice`
- `policy`
- `rt_priority`
- `io_class`
- `io_priority`

Examples:

- `audio-rt`
  - high-priority audio processes
- `desktop-rt`
  - compositor-critical desktop processes such as `kwin_wayland`
- `focused`
  - whichever app currently owns focus
- `background`
  - fallback for lower-priority app-scope processes
- `idle`
  - build jobs and other work that should yield aggressively

## Rules

Rules decide which profile should be applied to which processes.

Current rule fields:

- `enabled`
- `match`
- `pattern`
- `profile`
- `exclude`

Current matcher support:

- `comm`
- `exe`
- `cmdline`
- `unit`
- `slice`
- `cgroup`

Typical rule styles:

- exclude internal/system policy tools from further reassignment
- map audio daemons to `audio-rt`
- map compositor/session services to desktop-oriented profiles
- map builds and compilers to `idle`

If `exclude=true` matches, assignment stops for that process.

## Dynamic Policy

Not all scheduler behavior is file-backed static matching.
Two built-in dynamic policies are also exposed in `SCHED -> Rules`:

- `(focus)`
  - assigns `focus_profile` to the currently focused app
- `(background)`
  - applies `background_profile` to app-scope processes that were not handled by stronger rules

These are configured through `scheduler.conf`, not separate rule files.
They are shown in the UI so the policy is visible rather than hidden in code.

Current global settings include:

- `watcher_interval_ms`
- `focus_enabled`
- `focus_profile`
- `background_enabled`
- `background_profile`
- `background_match_app_slice`
- `background_match_background_slice`
- `background_match_app_unit_prefix`

## Focused / Active App Scheduling

The `focused` profile is a runtime override for the app that currently owns
window focus. It is intentionally dynamic rather than file-backed to avoid
hardcoding one app into a permanent high-priority bucket.

Current behavior:

- if focus tracking is enabled, the focused app is assigned `focus_profile`
- this is shown in `SCHED -> Rules` as `(focus)`
- the resolved live process state is shown in `SCHED -> Live`
- the focused profile can use nice, RT policy, and I/O tuning like any other profile

Current limitation:

- active-window detection is currently implemented only for KDE/Plasma
- that path is provided by `lulod-focus-kde`
- the core scheduler is desktop-agnostic, but the focused-window provider is not yet
- GNOME, sway, and other compositor-specific providers are planned

That means normal rules, profiles, background fallback, and scheduler
enforcement work regardless of desktop, but dynamic focused-app boosting is
currently limited to environments with a supported focus provider.

## Enforcement Model

`lulod-system` applies the configured policy continuously rather than only once.
That matters because processes come and go, and some applications respawn helper
processes or threads after startup.

In practice the scheduler works like this:

- load global settings, profiles, and rules
- scan `/proc` on the configured watcher interval
- classify processes using explicit rules plus dynamic focus/background policy
- apply:
  - nice
  - Linux scheduling policy
  - RT priority
  - Linux I/O priority
- report the current live state back to the TUI

This makes the scheduler closer to a compact policy daemon than a one-shot
`chrt` / `renice` wrapper.

## TUI Model

The `SCHED` page has three subviews:

- `Profiles`
  - shows loaded profiles and their configured priority settings
- `Rules`
  - shows explicit rules plus the dynamic `(focus)` and `(background)` entries
- `Live`
  - shows the currently observed process state, including:
    - PID
    - command name
    - resolved profile
    - Linux scheduler policy
    - nice
    - RT priority
    - I/O priority
    - apply status

## Editing Workflow

`lulo` does not try to be a text editor.
Instead, it uses structured navigation plus external editor handoff.

Common actions in `SCHED`:

- `i`
  - edit the selected profile/rule file
  - for dynamic entries like `(focus)` and `(background)`, this opens `scheduler.conf`
- `n`
  - create a new profile or rule file and open it in the editor
- `d`
  - delete the selected file-backed profile or rule
- `R`
  - reload scheduler config

System files are committed through the privileged path, so editing `/etc/lulo`
config does not require the TUI itself to run as root.

## Relation to Other Pages

The scheduler is not isolated from the rest of the app:

- `CPU` / proc view lets you inspect the processes being managed
- `CGROUPS` exposes the cgroup layout the scheduler can match against
- `SYSTEMD` exposes units and slices that are useful for `unit` / `slice` rule matching
- `TUNE` and `UDEV` follow the same file-backed edit model used by `SCHED`

## See Also

- [ARCHITECTURE.md](ARCHITECTURE.md)
- [INSTALL.md](INSTALL.md)
- `examples/scheduler/README.md`
