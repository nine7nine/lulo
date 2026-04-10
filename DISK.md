# Lulo Disk View

The `DISK` page is a compact filesystem usage dashboard.
It is intentionally simpler than the scheduler, systemd, or tune pages.

## Scope

The page shows mounted filesystem usage with a fast, glanceable layout.

Current data includes:

- device / filesystem source
- mount point
- used vs total space
- percentage used
- an aggregate total row

## Layout Model

Each mounted filesystem is rendered as its own row with:

- a stable per-filesystem color
- a usage bar
- percentage used
- used / total size text

The top `total` row provides a combined filesystem summary using its own color
so it reads as an aggregate rather than as one of the component mounts.

## Intended Use

The disk page is designed for:

- quick capacity checks
- spotting heavily used mounts
- seeing total usage without leaving the TUI

It is not currently a filesystem editor or block-device management surface.

## Relation to Other Pages

`DISK` is deliberately read-only and lightweight.
More detailed system-management surfaces live elsewhere:

- [SYSTEMD.md](SYSTEMD.md)
- [CGROUPS.md](CGROUPS.md)
- [TUNE.md](TUNE.md)
