# Lulo Cgroups View

The `CGROUPS` page is the cgroup inspection and editing surface. It is intended to make cgroup state and configuration visible without dropping out of the TUI.

## Subviews

| Subview | Purpose |
| --- | --- |
| `Tree` | Browsable cgroup hierarchy |
| `Files` | File-level view of the selected cgroup's control files |
| `Config` | Related config and unit-backed files |

## Tree View

| What it shows | Why it matters |
| --- | --- |
| Workload placement | See where a workload lives |
| Unit and slice mapping | Understand how units/slices map into the cgroup tree |
| Scope/subtree structure | Understand which subtree a process or service belongs to |

This is especially useful with the scheduler, since `SCHED` rules can match on cgroup, slice, and unit concepts.

## Files View

`Files` exposes cgroup control files for the selected node.

| Benefit | Notes |
| --- | --- |
| Direct controller visibility | Inspect live cgroup controller state |
| Tunable visibility | See available control points |
| Backend-safe editing | Privileged path is used when required |

## Config View

`Config` shows related higher-level config and service-backed files that affect cgroup behavior. It gives the page both a live cgroup-filesystem angle and a config-level angle.

## Current Scope

| What Exists Today | What Is Not Yet a Polished Workflow |
| --- | --- |
| Browse the cgroup hierarchy | Creating new cgroups |
| Inspect cgroup files | Deleting cgroups |
| Inspect/edit related config files | Full cgroup orchestration from the TUI |

The page is currently strongest as an inspection and configuration surface.

## Relation to Scheduler

| Page | Role |
| --- | --- |
| `CGROUPS` | Shows the hierarchy and control points |
| `SCHED` | Uses cgroup-related matchers for policy decisions |

This makes `CGROUPS` the place to understand the shape of the workload tree, while `SCHED` is the place to define how that workload should be prioritized.

## See Also

- [SCHED.md](SCHED.md)
- [TUNE.md](TUNE.md)
- [ARCHITECTURE.md](ARCHITECTURE.md)
