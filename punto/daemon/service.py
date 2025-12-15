from __future__ import annotations

import asyncio
import logging
import signal

import evdev  # type: ignore

from punto.core.config import ConfigManager
from punto.core.clipboard import ClipboardManager
from punto.core.converters import switch_layout, transliterate, invert_case, number_to_text
from punto.core.sound import SoundEngine
from punto.core.window import WindowDetector
from punto.daemon.analyzer import ActionType, InputAnalyzer
from punto.daemon.injector import Injector
from punto.daemon.input_handler import InputHandler

logger = logging.getLogger(__name__)


class PuntoService:
    def __init__(self) -> None:
        self.config_manager = ConfigManager()
        self.config = self.config_manager.load()
        
        self.injector = Injector()
        # Analyzer init requires dicts from config now
        self.analyzer = InputAnalyzer(
            layout_switch_hotkey=self.config.layout_switch_hotkey,
            autocorrect=self.config.autocorrect,
            autoreplace=self.config.autoreplace
        )
        self.handler = InputHandler(self.on_input_event)
        self.clipboard = ClipboardManager()
        self.sound = SoundEngine()
        self.sound.enabled = self.config.sound_enabled
        self.window_detector = WindowDetector()
        
        self.current_layout_index = 0 # Assume 0 (EN) start. TODO: Sync with OS.
        
        self._running_event = asyncio.Event()

    async def run(self) -> None:
        logger.info("Starting Punto Service...")
        
        # Setup signal handlers
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, self.stop)
            except NotImplementedError:
                pass
                
        try:
            loop.add_signal_handler(signal.SIGHUP, self.reload_config)
        except (NotImplementedError, AttributeError):
            pass # SIGHUP might not be available on all platforms
            
        self._running_event.set()
        
        try:
            await self.handler.start()
            
            # Keep running until stopped
            while self._running_event.is_set():
                await self._check_active_window()
                await asyncio.sleep(1)
                
        except asyncio.CancelledError:
            pass
        finally:
            await self.handler.stop()
            self.injector.close()
            logger.info("Service shutdown complete.")

    def stop(self) -> None:
        logger.info("Stopping service...")
        self._running_event.clear()

    def reload_config(self) -> None:
        logger.info("Reloading configuration...")
        try:
            self.config = self.config_manager.load()
            
            # Re-init dependent components
            self.sound.enabled = self.config.sound_enabled
            
            # Update Analyzer with new dicts
            # Since Analyzer has state, re-creating it clears buffer. This is acceptable on reload.
            self.analyzer = InputAnalyzer(
                layout_switch_hotkey=self.config.layout_switch_hotkey,
                autocorrect=self.config.autocorrect,
                autoreplace=self.config.autoreplace
            )
            
            logger.info("Configuration reloaded successfully.")
        except Exception as e:
            logger.error(f"Failed to reload config: {e}")

    async def on_input_event(self, event: evdev.InputEvent) -> None:
        if event.type != evdev.ecodes.EV_KEY:
            return

        res = self.analyzer.process_key(event.code, event.value)
        
        if res.action == ActionType.LAYOUT_CHANGED:
            self.current_layout_index = 1 - self.current_layout_index
            logger.info(f"Layout changed manually. New state: {self.current_layout_index}")
            return

        if res.action == ActionType.SWITCH_LAYOUT and res.payload:
            # Only switch if we are NOT in the target layout
            if res.target_layout_index != self.current_layout_index:
                logger.info(f"Auto-switching layout to {res.target_layout_index}. Confidence: {res.confidence}")
                self._perform_switch(res.payload)
            else:
                logger.debug(f"Skipping switch, already in layout {self.current_layout_index}")
        
        if res.action == ActionType.REPLACE_TEXT and res.payload and res.text_payload:
            logger.info(f"Auto-replacing text -> {res.text_payload}")
            # Backspace buffer len + 1 (for the delimiter that triggered it)
            self.injector.backspace(len(res.payload) + 1)
            self.injector.type_string(res.text_payload)
            # Add implicit space if triggered by space? 
            # Usually users type "dd " -> "Dobry den ". 
            # Delimiter was NOT in buffer probably (or was it?).
            # Analyzer resets on delimiter. Buffer contains word BEFORE delimiter.
            # So we backspace word. The delimiter (Space) was just pressed.
            # Ideally we want to keep the delimiter? Or replace it too?
            # If triggered by space, space is printed. Buffer is just word.
            # We BS word, print Replacement. Space stays?
            # No, BS erases word. Space is AFTER word visually.
            # So "word " -> BS(4) -> " " -> "Repl " -> "Repl ". Correct.
            return

        elif res.action in (ActionType.CORRECT_WRONG_LAYOUT, ActionType.TRANSLITERATE, ActionType.INVERT_CASE, ActionType.NUM_TO_WORDS):
            logger.info(f"Manual action triggered: {res.action}")
            if res.payload:
                # Todo: apply specific action to word? For now simple correction/switch logic is mostly bound to switch
                # If transliterate buffer -> Need special Injector logic (type string).
                # Fallback to selection correction for advanced actions on words (user should select text)
                # Or implement word modification later.
                if res.action == ActionType.CORRECT_WRONG_LAYOUT:
                    self._perform_switch(res.payload)
                else:
                    logger.warning("Advanced word modification not implemented, try selecting text.")
            else:
                asyncio.create_task(self._perform_selection_correction(res.action))

    async def _perform_selection_correction(self, action: ActionType) -> None:
        logger.info(f"Correction of selection requested: {action}")
        
        # 1. Copy selected text (Ctrl+C)
        self.injector.send_combo([evdev.ecodes.KEY_LEFTCTRL], evdev.ecodes.KEY_C)
        
        # Wait a bit for clipboard to update
        await asyncio.sleep(0.3)
        
        # 2. Get text
        text = self.clipboard.get_text()
        if not text:
            logger.warning("Clipboard empty or access failed.")
            return

        # 3. Process
        if action == ActionType.TRANSLITERATE:
            new_text = transliterate(text)
        elif action == ActionType.INVERT_CASE:
            new_text = invert_case(text)
        elif action == ActionType.NUM_TO_WORDS:
            val = number_to_text(text)
            new_text = val if val else text
        else: # CORRECT_WRONG_LAYOUT
            new_text = switch_layout(text)
        
        if new_text == text:
            logger.info("Text processing resulted in no change.")
            return

        # 4. Set text
        self.clipboard.set_text(new_text)
        
        # Wait a bit for clipboard to accept
        await asyncio.sleep(0.1)
        
        # 5. Paste (Ctrl+V)
        self.injector.send_combo([evdev.ecodes.KEY_LEFTCTRL], evdev.ecodes.KEY_V)



    async def _check_active_window(self) -> None:
        # If disabled globally, skip
        if not self.config.auto_switch_enabled:
             # Just ensure analyzer is not paused because of window? 
             # Or if global switch OFF, analyzer should be OFF?
             # Analyzer handles "process_key" logic. If we pause it, manual corrections also stop?
             # Requirements say: "Назначение или отмена опций автопереключения... (по дефолту включен всегда)".
             # If user turns OFF auto-switch via checkbox, manual corrections should still work!
             # So we shouldn't pause analyzer completely?
             # Pause affects "Auto" logic. Manual logic should persist?
             # Analyzer implementation of `paused` returns NONE immediately.
             # So we need separate flags for "AutoSwitching Paused" and "Everything Paused".
             pass
             
        # Check current window against exceptions
        info = self.window_detector.get_active_window_info()
        if info:
            title = info.get("title", "")
            # Check config exceptions
            # Config exceptions: {"processes": [], "window_titles": []}
            
            is_blacklisted = False
            for blocked_title in self.config.exceptions.get("window_titles", []):
                if blocked_title and blocked_title in title:
                    is_blacklisted = True
                    break
            
            # If blacklisted -> Pause Analyzer completely? Or just Auto?
            # Usually users want "Disable Punto" in games completely.
            self.analyzer.set_paused(is_blacklisted)

    def _perform_switch(self, key_codes: list[int]) -> None:
        # 1. Erase current word
        # Note: key_codes includes the last key that triggered it.
        # But the last key was just pressed physically (and passed through).
        # So we need to backspace len(key_codes).
        self.injector.backspace(len(key_codes))
        
        # 2. Switch Layout
        self.injector.switch_layout(self.config.layout_switch_hotkey)
        self.current_layout_index = 1 - self.current_layout_index
        self.sound.play("switch")
        
        # 3. Re-type
        self.injector.type_sequence(key_codes)
        
        # 4. Reset analyzer buffer, because we just handled the word.
        self.analyzer.reset()
