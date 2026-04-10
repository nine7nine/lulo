# Lulo Install

This project supports a normal `/usr` install layout directly from the existing `Makefile`.

## Build Targets

| Command | Purpose |
| --- | --- |
| `make PREFIX=/usr` | Standard build |
| `make strict PREFIX=/usr` | Stricter warnings build |
| `make analyze PREFIX=/usr` | Static analysis build |
| `make asan PREFIX=/usr` | Sanitizer build |
| `make print-install-paths PREFIX=/usr` | Show resolved install paths |

## Installed Layout

| Path | Contents |
| --- | --- |
| `/usr/bin` | `lulo`, `lulod`, `lulod-system` |
| `/usr/libexec/lulo` | `lulo-admin`, `lulod-focus-kde` |
| `/usr/share/lulo` | Runtime data, KWin script, example scheduler config |
| `/usr/lib/systemd/user` | `lulod.service` |
| `/usr/lib/systemd/system` | `lulod-system.service` |
| `/usr/share/polkit-1/actions` | Polkit policy |
| `/etc/lulo/scheduler` | Default scheduler config |

## First Install

```bash
cd /path/to/lulo
make PREFIX=/usr
sudo make install PREFIX=/usr
./install-lulod-user-service.sh
./install-lulod-system-service.sh
/usr/bin/lulo -i nc
```

This is the simplest path. The helper scripts detect the installed `/usr` layout and use it.

## Vendor Unit Flow

If you want to rely on the installed systemd units directly:

```bash
cd /path/to/lulo
make PREFIX=/usr
sudo make install PREFIX=/usr
sudo systemctl daemon-reload
systemctl --user daemon-reload
sudo systemctl enable --now lulod-system.service
systemctl --user enable --now lulod.service
./install-lulod-user-service.sh
/usr/bin/lulo -i nc
```

## Migrating Away From Old Repo-Local Units

| Scope | Cleanup |
| --- | --- |
| User service | `systemctl --user disable --now lulod.service || true` then remove `~/.config/systemd/user/lulod.service` and reload user units |
| System service | `sudo systemctl disable --now lulod-system.service || true` then remove `/etc/systemd/system/lulod-system.service` and reload system units |

Commands:

```bash
systemctl --user disable --now lulod.service || true
rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/lulod.service"
systemctl --user daemon-reload

sudo systemctl disable --now lulod-system.service || true
sudo rm -f /etc/systemd/system/lulod-system.service
sudo systemctl daemon-reload
```

Then enable the installed units:

```bash
sudo systemctl enable --now lulod-system.service
systemctl --user enable --now lulod.service
```

## Updating

If the install path is unchanged:

```bash
cd /path/to/lulo
make PREFIX=/usr
sudo make install PREFIX=/usr
sudo systemctl restart lulod-system.service
systemctl --user restart lulod.service
```

If your session environment changed significantly, rerun:

```bash
./install-lulod-user-service.sh
```

## Reset Scheduler Config

To replace `/etc/lulo/scheduler` with the shipped example config:

```bash
cd /path/to/lulo
./reset-lulod-system-config.sh
sudo systemctl restart lulod-system.service
```

## Packaging / Staging

```bash
make install PREFIX=/usr DESTDIR=/tmp/pkgroot
```
