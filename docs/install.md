# Lulo -- Install & Packaging

Lulo builds from a single `Makefile` into a standard `/usr` layout: three binaries on `PATH`, three helpers in `libexec`, two systemd units (one user, one system), a polkit policy, and the default scheduler config -- plus a one-time GNOME Shell extension step when you want focus tracking on GNOME.

## Table of Contents

1. [Build targets](#1-build-targets)
2. [Installed layout](#2-installed-layout)
3. [First install](#3-first-install)
4. [The one-command path](#4-the-one-command-path)
5. [GNOME focus provider](#5-gnome-focus-provider)
6. [Services and privilege](#6-services-and-privilege)
7. [Running lulo](#7-running-lulo)
8. [Updating, resetting, and migrating](#8-updating-resetting-and-migrating)
9. [Packaging](#9-packaging)
10. [See also](#10-see-also)

---

## 1. Build targets

The build is a plain `make` with `PREFIX` controlling the install root.

| Command | Purpose |
| --- | --- |
| `make PREFIX=/usr` | Standard build |
| `make strict PREFIX=/usr` | Build with stricter warnings |
| `make analyze PREFIX=/usr` | Static analysis pass |
| `make asan PREFIX=/usr` | Sanitizer build |
| `make print-install-paths PREFIX=/usr` | Show the resolved install paths |

## 2. Installed layout

`make install` lays the project out under `PREFIX` (here `/usr`) plus system config under `/etc`.

<div class="diagram-container">
<svg width="100%" viewBox="0 0 980 320" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg      { fill: #1a1b26; }
    .bin     { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .lib     { fill: #1a2235; stroke: #7aa2f7; stroke-width: 1.5; }
    .sys     { fill: #2a1f35; stroke: #bb9af7; stroke-width: 1.5; }
    .data    { fill: #24283b; stroke: #3b4261; stroke-width: 1; }
    .lbl-sm  { fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut { fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn { fill: #9ece6a; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-blu { fill: #7aa2f7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-pur { fill: #bb9af7; font-size: 10px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .title   { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="980" height="320" class="bg"/>
  <text x="490" y="26" text-anchor="middle" class="title">Installed footprint</text>

  <rect x="30"  y="56" width="300" height="84" class="bin"/>
  <text x="46" y="76" class="lbl-grn">/usr/bin</text>
  <text x="46" y="94"  class="lbl-mut">lulo  (TUI)</text>
  <text x="46" y="108" class="lbl-mut">lulod  (session daemon)</text>
  <text x="46" y="122" class="lbl-mut">lulod-system  (privileged daemon)</text>

  <rect x="350" y="56" width="300" height="84" class="lib"/>
  <text x="366" y="76" class="lbl-blu">/usr/libexec/lulo</text>
  <text x="366" y="94"  class="lbl-mut">lulo-admin  (pkexec helper)</text>
  <text x="366" y="108" class="lbl-mut">lulod-focus-kde</text>
  <text x="366" y="122" class="lbl-mut">lulod-focus-gnome</text>

  <rect x="670" y="56" width="280" height="84" class="data"/>
  <text x="686" y="76" class="lbl-sm">/usr/share/lulo</text>
  <text x="686" y="94"  class="lbl-mut">KWin focus script</text>
  <text x="686" y="108" class="lbl-mut">GNOME Shell extension</text>
  <text x="686" y="122" class="lbl-mut">example scheduler config</text>

  <rect x="30"  y="164" width="300" height="68" class="sys"/>
  <text x="46" y="184" class="lbl-pur">/usr/lib/systemd</text>
  <text x="46" y="202" class="lbl-mut">user/lulod.service</text>
  <text x="46" y="216" class="lbl-mut">system/lulod-system.service</text>

  <rect x="350" y="164" width="300" height="68" class="data"/>
  <text x="366" y="184" class="lbl-sm">/usr/share/polkit-1/actions</text>
  <text x="366" y="202" class="lbl-mut">io.lulo.system.unlock-rw</text>
  <text x="366" y="216" class="lbl-mut">io.lulo.admin.pkexec.apply-tune</text>

  <rect x="670" y="164" width="280" height="68" class="data"/>
  <text x="686" y="184" class="lbl-sm">/etc/lulo/scheduler</text>
  <text x="686" y="202" class="lbl-mut">scheduler.conf</text>
  <text x="686" y="216" class="lbl-mut">profiles.d / rules.d</text>
</svg>
</div>

| Path | Contents |
| --- | --- |
| `/usr/bin` | `lulo`, `lulod`, `lulod-system` |
| `/usr/libexec/lulo` | `lulo-admin`, `lulod-focus-kde`, `lulod-focus-gnome` |
| `/usr/share/lulo` | Runtime data, KWin focus script, GNOME Shell extension, example scheduler config |
| `/usr/lib/systemd/user` | `lulod.service` |
| `/usr/lib/systemd/system` | `lulod-system.service` |
| `/usr/share/polkit-1/actions` | Polkit policy (the two actions) |
| `/etc/lulo/scheduler` | Default scheduler config |

## 3. First install

The simplest path builds, installs, sets up the user and system services, and launches the TUI:

```bash
cd /path/to/lulo
make PREFIX=/usr
sudo make install PREFIX=/usr
./install-lulod-user-service.sh
./install-lulod-system-service.sh
/usr/bin/lulo -i nc
```

The helper scripts detect the installed `/usr` layout and use it. `install-lulod-user-service.sh` also auto-detects your desktop and, on GNOME, sets `LULOD_FOCUS_PROVIDER=gnome` for the user service.

## 4. The one-command path

For a single command that builds, installs to `/usr`, wires up both services, and (on GNOME) installs the focus extension:

```bash
cd /path/to/lulo
./install-system.sh
```

## 5. GNOME focus provider

KDE loads its focus script at runtime, but GNOME Shell only loads extensions from its own directories, so the GNOME focus provider needs a one-time extension install (see [Focus Providers](focus-providers.gen.html) for why):

```bash
./install-lulo-gnome-extension.sh   # copies + enables lulod-focus@ninez.org for your user
```

On Wayland, log out and back in so GNOME Shell loads the extension, then `systemctl --user restart lulod.service`. The provider can always be overridden with `LULOD_FOCUS_PROVIDER` (`kde`, `gnome`, `none`, or `auto`).

## 6. Services and privilege

Lulo runs two long-lived services with very different privilege.

| Unit | Scope | Role |
| --- | --- | --- |
| `lulod.service` | systemd **user** unit | The unprivileged session daemon: snapshot caches and focus integration |
| `lulod-system.service` | systemd **system** unit | The privileged daemon: scheduler enforcement, privileged edits, tracing |

If you prefer to rely on the installed vendor units directly:

```bash
sudo systemctl daemon-reload
systemctl --user daemon-reload
sudo systemctl enable --now lulod-system.service
systemctl --user enable --now lulod.service
```

The privileged daemon is the only component that runs as root; the TUI stays unprivileged and reaches privilege through the RW lease or the `pkexec` helper (see [Process Model & IPC](process-model-ipc.gen.html)). The two polkit actions installed under `/usr/share/polkit-1/actions` govern those two paths.

## 7. Running lulo

```bash
/usr/bin/lulo -i nc
```

The `-i` flag selects the input backend; `LULO_INPUT` is the environment equivalent. Lulo supports a Notcurses input backend and a raw-terminal backend, auto-selecting based on `TERM` when not told otherwise -- `-i nc` forces the Notcurses backend, which is the right choice on VTE/GNOME terminals. Inside the TUI, `?` shows a per-page help overlay, `Tab` switches pages, `Shift+Tab` cycles subviews, and `Shift+W` unlocks RW mode.

## 8. Updating, resetting, and migrating

To update in place when the install path is unchanged:

```bash
cd /path/to/lulo
make PREFIX=/usr
sudo make install PREFIX=/usr
sudo systemctl restart lulod-system.service
systemctl --user restart lulod.service
```

If your session environment changed significantly, rerun `./install-lulod-user-service.sh`. To replace `/etc/lulo/scheduler` with the shipped example config:

```bash
./reset-lulod-system-config.sh
sudo systemctl restart lulod-system.service
```

If you previously used old repo-local systemd units, disable and remove them before enabling the installed ones (the user unit under `~/.config/systemd/user/lulod.service`, the system unit under `/etc/systemd/system/lulod-system.service`), then `daemon-reload` each scope.

## 9. Packaging

For staged / distro packaging, install into a `DESTDIR` root:

```bash
make install PREFIX=/usr DESTDIR=/tmp/pkgroot
```

`make print-install-paths PREFIX=/usr` shows exactly where each artifact will land, which is useful when authoring a package manifest.

## 10. See also

- [Architecture Overview](architecture.gen.html)
- [Process Model & IPC](process-model-ipc.gen.html)
- [Focus Providers](focus-providers.gen.html)
- [Scheduler](scheduler.gen.html)
