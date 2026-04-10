#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
policy_path="/usr/share/polkit-1/actions/io.lulo.admin.policy"
tmp_policy="$(mktemp)"
trap 'rm -f "$tmp_policy"' EXIT

chmod +x "$repo_dir/lulo-admin"
sed -e "s|@EXEC_START@|$repo_dir/lulo-admin|g" \
  "$repo_dir/lulo-admin.policy.in" >"$tmp_policy"

sudo install -Dm644 "$tmp_policy" "$policy_path"
printf 'installed polkit policy to %s\n' "$policy_path"
printf 'helper path: %s/lulo-admin\n' "$repo_dir"
printf 're-run this script if the repo path changes.\n'
printf 'normal update cycle:\n'
printf '  cd %s\n' "$repo_dir"
printf '  make\n'
printf '  systemctl --user restart lulod.service\n'
