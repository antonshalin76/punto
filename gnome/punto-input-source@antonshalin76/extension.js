import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as IBusManager from 'resource:///org/gnome/shell/misc/ibusManager.js';
import * as KeyboardManager from 'resource:///org/gnome/shell/misc/keyboardManager.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as InputSourceManager from 'resource:///org/gnome/shell/ui/status/keyboard.js';

import {
    installKeyboardFreezeCompat,
    supportsGnome46X11,
} from './compat.js';
import {
    InputSourceBridgeCore,
} from './inputSourceBridge.js';
import {createDbusImplementation} from './dbusAdapter.js';

const HELPER_BUS_NAME = 'org.punto.Switcher.SessionBridge1';
const OBJECT_PATH = '/org/punto/Switcher/InputSource1';
const INTERFACE_NAME = 'org.punto.Switcher.InputSource1';

function monotonicUs() {
    return BigInt(GLib.get_monotonic_time());
}

function randomId() {
    return GLib.uuid_string_random().replaceAll('-', '');
}

function digestRequest(value) {
    return GLib.compute_checksum_for_string(GLib.ChecksumType.SHA256, value, -1);
}

export default class PuntoInputSourceCompat extends Extension {
    enable() {
        this._patch = null;
        this._bridge = null;
        this._bridgeCore = null;
        this._helperWatch = 0;
        this._sourceSignals = [];
        this._helperOwner = '';
        this._inputSourceManager = null;
        if (!supportsGnome46X11(
            Config.PACKAGE_VERSION, Meta.is_wayland_compositor()))
            return;

        this._patch = installKeyboardFreezeCompat(
            IBusManager.getIBusManager(),
            global.backend,
            KeyboardManager.releaseKeyboard);
        if (this._patch === null) {
            console.warn(`${this.uuid}: unsupported or already patched GNOME API`);
            return;
        }

        let bridgeReady = false;
        try {
            this._enableBridge();
            bridgeReady = true;
        } catch (error) {
            this._disableBridge();
            console.error(`${this.uuid}: input-source bridge unavailable ` +
                `(${error instanceof Error ? error.name : 'unknown error'})`);
        }
        console.log(`${this.uuid}: GNOME 46 X11 compatibility active; ` +
            `input-source bridge ${bridgeReady ? 'active' : 'unavailable'}`);
    }

    disable() {
        this._disableBridge();
        if (this._patch !== null && !this._patch.disable())
            console.warn(`${this.uuid}: compatibility wrappers changed; safe restore skipped`);
        this._patch = null;
    }

    _enableBridge() {
        const manager = InputSourceManager.getInputSourceManager();
        this._inputSourceManager = manager;
        const snapshot = () => ({
            sources: Object.values(manager.inputSources),
            currentSource: manager.currentSource,
            currentLayout: manager.keyboardManager.currentLayout,
            locked: Main.sessionMode.isLocked || Main.sessionMode.isGreeter,
        });
        this._bridgeCore = new InputSourceBridgeCore({
            nowUs: monotonicUs,
            randomId,
            helperOwner: () => this._helperOwner,
            snapshot,
            digestRequest,
        });
        this._sourceSignals.push([
            'sources-changed',
            manager.connect('sources-changed', () =>
                this._bridgeCore?.noteSourcesChanged()),
        ]);
        this._sourceSignals.push([
            'current-source-changed',
            manager.connect('current-source-changed', () =>
                this._bridgeCore?.noteCurrentSourceChanged()),
        ]);

        const connection = Gio.DBus.session;
        this._helperWatch = Gio.bus_watch_name_on_connection(
            connection,
            HELPER_BUS_NAME,
            Gio.BusNameWatcherFlags.NONE,
            (_connection, _name, owner) => this._setHelperOwner(owner),
            () => this._setHelperOwner(''));

        const protocolPath = GLib.build_filenamev([
            this.path,
            'punto-dbus-v1.xml',
        ]);
        const [loaded, bytes] = GLib.file_get_contents(protocolPath);
        if (!loaded)
            throw new Error('protocol file is not readable');
        const nodeInfo = Gio.DBusNodeInfo.new_for_xml(
            new TextDecoder().decode(bytes));
        const interfaceInfo = nodeInfo.lookup_interface(INTERFACE_NAME);
        if (interfaceInfo === null)
            throw new Error('extension protocol interface is missing');
        this._bridge = Gio.DBusExportedObject.wrapJSObject(
            interfaceInfo, createDbusImplementation(this._bridgeCore));
        this._bridge.export(connection, OBJECT_PATH);
    }

    _setHelperOwner(owner) {
        if (this._helperOwner === owner)
            return;
        this._helperOwner = owner;
        this._bridgeCore?.noteHelperOwnerChanged();
    }

    _disableBridge() {
        if (this._bridgeCore !== null)
            this._bridgeCore.disable();
        if (this._bridge !== null)
            this._bridge.unexport();
        this._bridge = null;
        if (this._helperWatch !== 0)
            Gio.bus_unwatch_name(this._helperWatch);
        this._helperWatch = 0;
        if (this._inputSourceManager !== null) {
            for (const [, signal] of this._sourceSignals ?? [])
                this._inputSourceManager.disconnect(signal);
        }
        this._sourceSignals = [];
        this._inputSourceManager = null;
        this._bridgeCore = null;
        this._helperOwner = '';
    }
}
