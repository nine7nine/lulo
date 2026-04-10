# Lulo CPU and Process View

The `CPU` page combines machine-level CPU monitoring with a live process tree. It is the main runtime observability view in `lulo`.

## Scope

| Area | What it shows |
| --- | --- |
| CPU history | Per-CPU history and current load |
| CPU metadata | Frequency, governor, and temperature summary |
| Process tree | Scheduler-relevant process metadata in tree form |
| Process actions | Direct process inspection and signaling |

## CPU Sampling Model

| Item | Behavior |
| --- | --- |
| Top bar sample interval | Controls the main CPU sampling cadence |
| Machine CPU history | Driven by the main CPU sample cadence |
| Per-core snapshots | Updated from the same CPU sampling path |
| Process refresh | Tracked separately so the proc tree can be tuned independently |

## Process Tree

The lower half of the page is a live process tree. It preserves parent/child structure and exposes runtime process state.

### Current Process Data

| Field | Meaning |
| --- | --- |
| PID | Process id |
| user | Owning user |
| policy | Linux scheduling policy |
| priority / nice | Current scheduling priority |
| CPU | Current CPU usage |
| memory | Memory usage |
| time | Accumulated CPU time |
| I/O | Linux I/O priority |
| command | Command line |

Long command lines can be horizontally panned so tree structure stays readable while still allowing full command inspection.

## CPU Percentage Modes

| Mode | Meaning |
| --- | --- |
| `total` | Default; percent of total machine CPU capacity |
| `per-core` | htop-style percent of one CPU |

This affects the process tree CPU column only. It does not change the machine CPU history graphs.

## Process Actions

| Action | Purpose |
| --- | --- |
| Selection / navigation | Move through the process tree |
| Collapse / expand | Control tree visibility |
| `SIGTERM` | Graceful process termination |
| `SIGKILL` | Forced process termination |

The page is intentionally conservative about privileged inspection helpers. When future process-debug actions are added, they should follow the same explicit model used elsewhere in the app.

## Relation to Scheduler

| Page | Role |
| --- | --- |
| `CPU` | Live process-centric inspection surface |
| `SCHED` | Scheduler policy and configuration surface |

That means the process tree is where you confirm how a process looks right now, while `SCHED -> Live` explains why a process is being treated that way.

## See Also

- [SCHED.md](SCHED.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
