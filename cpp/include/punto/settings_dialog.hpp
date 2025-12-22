/**
 * @file settings_dialog.hpp
 * @brief Диалог настроек punto
 */

#pragma once

#include <gtk/gtk.h>
#include <string>

namespace punto {

/**
 * @brief Структура для хранения настроек в UI
 */
struct SettingsData {
  // Auto-switch
  bool auto_enabled = true;
  double threshold = 3.5;
  int min_word_len = 2;
  double min_score = 5.0;
  int max_rollback_words = 5;

  // Sound
  bool sound_enabled = true;

  // Delays
  int key_press = 12;
  int layout_switch = 150;
  int retype = 15;
  int turbo_key_press = 12;
  int turbo_retype = 20;

  // Hotkey
  std::string modifier = "leftctrl";
  std::string key = "grave";
};

/**
 * @brief Класс диалога настроек
 */
class SettingsDialog {
public:
  /**
   * @brief Показывает диалог настроек
   * @param parent Родительское окно (может быть nullptr)
   * @return true если настройки были сохранены
   */
  static bool show(GtkWidget* parent = nullptr);

  /// Загружает настройки из файла (используется также tray меню)
  static SettingsData load_settings();

  /// Сохраняет настройки в файл (используется также tray меню)
  static bool save_settings(const SettingsData& settings);

  /// Путь к user config (~/.config/punto/config.yaml)
  static std::string get_user_config_path();

private:

  /// Создаёт user config если его нет
  static bool ensure_user_config();
};

} // namespace punto
