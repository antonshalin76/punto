import Meta from 'gi://Meta';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Config from 'resource:///org/gnome/shell/misc/config.js';
import * as IBusManager from 'resource:///org/gnome/shell/misc/ibusManager.js';
import * as KeyboardManager from 'resource:///org/gnome/shell/misc/keyboardManager.js';

import {
    installKeyboardFreezeCompat,
    supportsGnome46X11,
} from './compat.js';

export default class PuntoInputSourceCompat extends Extension {
    enable() {
        this._patch = null;
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

        console.log(`${this.uuid}: GNOME 46 X11 compatibility active`);
    }

    disable() {
        if (this._patch !== null && !this._patch.disable())
            console.warn(`${this.uuid}: compatibility wrappers changed; safe restore skipped`);
        this._patch = null;
    }
}
