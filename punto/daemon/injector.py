from __future__ import annotations

import logging


import evdev  # type: ignore
from punto.core.layout_utils import get_sequence_for_char

logger = logging.getLogger(__name__)


class Injector:
    def __init__(self) -> None:
        """
        Initialize the virtual keyboard injector using uinput.
        """
        self._ui: evdev.UInput | None = None
        self._ensure_uinput()

    def _ensure_uinput(self) -> None:
        if self._ui is None:
            try:
                # Create a virtual keyboard with all key capabilities
                cap = {
                    evdev.ecodes.EV_KEY: evdev.ecodes.keys.keys(),
                }
                # Lctrl+Shift etc. will be used in future for other actions
                # lctrl = evdev.ecodes.KEY_LEFTCTRL in self.active_modifiers
                # lshift = evdev.ecodes.KEY_LEFTSHIFT in self.active_modifiers
                self._ui = evdev.UInput(cap, name="punto-virtual-keyboard", version=0x1)
                logger.info("Virtual keyboard initialized.")
            except Exception as e:
                logger.error(f"Failed to initialize uinput: {e}")
                raise

    def close(self) -> None:
        if self._ui:
            self._ui.close()
            self._ui = None

    def send_key(self, key_code: int, press: bool = True, release: bool = True) -> None:
        if not self._ui:
            return
        
        try:
            if press:
                self._ui.write(evdev.ecodes.EV_KEY, key_code, 1)
                self._ui.syn()
            if release:
                self._ui.write(evdev.ecodes.EV_KEY, key_code, 0)
                self._ui.syn()
        except OSError:
            logger.error("Error writing to uinput device")

    def send_combo(self, modifiers: list[int], key: int) -> None:
        # Press modifiers
        for mod in modifiers:
            self.send_key(mod, press=True, release=False)
        
        # Tap key
        self.send_key(key)
        
        # Release modifiers
        for mod in reversed(modifiers):
            self.send_key(mod, press=False, release=True)

    def backspace(self, count: int = 1) -> None:
        for _ in range(count):
            self.send_key(evdev.ecodes.KEY_BACKSPACE)

    def switch_layout(self, switch_keys: list[int]) -> None:
        # Example switch_keys: [KEY_LEFTMETA, KEY_SPACE]
        # Iterate and press all
        for k in switch_keys:
            self.send_key(k, press=True, release=False)
        
        # Release all reversed
        for k in reversed(switch_keys):
            self.send_key(k, press=False, release=True)
            
    def type_sequence(self, key_codes: list[int]) -> None:
        for k in key_codes:
            self.send_key(k)
            
    def type_string(self, text: str) -> None:
        for char in text:
            seq = get_sequence_for_char(char)
            if seq:
                code, shift = seq
                if shift:
                    self.send_key(evdev.ecodes.KEY_LEFTSHIFT, press=True, release=False)
                
                self.send_key(code)
                
                if shift:
                    self.send_key(evdev.ecodes.KEY_LEFTSHIFT, press=False, release=True)
            else:
                logger.warning(f"Cannot type char '{char}': no keycode found")
