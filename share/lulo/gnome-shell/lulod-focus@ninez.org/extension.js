// Lulo Focus Reporter
//
// Mirrors share/lulo/kwin/lulod_focus_kde.js for GNOME Shell. GNOME (Mutter)
// does not expose the focused window's PID to external processes -- the
// Introspect D-Bus API is whitelist-gated and Shell.Eval is disabled in
// production -- so this small extension runs inside gnome-shell, watches the
// focus-window property, and publishes the focused PID on the session bus.
//
// The lulod-focus-gnome helper consumes org.ninez.LulodFocus and forwards the
// PID to the lulo scheduler daemon.

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const BUS_NAME = 'org.ninez.LulodFocus';
const OBJECT_PATH = '/LulodFocus';

const IFACE_XML = `
<node>
  <interface name="org.ninez.LulodFocus">
    <method name="GetFocusedPid">
      <arg type="i" name="pid" direction="out"/>
    </method>
    <signal name="FocusedPidChanged">
      <arg type="i" name="pid"/>
    </signal>
  </interface>
</node>`;

export default class LulodFocusExtension extends Extension {
    enable() {
        this._currentPid = -1;
        this._ownerId = 0;
        this._focusId = 0;

        this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(IFACE_XML, this);
        this._ownerId = Gio.bus_own_name(
            Gio.BusType.SESSION,
            BUS_NAME,
            Gio.BusNameOwnerFlags.REPLACE,
            connection => this._dbusImpl.export(connection, OBJECT_PATH),
            null,
            null);

        this._focusId = global.display.connect('notify::focus-window',
            () => this._onFocusChanged());
        this._onFocusChanged();
    }

    disable() {
        if (this._focusId) {
            global.display.disconnect(this._focusId);
            this._focusId = 0;
        }
        if (this._ownerId) {
            Gio.bus_unown_name(this._ownerId);
            this._ownerId = 0;
        }
        if (this._dbusImpl) {
            this._dbusImpl.unexport();
            this._dbusImpl = null;
        }
        this._currentPid = -1;
    }

    // Resolve the PID of the focused window, ignoring shell chrome / windowless
    // focus. Returns 0 when nothing reportable owns focus.
    _focusedPid() {
        const win = global.display.focus_window;
        if (!win)
            return 0;
        const pid = win.get_pid();
        return pid > 0 ? pid : 0;
    }

    _onFocusChanged() {
        const pid = this._focusedPid();
        if (pid === this._currentPid)
            return;
        this._currentPid = pid;
        if (this._dbusImpl) {
            this._dbusImpl.emit_signal('FocusedPidChanged',
                new GLib.Variant('(i)', [pid]));
        }
    }

    // D-Bus method: lets a freshly started helper learn the current focus
    // without waiting for the next change.
    GetFocusedPid() {
        return this._focusedPid();
    }
}
