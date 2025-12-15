from __future__ import annotations

import asyncio
import logging
from typing import Callable, Coroutine

import evdev  # type: ignore

logger = logging.getLogger(__name__)


class InputHandler:
    def __init__(self, callback: Callable[[evdev.InputEvent], Coroutine[None, None, None] | None]) -> None:
        """
        Initialize the InputHandler.
        
        :param callback: Async or sync function to call for each key event.
        """
        self.callback = callback
        self.monitors: dict[str, asyncio.Task[None]] = {}
        self.should_run = False

    async def start(self) -> None:
        self.should_run = True
        logger.info("Starting InputHandler...")
        await self._scan_devices()
        # Start scanning in background
        asyncio.create_task(self._device_scanner_loop())

    async def stop(self) -> None:
        self.should_run = False
        # Create a list of items to iterate over, since we modify the dictionary
        tasks = list(self.monitors.items())
        for path, task in tasks:
            task.cancel()
            logger.debug(f"Stopped monitoring {path}")
        self.monitors.clear()
        logger.info("InputHandler stopped.")

    async def _device_scanner_loop(self) -> None:
        while self.should_run:
            await self._scan_devices()
            await asyncio.sleep(5)

    async def _scan_devices(self) -> None:
        try:
            paths = evdev.list_devices()
            current_paths = set(self.monitors.keys())
            found_paths = set(paths)

            # Remove disconnected
            for path in current_paths - found_paths:
                task = self.monitors.pop(path)
                task.cancel()
                logger.info(f"Device disconnected: {path}")

            # Add new
            for path in found_paths - current_paths:
                try:
                    device = evdev.InputDevice(path)
                    if self._is_keyboard(device):
                        if device.name == "punto-virtual-keyboard":
                             # Ignore our own device
                             device.close()
                             continue
                        logger.info(f"Found keyboard: {device.name} at {path}")
                        self._start_monitor(device)
                    else:
                        device.close()
                except OSError:
                    # Device might have disappeared or permissions issue
                    continue

        except Exception as e:
            logger.error(f"Error scanning devices: {e}")

    def _is_keyboard(self, device: evdev.InputDevice) -> bool:
        # Simple heuristic: must have keys
        caps = device.capabilities()
        if evdev.ecodes.EV_KEY not in caps:
            return False
        
        # Check for specific keys to differentiate from mice (optional, but good)
        # E.g. check for KEY_A, KEY_ESC
        supported_keys = caps[evdev.ecodes.EV_KEY]
        if evdev.ecodes.KEY_A in supported_keys and evdev.ecodes.KEY_ENTER in supported_keys:
            return True
            
        return False

    def _start_monitor(self, device: evdev.InputDevice) -> None:
        task = asyncio.create_task(self._monitor_device(device))
        self.monitors[device.path] = task

    async def _monitor_device(self, device: evdev.InputDevice) -> None:
        try:
            async for event in device.async_read_loop():
                if event.type == evdev.ecodes.EV_KEY:
                    try:
                        res = self.callback(event)
                        if asyncio.iscoroutine(res):
                            await res
                    except Exception as e:
                        logger.exception(f"Error in input callback: {e}")
        except OSError:
            logger.warning(f"Device disconnected during read: {device.path}")
        except asyncio.CancelledError:
            pass
        except Exception as e:
            logger.error(f"Unexpected error monitoring {device.path}: {e}")
        finally:
            if device.path in self.monitors:
                del self.monitors[device.path]
            try:
                device.close()
            except Exception as e:
                logger.error(f"Failed to close device {device.path}: {e}")
