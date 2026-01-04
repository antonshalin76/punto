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

  // Typo correction (v2.7+)
  bool typo_correction_enabled = true;
  int max_typo_diff = 1;
  bool sticky_shift_correction_enabled = true;

  // Sound
  bool sound_enabled = true;

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
  static bool show(GtkWidget *parent = nullptr);

  /// Загружает настройки из файла (используется также tray меню)
  static SettingsData load_settings();

  /// Сохраняет настройки в файл (используется также tray меню)
  static bool save_settings(const SettingsData &settings);

  /// Путь к user config (~/.config/punto/config.yaml)
  static std::string get_user_config_path();

private:
  /// Создаёт user config если его нет
  static bool ensure_user_config();
};

} // namespace punto
