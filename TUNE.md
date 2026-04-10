# Lulo Tune View

The `TUNE` page is the tunables explorer and preset/snapshot surface.
It is designed around browsing real kernel/sysfs/procfs state, editing values,
and saving reusable bundles.

## Subviews

The page has three subviews:

- `Explore`
  - live filesystem-style browser for tunable sources
- `Snapshots`
  - saved point-in-time bundles
- `Presets`
  - named reusable bundles intended for repeated application

## Explore View

`Explore` is the live browser for tunable paths.
It is used for sources such as:

- `/proc/sys`
- `/sys`
- `/sys/fs/cgroup`

The page shows path-oriented state rather than a hardcoded list of known keys,
which keeps it useful even as the kernel and local system layout vary.

## Editing Model

`TUNE` uses two different edit styles depending on the kind of item:

- inline value editing
  - for pseudo-files and direct tunable values
- external editor handoff
  - for file-backed snapshot and preset bundles

That split is intentional: kernel pseudo-files are not normal config documents,
so inline editing is the right UX there.

## Snapshots and Presets

Tune bundles are file-backed and manageable from the TUI.

Current bundle operations include:

- create
- edit in `$VISUAL` / `$EDITOR`
- rename displayed bundle metadata
- delete
- apply

The distinction is conceptual:

- `Snapshots`
  - capture a saved state from exploration work
- `Presets`
  - reusable named bundles intended for repeated application

## Privileged Apply Model

Applying tunables is not done directly by the TUI.
When privileged writes are required, they are committed through the backend
privileged path.

That keeps:

- editing and staging in the TUI
- privileged mutation in `lulod-system`

## Relation to Other Pages

`TUNE` overlaps with several other system-management surfaces:

- `CGROUPS` for cgroup filesystem and config-level work
- `SCHED` for process scheduling policy
- `SYSTEMD` and `UDEV` for service/device-related config editing

## See Also

- [CGROUPS.md](CGROUPS.md)
- [SCHED.md](SCHED.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
