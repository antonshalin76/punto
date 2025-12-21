/**
 * @file settings_dialog.cpp
 * @brief Реализация диалога настроек
 */

#include "punto/settings_dialog.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <locale>

#include <glib.h>

#include "punto/types.hpp"

namespace punto {

namespace {

constexpr const char* kSystemConfigPath = "/etc/punto/config.yaml";

/// Вспомогательная функция trim
std::string trim(const std::string& str) {
  size_t start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = str.find_last_not_of(" \t\r\n");
  return str.substr(start, end - start + 1);
}

/// Парсит значение после двоеточия
std::string parse_value(const std::string& line) {
  auto pos = line.find(':');
  if (pos == std::string::npos) return "";
  return trim(line.substr(pos + 1));
}

/// Парсит double в независимости от локали (использует C locale)
bool parse_double_clocale(const std::string& text, double& out) {
  std::istringstream ss(text);
  ss.imbue(std::locale::classic());
  ss >> out;
  return !ss.fail();
}

} // namespace

std::string SettingsDialog::get_user_config_path() {
  const char* home = std::getenv("HOME");
  if (!home || std::string(home).empty()) {
    // На всякий случай используем glib, если переменная HOME не прокинута
    const char* ghome = g_get_home_dir();
    if (ghome && std::string(ghome).size() > 0) {
      home = ghome;
    }
  }

  if (home) {
    return std::string(home) + "/" + std::string(kUserConfigRelPath);
  }
  return "";
}

bool SettingsDialog::ensure_user_config() {
  std::string user_path = get_user_config_path();
  if (user_path.empty()) return false;

  std::ifstream check{user_path};
  if (check.is_open()) return true;

  // Создаём директорию
  std::string dir = user_path.substr(0, user_path.rfind('/'));
  std::string mkdir_cmd = "mkdir -p \"" + dir + "\"";
  if (std::system(mkdir_cmd.c_str()) != 0) return false;

  // Копируем системный конфиг
  std::string cp_cmd = "cp \"" + std::string(kSystemConfigPath) + "\" \"" + user_path + "\"";
  return std::system(cp_cmd.c_str()) == 0;
}

SettingsData SettingsDialog::load_settings() {
  SettingsData settings;

  ensure_user_config();
  std::string config_path = get_user_config_path();
  if (config_path.empty()) {
    return settings;
  }

  std::ifstream file{config_path};
  if (!file.is_open()) {
    return settings;
  }

  std::string line;
  std::string section;

  while (std::getline(file, line)) {
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;

    // Секции
    if (trimmed == "hotkey:") { section = "hotkey"; continue; }
    if (trimmed == "delays:") { section = "delays"; continue; }
    if (trimmed == "auto_switch:") { section = "auto_switch"; continue; }

    std::string value = parse_value(trimmed);
    if (value.empty()) continue;

    if (section == "hotkey") {
      if (trimmed.find("modifier:") != std::string::npos) {
        settings.modifier = value;
      } else if (trimmed.find("key:") != std::string::npos) {
        settings.key = value;
      }
    } else if (section == "delays") {
      if (trimmed.find("key_press:") != std::string::npos) {
        settings.key_press = std::stoi(value);
      } else if (trimmed.find("layout_switch:") != std::string::npos) {
        settings.layout_switch = std::stoi(value);
      } else if (trimmed.find("retype:") != std::string::npos && trimmed.find("turbo") == std::string::npos) {
        settings.retype = std::stoi(value);
      } else if (trimmed.find("turbo_key_press:") != std::string::npos) {
        settings.turbo_key_press = std::stoi(value);
      } else if (trimmed.find("turbo_retype:") != std::string::npos) {
        settings.turbo_retype = std::stoi(value);
      }
    } else if (section == "auto_switch") {
      if (trimmed.find("enabled:") != std::string::npos) {
        settings.auto_enabled = (value == "true" || value == "1");
      } else if (trimmed.find("threshold:") != std::string::npos) {
        double v{};
        if (parse_double_clocale(value, v)) {
          settings.threshold = v;
        }
      } else if (trimmed.find("min_word_len:") != std::string::npos) {
        settings.min_word_len = std::stoi(value);
      } else if (trimmed.find("min_score:") != std::string::npos) {
        double v{};
        if (parse_double_clocale(value, v)) {
          settings.min_score = v;
        }
      }
    }
  }

  return settings;
}

bool SettingsDialog::save_settings(const SettingsData& settings) {
  ensure_user_config();
  std::string config_path = get_user_config_path();
  if (config_path.empty()) {
    return false;
  }

  std::ofstream file{config_path, std::ios::trunc};
  if (!file.is_open()) {
    return false;
  }

  file << "# Punto Switcher Configuration\n";
  file << "# Автоматически сгенерировано punto-tray\n\n";

  file << "hotkey:\n";
  file << "  modifier: " << settings.modifier << "\n";
  file << "  key: " << settings.key << "\n\n";

  file << "delays:\n";
  file << "  key_press: " << settings.key_press << "\n";
  file << "  layout_switch: " << settings.layout_switch << "\n";
  file << "  retype: " << settings.retype << "\n";
  file << "  turbo_key_press: " << settings.turbo_key_press << "\n";
  file << "  turbo_retype: " << settings.turbo_retype << "\n\n";

  file << "auto_switch:\n";
  file << "  enabled: " << (settings.auto_enabled ? "true" : "false") << "\n";
  file << "  threshold: " << settings.threshold << "\n";
  file << "  min_word_len: " << settings.min_word_len << "\n";
  file << "  min_score: " << settings.min_score << "\n";

  file.flush();
  if (!file.good()) {
    return false;
  }

  return true;
}

bool SettingsDialog::show(GtkWidget* parent) {
  // Загружаем текущие настройки
  SettingsData settings = load_settings();

  // Создаём диалог
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Настройки Punto Switcher",
      parent ? GTK_WINDOW(parent) : nullptr,
      GTK_DIALOG_MODAL,
      "_Отмена", GTK_RESPONSE_CANCEL,
      "_Сохранить", GTK_RESPONSE_ACCEPT,
      nullptr);

  gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  // Notebook для вкладок
  GtkWidget* notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);

  // ===== Вкладка "Автопереключение" =====
  GtkWidget* auto_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(auto_box), 12);

  // Enabled checkbox
  GtkWidget* auto_enabled = gtk_check_button_new_with_label("Включить автопереключение");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_enabled), settings.auto_enabled);
  gtk_box_pack_start(GTK_BOX(auto_box), auto_enabled, FALSE, FALSE, 0);

  // Grid для параметров
  GtkWidget* auto_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(auto_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(auto_grid), 12);
  gtk_box_pack_start(GTK_BOX(auto_box), auto_grid, FALSE, FALSE, 8);

  // Threshold
  gtk_grid_attach(GTK_GRID(auto_grid), gtk_label_new("Порог срабатывания:"), 0, 0, 1, 1);
  GtkWidget* threshold_spin = gtk_spin_button_new_with_range(0.5, 10.0, 0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(threshold_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(threshold_spin), settings.threshold);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(threshold_spin), 0.1, 0.1);
  gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(threshold_spin), FALSE);
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_spin, 1, 0, 1, 1);

  // Min word len
  gtk_grid_attach(GTK_GRID(auto_grid), gtk_label_new("Мин. длина слова:"), 0, 1, 1, 1);
  GtkWidget* min_word_spin = gtk_spin_button_new_with_range(1, 10, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_word_spin), settings.min_word_len);
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_spin, 1, 1, 1, 1);

  // Min score
  gtk_grid_attach(GTK_GRID(auto_grid), gtk_label_new("Мин. уверенность:"), 0, 2, 1, 1);
  GtkWidget* min_score_spin = gtk_spin_button_new_with_range(0.0, 20.0, 0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(min_score_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_score_spin), settings.min_score);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(min_score_spin), 0.1, 0.1);
  gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(min_score_spin), FALSE);
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_spin, 1, 2, 1, 1);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), auto_box, gtk_label_new("Автопереключение"));

  // ===== Вкладка "Задержки" =====
  GtkWidget* delays_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(delays_box), 12);

  GtkWidget* delays_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(delays_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(delays_grid), 12);
  gtk_box_pack_start(GTK_BOX(delays_box), delays_grid, FALSE, FALSE, 0);

  // Key press
  gtk_grid_attach(GTK_GRID(delays_grid), gtk_label_new("Нажатие клавиши (мс):"), 0, 0, 1, 1);
  GtkWidget* key_press_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(key_press_spin), settings.key_press);
  gtk_grid_attach(GTK_GRID(delays_grid), key_press_spin, 1, 0, 1, 1);

  // Layout switch
  gtk_grid_attach(GTK_GRID(delays_grid), gtk_label_new("Переключение раскладки (мс):"), 0, 1, 1, 1);
  GtkWidget* layout_spin = gtk_spin_button_new_with_range(10, 500, 10);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(layout_spin), settings.layout_switch);
  gtk_grid_attach(GTK_GRID(delays_grid), layout_spin, 1, 1, 1, 1);

  // Retype
  gtk_grid_attach(GTK_GRID(delays_grid), gtk_label_new("Перепечатывание (мс):"), 0, 2, 1, 1);
  GtkWidget* retype_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(retype_spin), settings.retype);
  gtk_grid_attach(GTK_GRID(delays_grid), retype_spin, 1, 2, 1, 1);

  // Separator
  gtk_box_pack_start(GTK_BOX(delays_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  gtk_box_pack_start(GTK_BOX(delays_box), gtk_label_new("Турбо-режим (для автокоррекции):"), FALSE, FALSE, 0);

  GtkWidget* turbo_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(turbo_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(turbo_grid), 12);
  gtk_box_pack_start(GTK_BOX(delays_box), turbo_grid, FALSE, FALSE, 4);

  // Turbo key press
  gtk_grid_attach(GTK_GRID(turbo_grid), gtk_label_new("Турбо нажатие (мс):"), 0, 0, 1, 1);
  GtkWidget* turbo_key_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(turbo_key_spin), settings.turbo_key_press);
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_key_spin, 1, 0, 1, 1);

  // Turbo retype
  gtk_grid_attach(GTK_GRID(turbo_grid), gtk_label_new("Турбо перепечатка (мс):"), 0, 1, 1, 1);
  GtkWidget* turbo_retype_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(turbo_retype_spin), settings.turbo_retype);
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_retype_spin, 1, 1, 1, 1);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), delays_box, gtk_label_new("Задержки"));

  // ===== Вкладка "Горячие клавиши" =====
  GtkWidget* hotkey_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(hotkey_box), 12);

  GtkWidget* hotkey_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(hotkey_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(hotkey_grid), 12);
  gtk_box_pack_start(GTK_BOX(hotkey_box), hotkey_grid, FALSE, FALSE, 0);

  // Modifier combo
  gtk_grid_attach(GTK_GRID(hotkey_grid), gtk_label_new("Модификатор:"), 0, 0, 1, 1);
  GtkWidget* modifier_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftctrl", "Left Ctrl");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightctrl", "Right Ctrl");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftalt", "Left Alt");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightalt", "Right Alt");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftmeta", "Left Super");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightmeta", "Right Super");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(modifier_combo), settings.modifier.c_str());
  gtk_grid_attach(GTK_GRID(hotkey_grid), modifier_combo, 1, 0, 1, 1);

  // Key combo
  gtk_grid_attach(GTK_GRID(hotkey_grid), gtk_label_new("Клавиша:"), 0, 1, 1, 1);
  GtkWidget* key_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "grave", "` (Grave)");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "space", "Space");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "tab", "Tab");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "capslock", "Caps Lock");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(key_combo), settings.key.c_str());
  gtk_grid_attach(GTK_GRID(hotkey_grid), key_combo, 1, 1, 1, 1);

  // Примечание
  GtkWidget* note_label = gtk_label_new(
      "Примечание: это хоткей переключения раскладки,\n"
      "который punto эмулирует. Должен совпадать\n"
      "с системными настройками.");
  gtk_label_set_xalign(GTK_LABEL(note_label), 0);
  gtk_widget_set_margin_top(note_label, 12);
  PangoAttrList* attrs = pango_attr_list_new();
  pango_attr_list_insert(attrs, pango_attr_style_new(PANGO_STYLE_ITALIC));
  pango_attr_list_insert(attrs, pango_attr_scale_new(0.9));
  gtk_label_set_attributes(GTK_LABEL(note_label), attrs);
  pango_attr_list_unref(attrs);
  gtk_box_pack_start(GTK_BOX(hotkey_box), note_label, FALSE, FALSE, 0);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), hotkey_box, gtk_label_new("Горячие клавиши"));

  // Показываем диалог
  gtk_widget_show_all(dialog);

  gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  bool saved = false;
  if (response == GTK_RESPONSE_ACCEPT) {
    // Читаем значения из виджетов
    settings.auto_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_enabled));
    settings.threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(threshold_spin));
    settings.min_word_len = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(min_word_spin));
    settings.min_score = gtk_spin_button_get_value(GTK_SPIN_BUTTON(min_score_spin));

    settings.key_press = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(key_press_spin));
    settings.layout_switch = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(layout_spin));
    settings.retype = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(retype_spin));
    settings.turbo_key_press = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(turbo_key_spin));
    settings.turbo_retype = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(turbo_retype_spin));

    const gchar* mod_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(modifier_combo));
    const gchar* key_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(key_combo));
    if (mod_id) settings.modifier = mod_id;
    if (key_id) settings.key = key_id;

    saved = save_settings(settings);
  }

  gtk_widget_destroy(dialog);
  return saved;
}

} // namespace punto
