# Lulo Disk View

The `DISK` page is a compact filesystem usage dashboard. It is intentionally simpler than the scheduler, systemd, or tune pages.

## Scope

| Item | What it shows |
| --- | --- |
| Filesystem source | Device or mount source |
| Mount point | Where it is mounted |
| Usage bar | Used vs available space |
| Percentage | Percent used |
| Aggregate row | Combined total summary |

## Layout Model

Each mounted filesystem is rendered as its own row.

| Element | Purpose |
| --- | --- |
| Stable per-filesystem color | Makes rows easy to track visually |
| Usage bar | Quick at-a-glance capacity view |
| Percentage | Immediate utilization reading |
| Used / total text | Exact capacity values |

The top `total` row uses its own aggregate color so it reads as a summary rather than as one of the component mounts.

## Intended Use

| Good For | Not Intended For |
| --- | --- |
| Quick capacity checks | Full filesystem administration |
| Spotting heavily used mounts | Block-device partitioning |
| Total-usage awareness without leaving the TUI | Storage provisioning workflows |

## Relation to Other Pages

| Related Page | Why it matters |
| --- | --- |
| [SYSTEMD.md](SYSTEMD.md) | Service-side storage consumers and config |
| [CGROUPS.md](CGROUPS.md) | Control-oriented system resource views |
| [TUNE.md](TUNE.md) | Tunables and low-level kernel/system settings |
