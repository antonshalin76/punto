import {
    installKeyboardFreezeCompat,
    supportsGnome46X11,
} from '../gnome/punto-input-source@antonshalin76/compat.js';

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

function makeSurface(setEngine, freezeKeyboard) {
    const manager = Object.create({setEngine});
    const backend = Object.create({freeze_keyboard: freezeKeyboard});
    return {manager, backend};
}

async function testQualifyingActivation() {
    const calls = [];
    const expected = Promise.resolve('done');
    const releaseKeyboard = () => calls.push(['release']);
    const {manager, backend} = makeSurface(
        function setEngine(engine, callback) {
            calls.push(['engine', engine, callback, this]);
            return expected;
        },
        function freeze_keyboard(timestamp) {
            calls.push(['freeze', timestamp, this]);
        });
    const handle = installKeyboardFreezeCompat(
        manager, backend, releaseKeyboard);
    assert(handle !== null, 'compatible inherited API must be patched');
    assert(Object.hasOwn(manager, 'setEngine') &&
        Object.hasOwn(backend, 'freeze_keyboard'),
    'both inherited methods must be shadowed atomically');

    backend.freeze_keyboard(42);
    const result = manager.setEngine('xkb:us::eng', releaseKeyboard);
    assert(result === expected, 'original promise/return value changed');
    assert(calls.length === 1 && calls[0][0] === 'engine',
        'stock freeze or release was invoked');
    assert(calls[0][1] === 'xkb:us::eng' && calls[0][2] === undefined &&
        calls[0][3] === manager, 'engine argument or receiver changed');

    assert(handle.disable(), 'owned wrappers must restore cleanly');
    assert(!Object.hasOwn(manager, 'setEngine') &&
        !Object.hasOwn(backend, 'freeze_keyboard'),
    'disable must restore inherited property shape');
    assert(!handle.disable(), 'disable must be idempotent');
}

function testTransparentCalls() {
    const calls = [];
    const marker = {};
    const releaseKeyboard = () => calls.push(['release']);
    const unrelated = value => calls.push(['unrelated', value]);
    const {manager, backend} = makeSurface(
        function setEngine(engine, callback) {
            calls.push(['engine', engine, callback, arguments.length, this]);
            if (callback)
                callback('original-owner');
            return marker;
        },
        function freeze_keyboard(_timestamp) {});
    const handle = installKeyboardFreezeCompat(
        manager, backend, releaseKeyboard);
    assert(handle !== null, 'compatible API must be patched');

    assert(manager.setEngine('plain') === marker, 'callbackless return changed');
    assert(manager.setEngine('other', unrelated) === marker,
        'unrelated callback return changed');
    assert(calls.length === 3 && calls[0][3] === 1 && calls[0][4] === manager,
        'callbackless arguments or receiver changed');
    assert(calls[1][2] === unrelated && calls[1][3] === 2 &&
        calls[2][0] === 'unrelated' && calls[2][1] === 'original-owner',
    'unrelated callback arguments or timing changed');
    handle.disable();
}

function testFailClosedAndConflict() {
    const releaseKeyboard = () => {};
    const setEngine = function setEngine(engine, callback) {
        return [engine, callback];
    };
    const freezeKeyboard = function freeze_keyboard(timestamp) {
        return timestamp;
    };
    assert(installKeyboardFreezeCompat(
        null, {}, releaseKeyboard) === null, 'missing manager must fail closed');
    assert(installKeyboardFreezeCompat(
        {}, null, releaseKeyboard) === null, 'missing backend must fail closed');
    const wrongArity = makeSurface(engine => engine, freezeKeyboard);
    assert(installKeyboardFreezeCompat(
        wrongArity.manager, wrongArity.backend, releaseKeyboard) === null,
    'callback-free engine API must fail closed');

    const before = makeSurface(setEngine, freezeKeyboard);
    before.manager.setEngine = function competingEngine() {};
    assert(installKeyboardFreezeCompat(
        before.manager, before.backend, releaseKeyboard) === null,
    'pre-existing engine wrapper must not be replaced');
    assert(!Object.hasOwn(before.backend, 'freeze_keyboard'),
        'failed enable left a partial backend wrapper');

    const backendConflict = makeSurface(setEngine, freezeKeyboard);
    backendConflict.backend.freeze_keyboard = function competingFreeze() {};
    assert(installKeyboardFreezeCompat(
        backendConflict.manager, backendConflict.backend, releaseKeyboard) === null,
    'pre-existing backend wrapper must not be replaced');
    assert(!Object.hasOwn(backendConflict.manager, 'setEngine'),
        'failed enable left a partial engine wrapper');

    const after = makeSurface(setEngine, freezeKeyboard);
    const handle = installKeyboardFreezeCompat(
        after.manager, after.backend, releaseKeyboard);
    const foreign = function foreignWrapper() {};
    after.manager.setEngine = foreign;
    assert(installKeyboardFreezeCompat(
        after.manager, after.backend, releaseKeyboard) === null,
    'stale active handle must not report a foreign wrapper as patched');
    assert(!handle.disable(), 'disable must reject partial foreign ownership');
    assert(after.manager.setEngine === foreign &&
        Object.hasOwn(after.backend, 'freeze_keyboard'),
    'disable clobbered foreign state or restored only half the pair');

    for (const marker of [
        {},
        null,
        false,
        0,
        {isOwned() { return true; }},
        {isOwned() { throw new Error('foreign marker failure'); }},
    ]) {
        const marked = makeSurface(setEngine, freezeKeyboard);
        marked.manager[Symbol.for(
            'punto.gnome46.x11.keyboardFreezeCompat')] = marker;
        assert(installKeyboardFreezeCompat(
            marked.manager, marked.backend, releaseKeyboard) === null,
        'foreign marker must fail closed without throwing');
        assert(!Object.hasOwn(marked.backend, 'freeze_keyboard'),
            'foreign marker left a backend wrapper');
    }

    const accessor = makeSurface(setEngine, freezeKeyboard);
    Object.defineProperty(accessor.manager, Symbol.for(
        'punto.gnome46.x11.keyboardFreezeCompat'), {
        configurable: true,
        get() { throw new Error('foreign marker getter'); },
    });
    assert(installKeyboardFreezeCompat(
        accessor.manager, accessor.backend, releaseKeyboard) === null,
    'foreign marker accessor must fail closed without evaluation');
    assert(!Object.hasOwn(accessor.backend, 'freeze_keyboard'),
        'foreign marker accessor left a backend wrapper');
}

function testHandlePublicationRollback() {
    const errors = [];
    const prototype = {
        setEngine(engine, callback) { return [engine, callback]; },
    };
    const target = Object.create(prototype);
    const manager = new Proxy(target, {
        defineProperty(object, property, descriptor) {
            if (typeof property === 'symbol')
                throw new Error('marker rejected');
            return Reflect.defineProperty(object, property, descriptor);
        },
    });
    const backend = Object.create({
        freeze_keyboard(timestamp) { return timestamp; },
    });
    const handle = installKeyboardFreezeCompat(
        manager, backend, () => {}, error => errors.push(error));
    assert(handle === null, 'unpublished cleanup handle must fail install');
    assert(!Object.hasOwn(manager, 'setEngine') &&
        !Object.hasOwn(backend, 'freeze_keyboard'),
    'handle publication failure left partial wrappers');
    assert(errors.length === 1 && errors[0].includes('marker rejected'),
        'handle publication failure was not reported');
}

async function testExceptionsOverlapAndLateCompletion() {
    const releaseKeyboard = () => {};
    const syncError = new Error('original sync failure');
    const syncSurface = makeSurface(
        function setEngine(_engine, _callback) { throw syncError; },
        function freeze_keyboard(_timestamp) {});
    const syncHandle = installKeyboardFreezeCompat(
        syncSurface.manager, syncSurface.backend, releaseKeyboard);
    let observed = null;
    try {
        syncSurface.manager.setEngine('sync', releaseKeyboard);
    } catch (error) {
        observed = error;
    }
    assert(observed === syncError, 'original synchronous exception changed');
    syncHandle.disable();

    const completions = [];
    const calls = [];
    const asyncSurface = makeSurface(
        function setEngine(engine, callback) {
            calls.push([engine, callback]);
            return new Promise(resolve => completions.push(resolve));
        },
        function freeze_keyboard(_timestamp) {
            throw new Error('stock freeze called');
        });
    const handle = installKeyboardFreezeCompat(
        asyncSurface.manager, asyncSurface.backend, releaseKeyboard);
    asyncSurface.backend.freeze_keyboard(1);
    const first = asyncSurface.manager.setEngine('first', releaseKeyboard);
    asyncSurface.backend.freeze_keyboard(2);
    const second = asyncSurface.manager.setEngine('second', releaseKeyboard);
    assert(calls.length === 2 && calls.every(([, callback]) => callback === undefined),
        'overlapping activations were reordered, lost, or kept late callbacks');
    assert(installKeyboardFreezeCompat(
        asyncSurface.manager, asyncSurface.backend, releaseKeyboard) === handle,
    'repeated enable nested wrappers');
    assert(handle.disable(), 'disable during pending operations failed');
    completions[1]('second');
    completions[0]('first');
    assert(await first === 'first' && await second === 'second',
        'late/out-of-order original completion changed');
}

function testEnvironmentGuard() {
    assert(supportsGnome46X11('46', false), 'GNOME 46 X11 must be supported');
    assert(supportsGnome46X11('46.0', false), 'GNOME 46.0 X11 must be supported');
    assert(!supportsGnome46X11('46.0', true), 'Wayland must remain unpatched');
    assert(!supportsGnome46X11('45.9', false), 'GNOME 45 must remain unpatched');
    assert(!supportsGnome46X11('460', false), 'version parsing must be exact');
    assert(!supportsGnome46X11('47.0', false), 'GNOME 47 must remain unpatched');
}

await testQualifyingActivation();
testTransparentCalls();
testFailClosedAndConflict();
testHandlePublicationRollback();
await testExceptionsOverlapAndLateCompletion();
testEnvironmentGuard();
print('test_gnome46_x11_compat: OK');
