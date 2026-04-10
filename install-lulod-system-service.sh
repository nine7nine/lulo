#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
unit_path="/etc/systemd/system/lulod-system.service"
config_root="/etc/lulo/scheduler"
tmp_unit="$(mktemp)"
if (( EUID == 0 )); then
  as_root=()
else
  as_root=(sudo)
fi
trap 'rm -f "$tmp_unit"' EXIT

sed \
  -e "s|@EXEC_START@|$repo_dir/lulod-system|g" \
  -e "s|@WORKDIR@|$repo_dir|g" \
  "$repo_dir/lulod-system.service.in" >"$tmp_unit"

"${as_root[@]}" install -Dm644 "$tmp_unit" "$unit_path"
"${as_root[@]}" install -d "$config_root/profiles.d" "$config_root/rules.d"

while IFS= read -r -d '' src; do
  rel="${src#"$repo_dir/examples/scheduler/"}"
  dst="$config_root/$rel"
  if ! "${as_root[@]}" test -e "$dst"; then
    "${as_root[@]}" install -Dm644 "$src" "$dst"
  fi
done < <(find "$repo_dir/examples/scheduler" -type f -print0 | sort -z)

"${as_root[@]}" systemctl daemon-reload
"${as_root[@]}" systemctl enable --now lulod-system.service
"${as_root[@]}" systemctl restart lulod-system.service
"${as_root[@]}" systemctl status lulod-system.service --no-pager --lines=20

printf 'installed system service to %s\n' "$unit_path"
printf 'scheduler config root: %s\n' "$config_root"
printf 'example config files were installed only where files were absent.\n'
printf 're-run this script if the repo path changes.\n'
printf 'normal update cycle:\n'
printf '  cd %s\n' "$repo_dir"
printf '  make\n'
printf '  sudo systemctl restart lulod-system.service\n'
printf '  systemctl --user restart lulod.service\n'
printf '  TERM=gnome COLORTERM=24bit ./lulo -i nc\n'
