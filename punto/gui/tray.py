import signal
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('AppIndicator3', '0.1')

from gi.repository import Gtk, AppIndicator3
from punto.core.config import ConfigManager
from punto.gui.settings import SettingsWindow

APPINDICATOR_ID = 'punto-ubuntu-indicator'

class TrayApp:
    def __init__(self):
        self.indicator = AppIndicator3.Indicator.new(
            APPINDICATOR_ID,
            'input-keyboard', # system icon or path
            AppIndicator3.IndicatorCategory.APPLICATION_STATUS
        )
        self.indicator.set_status(AppIndicator3.IndicatorStatus.ACTIVE)
        self.indicator.set_menu(self.build_menu())
        
        self.config_manager = ConfigManager()
        self.settings_window = None

    def build_menu(self):
        menu = Gtk.Menu()
        
        item_settings = Gtk.MenuItem(label='Настройки')
        item_settings.connect('activate', self.open_settings)
        menu.append(item_settings)
        
        item_quit = Gtk.MenuItem(label='Выход')
        item_quit.connect('activate', self.quit)
        menu.append(item_quit)
        
        menu.show_all()
        return menu

    def open_settings(self, source):
        if not self.settings_window:
            self.settings_window = SettingsWindow(self.config_manager)
            self.settings_window.connect("destroy", self.on_settings_closed)
            self.settings_window.show_all()
        else:
            self.settings_window.present()

    def on_settings_closed(self, widget):
        self.settings_window = None

    def quit(self, source):
        Gtk.main_quit()

def main():
    # Handle Ctrl+C
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    
    _app = TrayApp()
    Gtk.main()

if __name__ == "__main__":
    main()
