#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
unit_dir="$config_home/systemd/user"
unit_path="$unit_dir/lulod.service"
dropin_dir="$unit_dir/lulod.service.d"
dropin_path="$dropin_dir/session-env.conf"
bindir="${LULO_BINDIR:-/usr/bin}"
datadir="${LULO_DATADIR:-/usr/share/lulo}"
exec_start="$repo_dir/lulod"
workdir="$repo_dir"
import_keys=(
  XDG_RUNTIME_DIR
  DBUS_SESSION_BUS_ADDRESS
  XDG_SESSION_TYPE
  XDG_CURRENT_DESKTOP
  XDG_SESSION_DESKTOP
  DESKTOP_SESSION
  KDE_FULL_SESSION
  KDE_SESSION_VERSION
  WAYLAND_DISPLAY
  DISPLAY
)

escape_systemd_env() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '%s' "$value"
}

detect_focus_provider() {
  local combined=""
  local vars=(
    "${LULOD_FOCUS_PROVIDER-}"
    "${XDG_CURRENT_DESKTOP-}"
    "${XDG_SESSION_DESKTOP-}"
    "${DESKTOP_SESSION-}"
    "${KDE_FULL_SESSION-}"
    "${KDE_SESSION_VERSION-}"
  )

  if [[ -n "${LULOD_FOCUS_PROVIDER-}" ]]; then
    printf '%s' "$LULOD_FOCUS_PROVIDER"
    return
  fi

  combined="${vars[*]}"
  shopt -s nocasematch
  if [[ "$combined" == *kde* || "$combined" == *plasma* ]]; then
    printf 'kde'
  fi
  shopt -u nocasematch
}

if [[ -x "$bindir/lulod" && -d "$datadir" ]]; then
  exec_start="$bindir/lulod"
  workdir="$datadir"
fi

mkdir -p "$unit_dir"

sed \
  -e "s|@EXEC_START@|$exec_start|g" \
  -e "s|@WORKDIR@|$workdir|g" \
  "$repo_dir/lulod.service.in" >"$unit_path"

mkdir -p "$dropin_dir"
{
  printf '[Service]\n'
  for key in XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP DESKTOP_SESSION KDE_FULL_SESSION KDE_SESSION_VERSION WAYLAND_DISPLAY DISPLAY; do
    value="${!key-}"
    if [[ -n "$value" ]]; then
      printf 'Environment="%s=%s"\n' "$key" "$(escape_systemd_env "$value")"
    fi
  done
  focus_provider="$(detect_focus_provider)"
  if [[ -n "$focus_provider" ]]; then
    printf 'Environment="LULOD_FOCUS_PROVIDER=%s"\n' "$(escape_systemd_env "$focus_provider")"
  fi
} >"$dropin_path"

systemctl --user import-environment "${import_keys[@]}" || true
systemctl --user daemon-reload
systemctl --user enable --now lulod.service
systemctl --user restart lulod.service
systemctl --user status lulod.service --no-pager --lines=20

printf 'normal update cycle:\n'
printf '  cd %s\n' "$repo_dir"
printf '  make PREFIX=/usr\n'
printf '  sudo make install PREFIX=/usr\n'
printf '  ./install-lulod-user-service.sh\n'
printf '  systemctl --user restart lulod.service\n'
