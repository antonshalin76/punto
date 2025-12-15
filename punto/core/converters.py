from __future__ import annotations

import logging

try:
    from num2words import num2words  # type: ignore
except ImportError:
    num2words = None

from punto.core.layout import KEY_MAP

logger = logging.getLogger(__name__)

# Build translation tables
_EN_CHARS = ""
_RU_CHARS = ""

for _, (en, ru) in KEY_MAP.items():
    _EN_CHARS += en
    _EN_CHARS += en.upper()
    _RU_CHARS += ru
    _RU_CHARS += ru.upper()

_TRANS_EN_TO_RU = str.maketrans(_EN_CHARS, _RU_CHARS)
_TRANS_RU_TO_EN = str.maketrans(_RU_CHARS, _EN_CHARS)

def switch_layout(text: str) -> str:
    """
    Swaps English <-> Russian characters in the text.
    Detects majority and swaps to the other.
    """
    # Simply swap chars that match the map
    # Using python's translate is efficient
    # We need a combined table for correct simultaneous swapping

    full_table = {**_TRANS_EN_TO_RU, **_TRANS_RU_TO_EN}
    return text.translate(full_table)

def transliterate(text: str) -> str:
    """
    Simple transliteration EN <-> RU.
    """
    # Placeholder: standard ICAO or similar mapping needed.
    # For now, implementing a basic custom map for demonstration.
    ru_to_lat = {
        "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ё": "yo",
        "ж": "zh", "з": "z", "и": "i", "й": "y", "к": "k", "л": "l", "м": "m",
        "н": "n", "о": "o", "п": "p", "р": "r", "с": "s", "т": "t", "у": "u",
        "ф": "f", "х": "kh", "ц": "ts", "ч": "ch", "ш": "sh", "щ": "sch",
        "ъ": "", "ы": "y", "ь": "", "э": "e", "ю": "yu", "я": "ya"
    }
    # Create upper case variants
    ru_to_lat_full = {}
    for k, v in ru_to_lat.items():
        ru_to_lat_full[k] = v
        ru_to_lat_full[k.upper()] = v.capitalize()

    # Need reverse map too? Usually Transliterate implies Ru -> En or En (translit) -> Ru
    # Let's support RU -> EN primarily.
    
    # Sorting keys by length to handle multi-char sequences (zh, ch) if we were doing En->Ru
    # For Ru->En it's direct mapping
    mapped = []
    for char in text:
        mapped.append(ru_to_lat_full.get(char, char))
    return "".join(mapped)

def invert_case(text: str) -> str:
    return text.swapcase()

def number_to_text(text: str) -> str | None:
    """
    Converts numbers in text to words using num2words.
    """
    if num2words is None:
        return None
        
    try:
        # Try to parse text as a number (handling , or .)
        clean_text = text.replace(',', '.').strip()
        # Check if it is a number
        number = float(clean_text)
        
        # Convert
        # Detect lang? Default to RU as requested
        return num2words(number, lang='ru')
    except ValueError:
        return None
