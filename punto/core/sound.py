import logging
import subprocess
import shutil
from pathlib import Path
from threading import Thread

logger = logging.getLogger(__name__)

class SoundEngine:
    def __init__(self, assets_dir: Path | None = None) -> None:
        self.enabled = True
        self.assets_dir = assets_dir or Path(__file__).parent.parent.parent / "assets" / "sounds"
        
        # Check players
        self.player = self._find_player()
        logger.info(f"Sound engine initialized. Player: {self.player}")

    def _find_player(self) -> str | None:
        if shutil.which("paplay"):
            return "paplay"
        if shutil.which("aplay"):
            return "aplay"
        return None

    def play(self, event_name: str) -> None:
        if not self.enabled or not self.player:
            return
            
        # Run in thread to allow non-blocking (CLI players block)
        Thread(target=self._play_sync, args=(event_name,), daemon=True).start()

    def _play_sync(self, event_name: str) -> None:
        # File mapping
        # We assume files exist: click.wav, switch.wav, error.wav
        filename = f"{event_name}.wav"
        path = self.assets_dir / filename
        
        if not path.exists():
            # logger.warning(f"Sound file not found: {path}")
            return
            
        try:
            subprocess.run(
                [self.player, str(path)], 
                stdout=subprocess.DEVNULL, 
                stderr=subprocess.DEVNULL
            )
        except Exception:
            pass
