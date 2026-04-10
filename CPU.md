# Lulo CPU and Process View

The `CPU` page combines machine-level CPU monitoring with a live process tree.
It is the main runtime observability view in `lulo`.

## Scope

The page covers:

- per-CPU history and current load
- CPU frequency, governor, and temperature summary
- a process tree with scheduler-relevant process metadata
- direct process inspection and signaling

## CPU Sampling Model

The top bar sample interval controls the main CPU sampling cadence.

The CPU page uses that to drive:

- machine CPU history
- per-core usage snapshots
- current CPU metadata shown alongside the history view

Process refresh is tracked separately from the top-level CPU heat graph so the
process tree can be tuned independently.

## Process Tree

The lower half of the page is a live process tree.
It preserves parent/child structure and exposes scheduling-relevant fields.

Current process data includes:

- PID
- user
- scheduling policy
- priority / nice
- CPU usage
- memory usage
- CPU time
- Linux I/O priority
- command line

Long command lines can be horizontally panned so tree structure and command text
remain usable without collapsing the entire row layout.

## CPU Percentage Modes

Process CPU can be shown in two normalization modes:

- `total`
  - default
  - percent of total machine CPU capacity
- `per-core`
  - htop-style percent of one CPU

This affects the process tree CPU column only; it does not change the machine
CPU history graphs.

## Process Actions

The CPU page also acts as the process-management surface.

Current actions include:

- selecting and navigating the process tree
- collapsing or expanding branches
- sending `SIGTERM`
- sending `SIGKILL`

The page is intentionally conservative about privileged inspection helpers.
When future process-debug actions are added, they should follow the same
explicit, context-aware model used elsewhere in the app.

## Scheduler Context

The CPU page and `SCHED` page are related but separate:

- `CPU` is the live process-centric inspection surface
- `SCHED` is the scheduler policy/configuration surface

That means the process tree is where you confirm how a process looks right now,
while `SCHED -> Live` explains why a process is being treated that way.

## See Also

- [SCHED.md](SCHED.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
