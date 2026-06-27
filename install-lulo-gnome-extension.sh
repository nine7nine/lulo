#!/usr/bin/env bash
set -euo pipefail

# Installs and enables the "Lulo Focus Reporter" GNOME Shell extension, the
# GNOME counterpart to the KWin focus script. It publishes the focused window
# PID on the session bus (org.ninez.LulodFocus) for lulod-focus-gnome to relay
# to the scheduler.
#
# GNOME loads extensions only from its own extension directories, so unlike the
# KWin script (loaded at runtime over D-Bus) this must be copied into the user
# extensions dir and enabled once.

uuid="lulod-focus@ninez.org"
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
dest_dir="$data_home/gnome-shell/extensions/$uuid"
datadir="${LULO_DATADIR:-/usr/share/lulo}"

# Prefer the extension shipped in this checkout, fall back to an installed copy.
src_dir=""
for candidate in \
  "$repo_dir/share/lulo/gnome-shell/$uuid" \
  "$datadir/gnome-shell/$uuid"; do
  if [[ -f "$candidate/metadata.json" && -f "$candidate/extension.js" ]]; then
    src_dir="$candidate"
    break
  fi
done

if [[ -z "$src_dir" ]]; then
  printf 'error: could not find %s extension source\n' "$uuid" >&2
  exit 1
fi

mkdir -p "$dest_dir"
install -m644 "$src_dir/metadata.json" "$src_dir/extension.js" "$dest_dir/"
printf 'installed %s -> %s\n' "$uuid" "$dest_dir"

if ! command -v gnome-extensions >/dev/null 2>&1; then
  printf 'gnome-extensions CLI not found; enable %s manually.\n' "$uuid" >&2
  exit 0
fi

# A freshly copied extension is not in the list until the shell rescans; enabling
# by uuid still registers it for the next load. On X11 a shell restart applies it
# immediately; on Wayland, log out and back in.
if gnome-extensions enable "$uuid" 2>/dev/null; then
  printf 'enabled %s\n' "$uuid"
else
  printf 'could not enable %s automatically; run: gnome-extensions enable %s\n' "$uuid" "$uuid" >&2
fi

printf '\n'
printf 'On Wayland, log out and back in (or re-run "gnome-extensions enable %s")\n' "$uuid"
printf 'so GNOME Shell loads the extension, then restart the daemon:\n'
printf '  systemctl --user restart lulod.service\n'
