#!/usr/bin/env bash
set -euo pipefail

# One-shot system install: build, install to $PREFIX (default /usr), set up the
# lulod user + system services, and (on GNOME) install the focus extension.
#
# Override the prefix with:  PREFIX=/usr/local ./install-system.sh

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_dir"

PREFIX="${PREFIX:-/usr}"
bindir="$PREFIX/bin"
datadir="$PREFIX/share/lulo"

echo "==> building (PREFIX=$PREFIX)"
make PREFIX="$PREFIX"

echo "==> installing to system (sudo)"
sudo make install PREFIX="$PREFIX"

# GNOME focus extension is per-user; only meaningful inside a GNOME session.
desktop="${XDG_CURRENT_DESKTOP:-}${XDG_SESSION_DESKTOP:-}${DESKTOP_SESSION:-}"
shopt -s nocasematch
if [[ "$desktop" == *gnome* ]]; then
  echo "==> installing GNOME focus extension for $USER"
  LULO_DATADIR="$datadir" ./install-lulo-gnome-extension.sh || true
else
  echo "==> not a GNOME session; skipping GNOME focus extension"
fi
shopt -u nocasematch

echo "==> setting up lulod user service"
LULO_BINDIR="$bindir" LULO_DATADIR="$datadir" ./install-lulod-user-service.sh

echo "==> setting up lulod-system service"
./install-lulod-system-service.sh

cat <<EOF

Done.
- Focused-app scheduling uses the auto-detected provider (KDE or GNOME).
- On GNOME/Wayland, log out and back in so GNOME Shell loads the focus
  extension, then: systemctl --user restart lulod.service
- Launch the TUI with: $bindir/lulo -i nc
EOF
