#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_root="/etc/lulo/scheduler"

if (( EUID == 0 )); then
  as_root=()
else
  as_root=(sudo)
fi

"${as_root[@]}" rm -rf "$config_root"
"${as_root[@]}" install -d "$config_root/profiles.d" "$config_root/rules.d"
"${as_root[@]}" install -m 0644 "$repo_dir/examples/scheduler/scheduler.conf" "$config_root/scheduler.conf"

while IFS= read -r -d '' src; do
  rel="${src#"$repo_dir/examples/scheduler/"}"
  dst="$config_root/$rel"
  "${as_root[@]}" install -Dm644 "$src" "$dst"
done < <(find "$repo_dir/examples/scheduler" -type f ! -name 'scheduler.conf' -print0 | sort -z)

printf 'reset scheduler config under %s\n' "$config_root"
