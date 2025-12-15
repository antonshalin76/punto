import logging
import shutil
import subprocess
from typing import Optional

logger = logging.getLogger(__name__)

class ClipboardManager:
    def __init__(self) -> None:
        self.backend = self._detect_backend()
        logger.info(f"ClipboardManager initialized with backend: {self.backend}")

    def _detect_backend(self) -> str:
        # Check for Wayland (wl-clipboard)
        if shutil.which("wl-copy") and shutil.which("wl-paste"):
            return "wayland"
        # Check for X11 (xclip)
        if shutil.which("xclip"):
            return "x11"
        return "none"

    def get_text(self) -> Optional[str]:
        try:
            if self.backend == "wayland":
                # wl-paste --no-newline to get exact content
                res = subprocess.run(
                    ["wl-paste", "--no-newline"], 
                    capture_output=True, text=True, timeout=1.0
                )
                if res.returncode == 0:
                    return res.stdout
            elif self.backend == "x11":
                res = subprocess.run(
                    ["xclip", "-selection", "primary", "-o"], 
                    capture_output=True, text=True, timeout=1.0
                )
                if res.returncode == 0:
                    return res.stdout
        except Exception as e:
            logger.error(f"Failed to get clipboard text: {e}")
        return None

    def set_text(self, text: str) -> None:
        try:
            if self.backend == "wayland":
                subprocess.run(
                    ["wl-copy"], 
                    input=text, text=True, check=True
                )
            elif self.backend == "x11":
                subprocess.run(
                    ["xclip", "-selection", "clipboard", "-i"], 
                    input=text, text=True, check=True
                )
                # Also set primary for consistency in X11
                subprocess.run(
                    ["xclip", "-selection", "primary", "-i"], 
                    input=text, text=True, check=True
                )
        except Exception as e:
            logger.error(f"Failed to set clipboard text: {e}")
