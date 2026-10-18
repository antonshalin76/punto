import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {createDbusImplementation} from
    '../gnome/punto-input-source@antonshalin76/dbusAdapter.js';
import {
    canonicalRequestText,
    InputSourceBridgeCore,
    Outcome,
    Protocol,
    wireUint64,
} from '../gnome/punto-input-source@antonshalin76/inputSourceBridge.js';

const BUS_NAME = 'org.punto.Switcher.AdapterDispatchTest1';
const OBJECT_PATH = '/org/punto/Switcher/InputSource1';
const INTERFACE_NAME = 'org.punto.Switcher.InputSource1';
const HELPER = '22'.repeat(16);
const CLIENT = '33'.repeat(16);
const loop = new GLib.MainLoop(null, false);
let exported = null;
let exitCode = 1;
let timeoutId = 0;

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

function digest(value) {
    return GLib.compute_checksum_for_string(
        GLib.ChecksumType.SHA256, value, -1);
}

function call(connection, method, signature, values, replySignature) {
    return new Promise((resolve, reject) => {
        connection.call(
            BUS_NAME, OBJECT_PATH, INTERFACE_NAME, method,
            new GLib.Variant(signature, values),
            new GLib.VariantType(replySignature), Gio.DBusCallFlags.NONE,
            1500, null, (source, result) => {
                try {
                    resolve(source.call_finish(result).deepUnpack());
                } catch (error) {
                    reject(error);
                }
            });
    });
}

async function run(connection) {
    let nowUs = 1_000_000n;
    let activeId = 'us';
    let currentLayout = {id: 'us', layout: 'us', variant: ''};
    let activations = 0;
    let core;
    const sources = ['us', 'ru'].map((id, index) => ({
        type: 'xkb',
        id,
        xkbId: id,
        index,
        activate(interactive) {
            assert(interactive === false,
                'adapter must preserve noninteractive activation');
            activations += 1;
            activeId = id;
            currentLayout = {id, layout: id, variant: ''};
            core.noteCurrentSourceChanged();
        },
    }));
    core = new InputSourceBridgeCore({
        nowUs: () => nowUs,
        randomId: () => '11'.repeat(16),
        helperOwner: () => connection.get_unique_name(),
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
        throw new Error('canonical protocol is unreadable');
    const nodeInfo = Gio.DBusNodeInfo.new_for_xml(
        new TextDecoder().decode(bytes));
    const interfaceInfo = nodeInfo.lookup_interface(INTERFACE_NAME);
    if (interfaceInfo === null)
        throw new Error('input-source interface is missing');
    exported = Gio.DBusExportedObject.wrapJSObject(
        interfaceInfo, createDbusImplementation(core));
    exported.export(connection, OBJECT_PATH);

    const bind = await call(connection, 'BindHelper', '(sut)', [
        HELPER, Protocol.Version, nowUs + 500_000n,
    ], '(us)');
    assert(bind[0] === Outcome.State && bind[1] === '11'.repeat(16),
        'real BindHelper dispatch failed');

    const versionRejected = await call(connection, 'BindHelper', '(sut)', [
        HELPER, Protocol.Version + 1, nowUs + 500_000n,
    ], '(us)');
    assert(versionRejected[0] === Outcome.VersionMismatch,
        'version rejection did not cross the production adapter');

    const state = await call(connection, 'GetState', '(sst)', [
        HELPER, CLIENT, nowUs + 500_000n,
    ], '(ussttty)');
    assert(state[0] === Outcome.State && state[1] === '11'.repeat(16) &&
        state[2].length === 32 && wireUint64(state[3]) === 31_000_000n &&
        wireUint64(state[4]) === 1n && wireUint64(state[5]) === 1n &&
        state[6] === Protocol.Role.EN,
    'GetState dispatch lost identity, uint64 fields or role');

    const request = {
        extensionInstance: state[1],
        expectedSourceGeneration: wireUint64(state[4]),
        expectedStateGeneration: wireUint64(state[5]),
        expectedRole: state[6],
        targetRole: Protocol.Role.RU,
    };
    const requestDigest = digest(canonicalRequestText(request));
    const switchValues = [
        HELPER, state[1], CLIENT, state[2], 1n, requestDigest,
        nowUs + 500_000n, request.expectedSourceGeneration,
        request.expectedStateGeneration, request.expectedRole,
        request.targetRole,
    ];
    const switched = await call(connection, 'Switch', '(sssststttyy)',
        switchValues, '(usstty)');
    assert(switched[0] === Outcome.GnomeStateApplied &&
        switched[1] === state[1] && switched[2] === requestDigest &&
        wireUint64(switched[3]) === 1n &&
        wireUint64(switched[4]) === 2n &&
        switched[5] === Protocol.Role.RU && activations === 1,
    'Switch dispatch did not preserve terminal identity or activation');

    const operation = await call(connection, 'GetOperation', '(sssstst)', [
        HELPER, state[1], CLIENT, state[2], 1n, requestDigest,
        nowUs + 500_000n,
    ], '(usstty)');
    assert(operation.join(',') === switched.join(',') && activations === 1,
        'GetOperation did not return the stored terminal operation');

    const replay = await call(connection, 'Switch', '(sssststttyy)',
        switchValues, '(usstty)');
    assert(replay.join(',') === switched.join(',') && activations === 1,
        'identical Switch replay activated the source twice');

    const badDigest = await call(connection, 'Switch', '(sssststttyy)', [
        HELPER, state[1], CLIENT, state[2], 2n, 'aa'.repeat(32),
        nowUs + 500_000n, 1n, 2n, Protocol.Role.RU, Protocol.Role.EN,
    ], '(usstty)');
    assert(badDigest[0] === Outcome.Conflict && activations === 1,
        'bad digest was accepted or changed the desktop source');

    const missing = await call(connection, 'GetOperation', '(sssstst)', [
        HELPER, state[1], CLIENT, state[2], 99n, requestDigest,
        nowUs + 500_000n,
    ], '(usstty)');
    assert(missing[0] === Outcome.NotFound && activations === 1,
        'missing operation query was accepted or changed the source');

    print('test_gjs_dbus_adapter_dispatch: OK');
}

if (ARGV.length !== 1)
    throw new Error('usage: test_gjs_dbus_adapter_dispatch.mjs PROTOCOL_XML');

timeoutId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 8000, () => {
    printerr('test_gjs_dbus_adapter_dispatch: timed out');
    loop.quit();
    return GLib.SOURCE_REMOVE;
});
const ownerId = Gio.bus_own_name(
    Gio.BusType.SESSION,
    BUS_NAME,
    Gio.BusNameOwnerFlags.NONE,
    () => {},
    connection => {
        run(connection).then(() => {
            exitCode = 0;
        }).catch(error => {
            printerr(error.message);
        }).finally(() => loop.quit());
    },
    () => loop.quit());

loop.run();
if (timeoutId !== 0)
    GLib.source_remove(timeoutId);
exported?.unexport();
Gio.bus_unown_name(ownerId);
if (exitCode !== 0)
    throw new Error('production D-Bus adapter dispatch test failed');
