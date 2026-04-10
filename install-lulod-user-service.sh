#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
unit_dir="$config_home/systemd/user"
unit_path="$unit_dir/lulod.service"

mkdir -p "$unit_dir"

sed \
  -e "s|@EXEC_START@|$repo_dir/lulod|g" \
  -e "s|@WORKDIR@|$repo_dir|g" \
  "$repo_dir/lulod.service.in" >"$unit_path"

systemctl --user daemon-reload
systemctl --user enable --now lulod.service
systemctl --user restart lulod.service
systemctl --user status lulod.service --no-pager --lines=20
