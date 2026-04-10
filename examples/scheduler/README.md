# Lulo Scheduler Config

This scheduler uses a compact directory-based config instead of a large DSL.

Layout:

- `scheduler.conf`
- `profiles.d/*.conf`
- `rules.d/*.conf`

Current matcher support:

- `match=comm`
- `match=exe`
- `match=cmdline`
- `match=unit`
- `match=slice`
- `match=cgroup`

Current profile support:

- `nice`
- `policy`
- `rt_priority`

Current rule support:

- `enabled`
- `exclude`
- `profile`
- `pattern`

Global scheduler settings:

- `watcher_interval_ms`
- `focus_enabled`
- `focus_profile`
- `background_enabled`
- `background_profile`
- `background_match_app_slice`
- `background_match_background_slice`
- `background_match_app_unit_prefix`

Dynamic policy:

- `exclude=true` stops further assignment
- if focus tracking is enabled, the focused app is assigned `focus_profile`
- focused assignment outranks ordinary static app buckets
- otherwise explicit rules apply
- finally app-scope processes can fall back to `background_profile`
- the app-scope fallback classifier is controlled by the `background_match_*`
  settings and is shown in the SCHED `Rules` view as `(background)`
- the focused-window policy is shown in the SCHED `Rules` view as `(focus)`

This keeps the watcher compact while still supporting session-aware focus
boosting and systemd-aware matching. I/O classes and richer policy logic can be
added later without changing the overall service architecture.
