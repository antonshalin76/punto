import subprocess
import gi
from dataclasses import replace

gi.require_version('Gtk', '3.0')
from gi.repository import Gtk

class SettingsWindow(Gtk.Window):
    def __init__(self, config_manager):
        super().__init__(title="Настройки Punto Ubuntu")
        self.set_border_width(10)
        self.set_default_size(400, 300)
        
        self.config_manager = config_manager
        self.config = self.config_manager.load()
        
        vbox = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        self.add(vbox)
        
        # Checking buttons for boolean options
        self.check_sound = Gtk.CheckButton(label="Включить звуки")
        self.check_sound.set_active(self.config.sound_enabled)
        self.check_sound.connect("toggled", self.on_change)
        vbox.pack_start(self.check_sound, False, False, 0)
        
        self.check_autoswitch = Gtk.CheckButton(label="Автопереключение")
        self.check_autoswitch.set_active(self.config.auto_switch_enabled)
        self.check_autoswitch.connect("toggled", self.on_change)
        vbox.pack_start(self.check_autoswitch, False, False, 0)

        # TODO: Editors for Lists/Dicts (Hotkeys, Exceptions)
        
        save_btn = Gtk.Button(label="Сохранить")
        save_btn.connect("clicked", self.on_save)
        vbox.pack_end(save_btn, False, False, 0)
        
    def on_change(self, widget):
        pass # Just tracking state in widgets

    def on_save(self, widget):
        # Update config object from widgets
        # Config is frozen dataclass, so we create new dict then new config?
        # Ideally we use replace()
        
        new_config = replace(
            self.config,
            sound_enabled=self.check_sound.get_active(),
            auto_switch_enabled=self.check_autoswitch.get_active()
        )
        
        try:
            self.config_manager.save(new_config)
            self.config = new_config
            # TODO: Signal daemon to reload (SIGHUP?)
            # pkill -HUP -f punto.daemon.main ?
            subprocess.run(["pkill", "-HUP", "-f", "punto.daemon.main"])
            
            dialog = Gtk.MessageDialog(
                self, 0, Gtk.MessageType.INFO, Gtk.ButtonsType.OK, "Настройки сохранены"
            )
            dialog.run()
            dialog.destroy()
        except Exception as e:
            dialog = Gtk.MessageDialog(
                self, 0, Gtk.MessageType.ERROR, Gtk.ButtonsType.OK, f"Ошибка: {e}"
            )
            dialog.run()
            dialog.destroy()
