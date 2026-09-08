const PATCH_HANDLE = Symbol.for('punto.gnome46.x11.keyboardFreezeCompat');
const OWNED_HANDLES = new WeakSet();

export function supportsGnome46X11(shellVersion, isWayland) {
    return !isWayland && /^46(?:[.]|$)/.test(shellVersion);
}

function inheritedMethod(target, name, arity) {
    if (target === null || typeof target !== 'object' || Object.hasOwn(target, name))
        return null;

    let prototype = Object.getPrototypeOf(target);
    while (prototype !== null) {
        const descriptor = Object.getOwnPropertyDescriptor(prototype, name);
        if (descriptor !== undefined) {
            if (!Object.hasOwn(descriptor, 'value') ||
                typeof descriptor.value !== 'function' ||
                descriptor.value.length !== arity ||
                target[name] !== descriptor.value)
                return null;
            return descriptor.value;
        }
        prototype = Object.getPrototypeOf(prototype);
    }
    return null;
}

export function installKeyboardFreezeCompat(
    manager, backend, releaseKeyboard,
    reportError = error => console.error(error)) {
    if (manager === null || typeof manager !== 'object' ||
        backend === null || typeof backend !== 'object' ||
        typeof releaseKeyboard !== 'function')
        return null;

    let activeHandleDescriptor;
    try {
        activeHandleDescriptor = Object.getOwnPropertyDescriptor(
            manager, PATCH_HANDLE);
    } catch (_error) {
        return null;
    }
    if (activeHandleDescriptor !== undefined) {
        if (!Object.hasOwn(activeHandleDescriptor, 'value'))
            return null;
        const activeHandle = activeHandleDescriptor.value;
        try {
            return activeHandle !== null &&
                typeof activeHandle === 'object' &&
                OWNED_HANDLES.has(activeHandle) &&
                typeof activeHandle.isOwned === 'function' &&
                activeHandle.isOwned() ? activeHandle : null;
        } catch (_error) {
            return null;
        }
    }

    const originalSetEngine = inheritedMethod(manager, 'setEngine', 2);
    const originalFreeze = inheritedMethod(backend, 'freeze_keyboard', 1);
    if (originalSetEngine === null || originalFreeze === null)
        return null;

    const freezeWrapper = function freeze_keyboard(_timestamp) {};
    const setEngineWrapper = function setEngine(engine, callback) {
        if (callback !== releaseKeyboard)
            return Reflect.apply(originalSetEngine, this, arguments);
        return Reflect.apply(originalSetEngine, this, [engine]);
    };

    try {
        Object.defineProperty(backend, 'freeze_keyboard', {
            configurable: true,
            writable: true,
            value: freezeWrapper,
        });
        Object.defineProperty(manager, 'setEngine', {
            configurable: true,
            writable: true,
            value: setEngineWrapper,
        });
    } catch (error) {
        if (backend.freeze_keyboard === freezeWrapper)
            delete backend.freeze_keyboard;
        reportError(`Punto GNOME 46 X11 compatibility install failed: ${error}`);
        return null;
    }

    if (backend.freeze_keyboard !== freezeWrapper ||
        manager.setEngine !== setEngineWrapper) {
        if (manager.setEngine === setEngineWrapper)
            delete manager.setEngine;
        if (backend.freeze_keyboard === freezeWrapper)
            delete backend.freeze_keyboard;
        return null;
    }

    const handle = Object.freeze({
        isOwned() {
            return manager.setEngine === setEngineWrapper &&
                backend.freeze_keyboard === freezeWrapper;
        },
        disable() {
            if (!this.isOwned())
                return false;

            delete manager.setEngine;
            delete backend.freeze_keyboard;
            delete manager[PATCH_HANDLE];
            return !Object.hasOwn(manager, 'setEngine') &&
                !Object.hasOwn(backend, 'freeze_keyboard');
        },
    });
    OWNED_HANDLES.add(handle);
    try {
        Object.defineProperty(manager, PATCH_HANDLE, {
            configurable: true,
            value: handle,
        });
    } catch (error) {
        if (manager.setEngine === setEngineWrapper)
            delete manager.setEngine;
        if (backend.freeze_keyboard === freezeWrapper)
            delete backend.freeze_keyboard;
        reportError(`Punto GNOME 46 X11 compatibility publication failed: ${error}`);
        return null;
    }
    return handle;
}
