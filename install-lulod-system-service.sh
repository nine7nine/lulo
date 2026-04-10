#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
unit_path="/etc/systemd/system/lulod-system.service"
config_root="/etc/lulo/scheduler"
bindir="${LULO_BINDIR:-/usr/bin}"
datadir="${LULO_DATADIR:-/usr/share/lulo}"
examples_root="$repo_dir/examples/scheduler"
exec_start="$repo_dir/lulod-system"
workdir="$repo_dir"
lulo_cmd="$repo_dir/lulo"
tmp_unit="$(mktemp)"
if (( EUID == 0 )); then
  as_root=()
else
  as_root=(sudo)
fi
trap 'rm -f "$tmp_unit"' EXIT

if [[ -x "$bindir/lulod-system" && -d "$datadir" ]]; then
  exec_start="$bindir/lulod-system"
  workdir="$datadir"
fi
if [[ -x "$bindir/lulo" ]]; then
  lulo_cmd="$bindir/lulo"
fi
if [[ -f "${LULO_EXAMPLES_DIR:-$datadir/examples/scheduler}/scheduler.conf" ]]; then
  examples_root="${LULO_EXAMPLES_DIR:-$datadir/examples/scheduler}"
fi

sed \
  -e "s|@EXEC_START@|$exec_start|g" \
  -e "s|@WORKDIR@|$workdir|g" \
  "$repo_dir/lulod-system.service.in" >"$tmp_unit"

"${as_root[@]}" install -Dm644 "$tmp_unit" "$unit_path"
"${as_root[@]}" install -d "$config_root/profiles.d" "$config_root/rules.d"

while IFS= read -r -d '' src; do
  rel="${src#"$examples_root/"}"
  dst="$config_root/$rel"
  if ! "${as_root[@]}" test -e "$dst"; then
    "${as_root[@]}" install -Dm644 "$src" "$dst"
  fi
done < <(find "$examples_root" -type f -print0 | sort -z)

"${as_root[@]}" systemctl daemon-reload
"${as_root[@]}" systemctl enable --now lulod-system.service
"${as_root[@]}" systemctl restart lulod-system.service
"${as_root[@]}" systemctl status lulod-system.service --no-pager --lines=20

printf 'installed system service to %s\n' "$unit_path"
printf 'scheduler config root: %s\n' "$config_root"
printf 'example config files were installed only where files were absent.\n'
printf 're-run this script if the install path changes.\n'
printf 'normal update cycle:\n'
printf '  cd %s\n' "$repo_dir"
printf '  make PREFIX=/usr\n'
printf '  sudo systemctl restart lulod-system.service\n'
printf '  systemctl --user restart lulod.service\n'
printf '  TERM=gnome COLORTERM=24bit %s -i nc\n' "$lulo_cmd"
