# Screenshots

Drop PNG screenshots here using the filenames below and they will appear in the
generated docs automatically (the `<figure class="screenshot">` blocks already
reference them). Capture at a comfortable terminal size; they are displayed at
full container width with a rounded border.

| File | Page | Suggested shot |
| --- | --- | --- |
| `architecture-overview.png` | architecture | The page bar across the top of the TUI with any live view below |
| `cpu-page.png` | cpu | The per-core CPU heatmap above the process tree |
| `disk-page.png` | disk | The DISK dashboard (filesystems + devices + I/O + queue panels) |
| `cgroups-tree.png` | cgroups | CGROUPS &rarr; Tree with the member-process preview |
| `systemd-services.png` | systemd | SYSTEMD &rarr; Services with a unit fragment previewed |
| `udev-rules.png` | udev | UDEV &rarr; Rules with a rule file previewed |
| `sched-live.png` | scheduler | SCHED &rarr; Live showing resolved per-process policy |
| `tune-explore.png` | tune | TUNE &rarr; Explore over /proc/sys with a staged value |
| `auth-overlay.png` | process-model-ipc | The RW-mode password prompt overlay inside the TUI |
| `focus-boost.png` | focus-providers | SCHED &rarr; Live with the focused app resolved to the focus profile |

Filenames are referenced from the `.md` sources; if you rename a file, update the
matching `<img src="img/...">` in that page and re-run `./md2html.sh <page>.md`.
