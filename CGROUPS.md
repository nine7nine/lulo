# Lulo Cgroups View

The `CGROUPS` page is the cgroup inspection and editing surface.
It is intended to make cgroup state and configuration visible without dropping
out of the TUI.

## Subviews

The page has three subviews:

- `Tree`
  - browsable cgroup hierarchy
- `Files`
  - file-level view of the selected cgroup's control files
- `Config`
  - related config and unit-backed files

## Tree View

`Tree` shows the cgroup hierarchy so you can see:

- where a workload lives
- how units and slices map into the cgroup tree
- which scope or subtree a process or service belongs to

This is especially useful when combined with the scheduler, since `SCHED` rules
can match on cgroup, slice, and unit concepts.

## Files View

`Files` exposes cgroup control files for the selected node.
That gives direct visibility into controller state and tunables without leaving
`lulo`.

Edits to file-backed cgroup control points go through the privileged backend
path when required.

## Config View

`Config` shows related higher-level config and service-backed files that affect
cgroup behavior.
This gives the page both a live cgroup filesystem angle and a config-level angle.

## Current Scope

What exists today:

- browse the cgroup hierarchy
- inspect cgroup files
- inspect/edit related config files

What is not yet a polished end-user workflow:

- creating new cgroups
- deleting cgroups
- full cgroup orchestration from the TUI

The page is currently strongest as an inspection and configuration surface.

## Relation to Scheduler

`CGROUPS` and `SCHED` are intentionally close:

- `CGROUPS` shows the hierarchy and control points
- `SCHED` uses cgroup-related matchers for policy decisions

This makes `CGROUPS` the place to understand the shape of the workload tree,
while `SCHED` is the place to define how that workload should be prioritized.

## See Also

- [SCHED.md](SCHED.md)
- [TUNE.md](TUNE.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
