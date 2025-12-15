
import logging
import subprocess
import shutil

logger = logging.getLogger(__name__)

class WindowDetector:
    def __init__(self) -> None:
        self.cmd = None
        if shutil.which("xdotool"):
            self.cmd = "xdotool"
        
    def get_active_window_info(self) -> dict[str, str] | None:
        """
        Returns {'title': ..., 'class': ...}
        Works primarily on X11. Wayland support is limited/non-existent without extensions.
        """
        if not self.cmd:
            return None
            
        try:
            # 1. Get ID
            # xdotool getactivewindow
            res = subprocess.run(["xdotool", "getactivewindow"], capture_output=True, text=True, timeout=0.5)
            if res.returncode != 0:
                return None
            wid = res.stdout.strip()
            
            # 2. Get Name
            res_name = subprocess.run(["xdotool", "getwindowname", wid], capture_output=True, text=True, timeout=0.5)
            title = res_name.stdout.strip()
            
            # 3. Get Class
            # xdotool getwindowclassname is messy, let's assume we match mostly by Title for now or rely on user config matching
            # xprop -id <id> WM_CLASS
            
            return {"title": title, "class": ""}
        except Exception:
            return None
