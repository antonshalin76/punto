import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {createDbusImplementation} from
    '../gnome/punto-input-source@antonshalin76/dbusAdapter.js';
import {InputSourceBridgeCore} from
    '../gnome/punto-input-source@antonshalin76/inputSourceBridge.js';

const SHELL_NAME = 'org.gnome.Shell';
const BRIDGE_NAME = 'org.punto.Switcher.SessionBridge1';
const OBJECT_PATH = '/org/punto/Switcher/InputSource1';
const INTERFACE_NAME = 'org.punto.Switcher.InputSource1';
const loop = new GLib.MainLoop(null, false);
let exported = null;
let statusExported = null;

if (ARGV.length !== 4)
    throw new Error('usage: session_bridge_gjs_fixture.mjs PROTOCOL MARKER STATUS_MARKER STATE_DIRECTORY');

const marker = Gio.File.new_for_path(ARGV[1]);
const statusMarker = Gio.File.new_for_path(ARGV[2]);
const enrollmentMarker = Gio.File.new_for_path(`${ARGV[3]}/accessibility-enrolled`);
const statusCounts = {Get: 0, Set: 0, forbidden: 0, markerBeforeSet: false};
const counts = {BindHelper: 0, GetState: 0, Switch: 0, GetOperation: 0,
    activations: 0};

function saveCounts() {
    marker.replace_contents(
        new TextEncoder().encode(`${JSON.stringify(counts)}\n`), null, false,
        Gio.FileCreateFlags.REPLACE_DESTINATION, null);
}

function saveStatus() {
    statusMarker.replace_contents(
        new TextEncoder().encode(`${JSON.stringify(statusCounts)}\n`), null, false,
        Gio.FileCreateFlags.REPLACE_DESTINATION, null);
}

function digest(value) {
    return GLib.compute_checksum_for_string(
        GLib.ChecksumType.SHA256, value, -1);
}

function ownerOf(connection, name) {
    try {
        const reply = connection.call_sync(
            'org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'GetNameOwner',
            new GLib.Variant('(s)', [name]), new GLib.VariantType('(s)'),
            Gio.DBusCallFlags.NO_AUTO_START, 500, null);
        return reply.deepUnpack()[0];
    } catch (_error) {
        return '';
    }
}

function start(connection) {
    const acquired = connection.call_sync(
        'org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'RequestName',
        new GLib.Variant('(su)', ['org.a11y.Bus', 4]),
        new GLib.VariantType('(u)'), Gio.DBusCallFlags.NO_AUTO_START, 1000, null);
    if (acquired.deepUnpack()[0] !== 1)
        throw new Error('private accessibility status name unavailable');
    let accessibilityEnabled = false;
    statusExported = Gio.DBusExportedObject.wrapJSObject(
        '<node><interface name="org.a11y.Status">' +
        '<property name="IsEnabled" type="b" access="readwrite"/>' +
        '<property name="ScreenReaderEnabled" type="b" access="readwrite"/>' +
        '</interface></node>', {
            get IsEnabled() {
                statusCounts.Get += 1;
                saveStatus();
                return accessibilityEnabled;
            },
            set IsEnabled(value) {
                statusCounts.Set += 1;
                const info = enrollmentMarker.query_info(
                    'standard::type,standard::size,unix::mode,unix::uid',
                    Gio.FileQueryInfoFlags.NOFOLLOW_SYMLINKS, null);
                statusCounts.markerBeforeSet =
                    info.get_file_type() === Gio.FileType.REGULAR &&
                    info.get_size() === 0 &&
                    (info.get_attribute_uint32('unix::mode') & 0o777) === 0o600 &&
                    info.get_attribute_uint32('unix::uid') === Gio.Credentials.new().get_unix_user();
                if (value !== true || !statusCounts.markerBeforeSet) {
                    statusCounts.forbidden += 1;
                    saveStatus();
                    throw new Error('accessibility mutation without enrollment');
                }
                accessibilityEnabled = value;
                saveStatus();
            },
            get ScreenReaderEnabled() {
                statusCounts.forbidden += 1;
                saveStatus();
                throw new Error('screen reader access forbidden');
            },
            set ScreenReaderEnabled(_value) {
                statusCounts.forbidden += 1;
                saveStatus();
                throw new Error('screen reader access forbidden');
            },
        });
    statusExported.export(connection, '/org/a11y/bus');
    saveStatus();
    let activeId = 'us';
    let currentLayout = {id: 'us', layout: 'us', variant: ''};
    let core;
    const sources = ['us', 'ru'].map((id, index) => ({
        type: 'xkb', id, xkbId: id, index,
        activate(interactive) {
            if (interactive !== false)
                throw new Error('interactive activation is forbidden');
            activeId = id;
            currentLayout = {id, layout: id, variant: ''};
            counts.activations += 1;
            core.noteCurrentSourceChanged();
            saveCounts();
        },
    }));
    core = new InputSourceBridgeCore({
        nowUs: () => BigInt(GLib.get_monotonic_time()),
        randomId: () => '11'.repeat(16),
        helperOwner: () => ownerOf(connection, BRIDGE_NAME),
        snapshot: () => ({
            sources,
            currentSource: sources.find(source => source.id === activeId),
            currentLayout,
            locked: false,
        }),
        digestRequest: digest,
    });

    const [loaded, bytes] = GLib.file_get_contents(ARGV[0]);
    if (!loaded)
        throw new Error('protocol is unreadable');
    const node = Gio.DBusNodeInfo.new_for_xml(new TextDecoder().decode(bytes));
    const info = node.lookup_interface(INTERFACE_NAME);
    const implementation = createDbusImplementation(core);
    for (const method of ['BindHelper', 'GetState', 'Switch', 'GetOperation']) {
        const key = `${method}Async`;
        const original = implementation[key].bind(implementation);
        implementation[key] = (parameters, invocation) => {
            counts[method] += 1;
            saveCounts();
            original(parameters, invocation);
        };
    }
    exported = Gio.DBusExportedObject.wrapJSObject(info, implementation);
    exported.export(connection, OBJECT_PATH);
    saveCounts();
    print('READY');
}

const ownerId = Gio.bus_own_name(
    Gio.BusType.SESSION, SHELL_NAME, Gio.BusNameOwnerFlags.NONE,
    () => {}, connection => start(connection), () => loop.quit());
loop.run();
exported?.unexport();
statusExported?.unexport();
Gio.bus_unown_name(ownerId);
