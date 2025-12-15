
from punto.core.layout import KEY_MAP

# Reverse lookup
# We need to distinguish between lower and upper
# KEY_MAP values are (en_lower, ru_lower)
# We assume standard shift behavior

CHAR_TO_KEYCODE: dict[str, int] = {}
for code, (en, ru) in KEY_MAP.items():
    CHAR_TO_KEYCODE[en] = code
    CHAR_TO_KEYCODE[ru] = code

def get_sequence_for_char(char: str) -> tuple[int, bool] | None:
    """
    Returns (keycode, needs_shift)
    """
    lower = char.lower()
    code = CHAR_TO_KEYCODE.get(lower)
    
    if code is None:
        return None
        
    needs_shift = char.isupper()
    # Special symbols logic (incomplete for PoC)
    # e.g. ',' is not upper '.' but distinct key.
    # Our KAR_MAP has ',' and '.' directly.
    
    return code, needs_shift
