#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
policy_path="/usr/share/polkit-1/actions/io.lulo.admin.policy"
helperdir="${LULO_HELPERDIR:-/usr/libexec/lulo}"
exec_start="$repo_dir/lulo-admin"
tmp_policy="$(mktemp)"
trap 'rm -f "$tmp_policy"' EXIT

if [[ -x "$helperdir/lulo-admin" ]]; then
  exec_start="$helperdir/lulo-admin"
else
  chmod +x "$repo_dir/lulo-admin"
fi

sed -e "s|@EXEC_START@|$exec_start|g" \
  "$repo_dir/lulo-admin.policy.in" >"$tmp_policy"

sudo install -Dm644 "$tmp_policy" "$policy_path"
printf 'installed polkit policy to %s\n' "$policy_path"
printf 'helper path: %s\n' "$exec_start"
printf 're-run this script if the helper install path changes.\n'
printf 'normal update cycle:\n'
printf '  cd %s\n' "$repo_dir"
printf '  make\n'
printf '  systemctl --user restart lulod.service\n'
