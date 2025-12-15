from __future__ import annotations

import evdev  # type: ignore

# Base mapping for QWERTY (US) and ЙЦУКЕН (RU) standard layouts
# Key: evdev scancode
# Value: (EN_char, RU_char)

# Common row 1
_ROW1 = {
    evdev.ecodes.KEY_Q: ("q", "й"),
    evdev.ecodes.KEY_W: ("w", "ц"),
    evdev.ecodes.KEY_E: ("e", "у"),
    evdev.ecodes.KEY_R: ("r", "к"),
    evdev.ecodes.KEY_T: ("t", "е"),
    evdev.ecodes.KEY_Y: ("y", "н"),
    evdev.ecodes.KEY_U: ("u", "г"),
    evdev.ecodes.KEY_I: ("i", "ш"),
    evdev.ecodes.KEY_O: ("o", "щ"),
    evdev.ecodes.KEY_P: ("p", "з"),
    evdev.ecodes.KEY_LEFTBRACE: ("[", "х"),
    evdev.ecodes.KEY_RIGHTBRACE: ("]", "ъ"),
}

# Common row 2
_ROW2 = {
    evdev.ecodes.KEY_A: ("a", "ф"),
    evdev.ecodes.KEY_S: ("s", "ы"),
    evdev.ecodes.KEY_D: ("d", "в"),
    evdev.ecodes.KEY_F: ("f", "а"),
    evdev.ecodes.KEY_G: ("g", "п"),
    evdev.ecodes.KEY_H: ("h", "р"),
    evdev.ecodes.KEY_J: ("j", "о"),
    evdev.ecodes.KEY_K: ("k", "л"),
    evdev.ecodes.KEY_L: ("l", "д"),
    evdev.ecodes.KEY_SEMICOLON: (";", "ж"),
    evdev.ecodes.KEY_APOSTROPHE: ("'", "э"),
}

# Common row 3
_ROW3 = {
    evdev.ecodes.KEY_Z: ("z", "я"),
    evdev.ecodes.KEY_X: ("x", "ч"),
    evdev.ecodes.KEY_C: ("c", "с"),
    evdev.ecodes.KEY_V: ("v", "м"),
    evdev.ecodes.KEY_B: ("b", "и"),
    evdev.ecodes.KEY_N: ("n", "т"),
    evdev.ecodes.KEY_M: ("m", "ь"),
    evdev.ecodes.KEY_COMMA: (",", "б"),
    evdev.ecodes.KEY_DOT: (".", "ю"),
    evdev.ecodes.KEY_SLASH: ("/", "."),
}

# Combined
KEY_MAP = {**_ROW1, **_ROW2, **_ROW3}

# Inverse lookup for tools
CHAR_TO_KEY = {}
for code, (en, ru) in KEY_MAP.items():
    CHAR_TO_KEY[en] = code
    CHAR_TO_KEY[ru] = code

def get_chars(code: int) -> tuple[str, str] | None:
    return KEY_MAP.get(code)

def is_printable(code: int) -> bool:
    return code in KEY_MAP
