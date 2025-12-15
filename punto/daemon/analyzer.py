from __future__ import annotations

import logging
from dataclasses import dataclass
from enum import Enum, auto

import evdev  # type: ignore

from punto.core.layout import KEY_MAP
from punto.core.detector import LanguageDetector

logger = logging.getLogger(__name__)


class ActionType(Enum):
    NONE = auto()
    SWITCH_LAYOUT = auto()
    CORRECT_WRONG_LAYOUT = auto()  # Manual correction
    LAYOUT_CHANGED = auto()
    TRANSLITERATE = auto()
    INVERT_CASE = auto()
    NUM_TO_WORDS = auto()
    REPLACE_TEXT = auto()


@dataclass
class AnalysisResult:
    action: ActionType
    target_layout_index: int = 0  # 0=EN, 1=RU (simplified)
    confidence: float = 0.0
    payload: list[int] | None = None
    text_payload: str | None = None


class InputAnalyzer:
    def __init__(self, 
                 layout_switch_hotkey: list[int] | None = None,
                 autocorrect: dict[str, str] | None = None,
                 autoreplace: dict[str, str] | None = None
                 ) -> None:
        self.buffer: list[int] = []  # Current word keycodes
        
        # Track modifiers
        self.active_modifiers: set[int] = set()
        
        self.detector = LanguageDetector()
        
        self.autocorrect = autocorrect or {}
        self.autoreplace = autoreplace or {}
        
        # Hotkey for manual switch detection
        # Format: [MOD1, MOD2, ..., TRIGGER_KEY]
        self.switch_hotkey = layout_switch_hotkey or []
        
        self.paused = False

    def set_paused(self, value: bool) -> None:
        if value and not self.paused:
            logger.info("Analyzer paused by exclusion rule.")
            self.reset()
        elif not value and self.paused:
            logger.info("Analyzer resumed.")
        self.paused = value

    def reset(self) -> None:
        self.buffer.clear()

    def process_key(self, key_code: int, value: int) -> AnalysisResult:
        """
        Ingest a key and return a recommended action.
        :param value: 0 for Up, 1 for Down, 2 for Repeat
        """
        if self.paused:
            return AnalysisResult(ActionType.NONE)
            
        # Handle modifiers (Shift, Ctrl, Alt, Super)
        # Using a simplified list of modifiers for now
        if key_code in (evdev.ecodes.KEY_LEFTSHIFT, evdev.ecodes.KEY_RIGHTSHIFT, 
                        evdev.ecodes.KEY_LEFTCTRL, evdev.ecodes.KEY_RIGHTCTRL,
                        evdev.ecodes.KEY_LEFTALT, evdev.ecodes.KEY_RIGHTALT,
                        evdev.ecodes.KEY_LEFTMETA, evdev.ecodes.KEY_RIGHTMETA):
            if value == 1:
                self.active_modifiers.add(key_code)
            elif value == 0:
                self.active_modifiers.discard(key_code)
            return AnalysisResult(ActionType.NONE)

        # Ignore key ups for normal processing, but maybe use them?
        if value == 0:
            return AnalysisResult(ActionType.NONE)

        # Handle Hotkeys (Pause)
        if self.switch_hotkey and key_code == self.switch_hotkey[-1]:
            # Check modifiers
            req_mods = self.switch_hotkey[:-1]
            if all(m in self.active_modifiers for m in req_mods):
                # Detected manual switch!
                self.reset()
                return AnalysisResult(ActionType.LAYOUT_CHANGED)

        # Handle Hotkeys (Pause)
        if key_code == evdev.ecodes.KEY_PAUSE:
            # Check for combos
            # Lctrl+Lshift+Pause -> Transliterate
            # Lctrl+Pause -> Invert Case All
            # Lshift+Pause -> Invert Case First/Individual
            
            # For brevity in PoC, just checking presence
            # Lctrl+Shift etc. will be used in future for other actions
            # lctrl = evdev.ecodes.KEY_LEFTCTRL in self.active_modifiers
            # lshift = evdev.ecodes.KEY_LEFTSHIFT in self.active_modifiers

            
            lctrl = evdev.ecodes.KEY_LEFTCTRL in self.active_modifiers
            lshift = evdev.ecodes.KEY_LEFTSHIFT in self.active_modifiers
            lalt = evdev.ecodes.KEY_LEFTALT in self.active_modifiers

            action = ActionType.CORRECT_WRONG_LAYOUT
            if lctrl and lshift:
                action = ActionType.TRANSLITERATE
            elif lctrl:
                action = ActionType.INVERT_CASE
            elif lalt:
                action = ActionType.NUM_TO_WORDS
            elif lshift:
                # Invert case for buffer/selection?
                # Using generic invert case action for now
                action = ActionType.INVERT_CASE

            if self.buffer:
                 # Logic for word not implemented yet for advanced features, 
                 # defaulting to handling as standard correction or selection
                 # For MVP: If modifiers are pressed, we usually target selection 
                 # OR current word if we want to get fancy with backspacing.
                 # Let's keep it simple: If word in buffer, assume we want to act on it via payload.
                 return AnalysisResult(action, payload=list(self.buffer))
            else:
                return AnalysisResult(action)

        # 1. Handle non-printable (control keys)
        if key_code == evdev.ecodes.KEY_BACKSPACE:
            if self.buffer:
                self.buffer.pop()
            return AnalysisResult(ActionType.NONE)

        if key_code in (evdev.ecodes.KEY_SPACE, evdev.ecodes.KEY_ENTER, evdev.ecodes.KEY_TAB, 
                        evdev.ecodes.KEY_COMMA, evdev.ecodes.KEY_DOT):
            # End of word delimiter. Check for replacements before resetting.
            res = self._check_replacements()
            self.reset()
            if res:
                return res
            return AnalysisResult(ActionType.NONE)

        if key_code not in KEY_MAP:
            # Punctuation or other keys might reset or be part of word
            # If it's something like TAB, reset
            self.reset()
            return AnalysisResult(ActionType.NONE)

        # 2. Append to buffer
        self.buffer.append(key_code)

        # 3. Analyze current buffer
        return self._analyze_buffer()

    def _analyze_buffer(self) -> AnalysisResult:
        # Heuristic:
        # Convert buffer to EN string and RU string
        # If EN string looks like "Gibberish" AND RU string looks like "Valid", suggest switch.
        
        if len(self.buffer) < 3:
            return AnalysisResult(ActionType.NONE)

        en_word = "".join(KEY_MAP[k][0] for k in self.buffer)
        ru_word = "".join(KEY_MAP[k][1] for k in self.buffer)

        # Detect language
        verdict = self.detector.analyze(en_word, ru_word)
        
        # If detector says RU but we typed in EN layout (en_word chars were used logic?)
        # Actually KEY_MAP maps KeyCode -> Char.
        # We don't know the current system layout easily without X11/Wayland query.
        # BUT we assume the user intends to type in the language that generates "Valid" text.
        
        # Heuristic: 
        # If verdict is RU, and en_word looks invalid -> We should have typed RU.
        # If currently we are effectively typing "ghbdtn" (which is en_word), strict logic says:
        # We don't know if system is EN or RU. 
        # But "ghbdtn" is invalid EN. "privet" is valid RU.
        # So providing the "RU" payload to the Service is what matters?
        # Service takes payload (keys) and switches layout.
        # IF we were already in RU, switching layout would make it EN (gibberish).
        # So we trust the toggle.
        
        # Wait, if I am in RU layout and type "руддщ" (hello mapped).
        # en_word = "hello" (Valid)
        # ru_word = "руддщ" (Invalid)
        # Verdict -> EN.
        # Service switches to EN. Correct.
        
        # Issue: What if both are valid? "net" (нет).
        # Verdict -> None. No switch.
        
        if verdict == "ru":
             # We think it should be Russian.
             # If it looks like we typed English gibberish to get here, trigger switch.
             # The Service relies on "Switch" toggling the system state.
             # So if we detect RU is better, we send switch.
             # BUT we must be careful not to loop.
             # Loop prevention is handled by: 
             # 1. clearing buffer
             # 2. auto-switch happens once per word usually.
             
             # Optimization: don't switch if we are arguably already satisfied?
             # No, we assume if we are processing, we might be wrong.
             # Actually, without knowing current layout, we assume "Improvement" is needed.
             # If "ghbdtn" -> verdict RU. We switch.
             # If "privet" -> verdict RU. We switch?? NO!
             # If we typed "privet", en_word="privet". ru_word="зейм.у".
             # Verdict EN (privet is valid EN, ru is gibberish).
             # So if we typed "privet", verdict is EN.
             
             return AnalysisResult(
                ActionType.SWITCH_LAYOUT, 
                target_layout_index=1, 
                confidence=0.8,
                payload=list(self.buffer)
            )

        elif verdict == "en":
             # We think it should be English.
             return AnalysisResult(
                ActionType.SWITCH_LAYOUT, 
                target_layout_index=0, 
                confidence=0.8,
                payload=list(self.buffer)
            )
            
        return AnalysisResult(ActionType.NONE)

    def get_current_word_len(self) -> int:
        return len(self.buffer)

    def _check_replacements(self) -> AnalysisResult | None:
        if not self.buffer:
            return None
            
        # Check both EN and RU variants of the typed word
        en_word = "".join(KEY_MAP[k][0] for k in self.buffer)
        ru_word = "".join(KEY_MAP[k][1] for k in self.buffer)
        
        if en_word in self.autoreplace:
            return AnalysisResult(ActionType.REPLACE_TEXT, payload=list(self.buffer), text_payload=self.autoreplace[en_word], confidence=1.0)
        
        if ru_word in self.autoreplace:
            return AnalysisResult(ActionType.REPLACE_TEXT, payload=list(self.buffer), text_payload=self.autoreplace[ru_word], confidence=1.0)
            
        # Check Autocorrect
        if en_word in self.autocorrect:
             return AnalysisResult(ActionType.REPLACE_TEXT, payload=list(self.buffer), text_payload=self.autocorrect[en_word], confidence=1.0)

        if ru_word in self.autocorrect:
             return AnalysisResult(ActionType.REPLACE_TEXT, payload=list(self.buffer), text_payload=self.autocorrect[ru_word], confidence=1.0)
             
        return None
