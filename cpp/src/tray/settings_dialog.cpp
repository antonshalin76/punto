/**
 * @file settings_dialog.cpp
 * @brief Реализация диалога настроек
 */

#include "punto/settings_dialog.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string_view>

#include <glib.h>

#include "punto/system_input_settings.hpp"
#include "punto/types.hpp"

namespace punto {

namespace {

constexpr const char* kSystemConfigPath = "/etc/punto/config.yaml";

[[nodiscard]] GtkWidget* make_left_label(const char* text) {
  GtkWidget* lbl = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0);
  return lbl;
}

[[nodiscard]] GtkWidget* make_dim_label(const char* text) {
  GtkWidget* lbl = make_left_label(text);
  gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(lbl),
                              GTK_STYLE_CLASS_DIM_LABEL);
  return lbl;
}

struct SettingsDialogUiContext {
  // Auto-switch
  GtkSpinButton* threshold_spin = nullptr;
  GtkSpinButton* min_word_spin = nullptr;
  GtkSpinButton* min_score_spin = nullptr;
  GtkSpinButton* max_rollback_words_spin = nullptr;

  // Delays
  GtkSpinButton* key_press_spin = nullptr;
  GtkSpinButton* layout_spin = nullptr;
  GtkSpinButton* retype_spin = nullptr;
  GtkSpinButton* turbo_key_spin = nullptr;
  GtkSpinButton* turbo_retype_spin = nullptr;

  // Hotkey
  GtkComboBox* modifier_combo = nullptr;
  GtkComboBox* key_combo = nullptr;

  // UI
  GtkWidget* hotkey_hint_label = nullptr;
  GtkWidget* save_button = nullptr;

  SettingsData initial;
};

[[nodiscard]] std::string hotkey_supported_combos_text(std::string_view active_backend) {
  std::string backend_line = "Текущий backend: ";
  if (active_backend.empty()) {
    backend_line += "<не определён>";
  } else {
    backend_line += std::string{active_backend};
  }

  // Важно: перечисляем именно те комбинации, которые программа умеет применить.
  // GNOME: используем gsettings keybindings.
  // X11: используем XKB grp:*_toggle через setxkbmap.
  std::string text;
  text += backend_line;
  text += "\n\n";

  text += "GNOME (gsettings):\n";
  text += "  - 1 модификатор (Ctrl/Alt/Shift/Super) + 1 клавиша\n";
  text += "  - Поддерживаемые клавиши в UI: `, Space, Tab, Backslash, CapsLock, а также Shift/Ctrl/Alt/Super\n\n";

  text += "X11 (setxkbmap, grp:*_toggle):\n";
  text += "  - Alt+Shift (left/right варианты)\n";
  text += "  - Ctrl+Shift (left/right варианты)\n";
  text += "  - Ctrl+Alt\n";
  text += "  - Alt+Space\n";
  text += "  - Ctrl+Space\n";
  text += "  - Win+Space\n";
  text += "  - Shift+CapsLock\n";

  return text;
}

[[nodiscard]] bool nearly_equal_double(double a, double b) {
  return std::abs(a - b) < 1e-9;
}

[[nodiscard]] bool non_hotkey_changed(const SettingsData& a, const SettingsData& b) {
  if (!nearly_equal_double(a.threshold, b.threshold)) return true;
  if (a.min_word_len != b.min_word_len) return true;
  if (!nearly_equal_double(a.min_score, b.min_score)) return true;
  if (a.max_rollback_words != b.max_rollback_words) return true;

  if (a.key_press != b.key_press) return true;
  if (a.layout_switch != b.layout_switch) return true;
  if (a.retype != b.retype) return true;
  if (a.turbo_key_press != b.turbo_key_press) return true;
  if (a.turbo_retype != b.turbo_retype) return true;

  return false;
}

[[nodiscard]] SettingsData read_non_hotkey_from_ui(const SettingsDialogUiContext& ctx) {
  SettingsData out = ctx.initial;

  if (ctx.threshold_spin) {
    out.threshold = gtk_spin_button_get_value(ctx.threshold_spin);
  }
  if (ctx.min_word_spin) {
    out.min_word_len = gtk_spin_button_get_value_as_int(ctx.min_word_spin);
  }
  if (ctx.min_score_spin) {
    out.min_score = gtk_spin_button_get_value(ctx.min_score_spin);
  }
  if (ctx.max_rollback_words_spin) {
    out.max_rollback_words =
        gtk_spin_button_get_value_as_int(ctx.max_rollback_words_spin);
  }

  if (ctx.key_press_spin) {
    out.key_press = gtk_spin_button_get_value_as_int(ctx.key_press_spin);
  }
  if (ctx.layout_spin) {
    out.layout_switch = gtk_spin_button_get_value_as_int(ctx.layout_spin);
  }
  if (ctx.retype_spin) {
    out.retype = gtk_spin_button_get_value_as_int(ctx.retype_spin);
  }
  if (ctx.turbo_key_spin) {
    out.turbo_key_press = gtk_spin_button_get_value_as_int(ctx.turbo_key_spin);
  }
  if (ctx.turbo_retype_spin) {
    out.turbo_retype = gtk_spin_button_get_value_as_int(ctx.turbo_retype_spin);
  }

  return out;
}

[[nodiscard]] LayoutToggle read_selected_hotkey(const SettingsDialogUiContext& ctx) {
  LayoutToggle selected;
  if (!ctx.modifier_combo || !ctx.key_combo) {
    return selected;
  }

  const gchar* mod_id = gtk_combo_box_get_active_id(ctx.modifier_combo);
  const gchar* key_id = gtk_combo_box_get_active_id(ctx.key_combo);
  if (mod_id) {
    selected.modifier = mod_id;
  }
  if (key_id) {
    selected.key = key_id;
  }

  return selected;
}

static void update_settings_dialog_state(SettingsDialogUiContext* ctx) {
  if (!ctx || !ctx->hotkey_hint_label || !ctx->save_button) {
    return;
  }

  SettingsData candidate = read_non_hotkey_from_ui(*ctx);
  LayoutToggle selected = read_selected_hotkey(*ctx);
  const auto validation = SystemInputSettings::validate_layout_toggle(selected);

  const bool backend_known = !validation.backend.empty();
  const bool hotkey_selected = !selected.modifier.empty() && !selected.key.empty();
  const bool hotkey_changed = hotkey_selected &&
                              (selected.modifier != ctx->initial.modifier ||
                               selected.key != ctx->initial.key);
  const bool hotkey_applicable = validation.result == SystemInputResult::Ok;

  // Если backend определён и хоткей НЕ применим — изменения хоткея игнорируем.
  const bool will_save_hotkey = hotkey_changed && (!backend_known || hotkey_applicable);

  const bool dirty = non_hotkey_changed(candidate, ctx->initial) || will_save_hotkey;
  gtk_widget_set_sensitive(ctx->save_button, dirty);

  std::string text = hotkey_supported_combos_text(validation.backend);
  text += "\n\nВыбрано: ";
  text += selected.modifier.empty() ? "<модификатор?>" : selected.modifier;
  text += " + ";
  text += selected.key.empty() ? "<клавиша?>" : selected.key;

  if (!backend_known) {
    text += "\nСтатус: backend не определён (значение сохранится в конфиг; в систему применить нельзя)";
  } else if (hotkey_applicable) {
    if (hotkey_changed) {
      text += "\nСтатус: применимо (изменение будет применено в систему при сохранении)";
    } else {
      text += "\nСтатус: применимо";
    }
  } else {
    text += "\nСтатус: НЕ применимо";
    if (hotkey_changed) {
      text += "\nИзменение хоткея будет проигнорировано при сохранении (остальные параметры сохранятся).";
    }
    if (!validation.error.empty()) {
      text += "\n";
      text += validation.error;
    }
  }

  gtk_label_set_text(GTK_LABEL(ctx->hotkey_hint_label), text.c_str());
}

static void on_any_setting_changed(GtkWidget* widget, gpointer user_data) {
  (void)widget;
  auto* ctx = static_cast<SettingsDialogUiContext*>(user_data);
  update_settings_dialog_state(ctx);
}

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
    if (trimmed == "sound:") { section = "sound"; continue; }

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
      } else if (trimmed.find("max_rollback_words:") != std::string::npos) {
        settings.max_rollback_words = std::stoi(value);
      }
    } else if (section == "sound") {
      if (trimmed.find("enabled:") != std::string::npos) {
        settings.sound_enabled = (value == "true" || value == "1");
      }
    }
  }

  return settings;
}

bool SettingsDialog::save_settings(const SettingsData& settings) {
  if (!ensure_user_config()) {
    return false;
  }

  std::string config_path_str = get_user_config_path();
  if (config_path_str.empty()) {
    return false;
  }

  std::filesystem::path config_path{config_path_str};
  std::filesystem::path tmp_path = config_path;
  tmp_path += ".tmp";

  {
    std::ofstream file{tmp_path, std::ios::trunc};
    if (!file.is_open()) {
      return false;
    }

    // Гарантируем точку в числах независимо от локали пользователя.
    file.imbue(std::locale::classic());

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
    file << "  max_rollback_words: " << settings.max_rollback_words << "\n\n";

    file << "sound:\n";
    file << "  enabled: " << (settings.sound_enabled ? "true" : "false") << "\n";

    file.flush();
    if (!file.good()) {
      return false;
    }
  }

  // Атомарно заменяем файл (rename в пределах одной ФС атомарен).
  std::error_code ec;
  std::filesystem::rename(tmp_path, config_path, ec);
  if (ec) {
    // Best-effort cleanup временного файла.
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    return false;
  }

  return true;
}

bool SettingsDialog::show(GtkWidget* parent) {
  static GtkWidget* s_dialog_instance = nullptr;
  if (s_dialog_instance) {
    gtk_window_present(GTK_WINDOW(s_dialog_instance));
    return false;
  }

  // Загружаем текущие настройки
  const SettingsData initial_settings = load_settings();

  // Создаём диалог
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Настройки Punto Switcher",
      parent ? GTK_WINDOW(parent) : nullptr,
      GTK_DIALOG_MODAL,
      "_Отмена", GTK_RESPONSE_CANCEL,
      "_Сохранить", GTK_RESPONSE_ACCEPT,
      nullptr);

  s_dialog_instance = dialog;

  gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);

  GtkWidget* save_button = gtk_dialog_get_widget_for_response(
      GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  if (save_button) {
    gtk_widget_set_sensitive(save_button, FALSE);
  }

  GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  // Notebook для вкладок
  GtkWidget* notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);

  SettingsDialogUiContext ui_ctx;
  ui_ctx.initial = initial_settings;
  ui_ctx.save_button = save_button;

  // ===== Вкладка "Автопереключение" =====
  GtkWidget* auto_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(auto_box), 12);

  GtkWidget* auto_note = make_dim_label(
      "Включение/выключение автопереключения — в меню трея.");
  gtk_box_pack_start(GTK_BOX(auto_box), auto_note, FALSE, FALSE, 0);

  // Grid для параметров
  GtkWidget* auto_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(auto_grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(auto_grid), 12);
  gtk_box_pack_start(GTK_BOX(auto_box), auto_grid, FALSE, FALSE, 8);

  // Threshold
  GtkWidget* threshold_lbl = make_left_label("Порог срабатывания:");
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_lbl, 0, 0, 1, 1);
  GtkWidget* threshold_spin = gtk_spin_button_new_with_range(0.5, 10.0, 0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(threshold_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(threshold_spin), initial_settings.threshold);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(threshold_spin), 0.1, 0.1);
  gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(threshold_spin), FALSE);
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_spin, 1, 0, 1, 1);
  GtkWidget* threshold_desc = make_dim_label(
      "Диапазон: 0.5–10.0. Чем выше значение — тем реже срабатывает автопереключение.");
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_desc, 0, 1, 2, 1);

  // Min word len
  GtkWidget* min_word_lbl = make_left_label("Мин. длина слова:");
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_lbl, 0, 2, 1, 1);
  GtkWidget* min_word_spin = gtk_spin_button_new_with_range(1, 10, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_word_spin), initial_settings.min_word_len);
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_spin, 1, 2, 1, 1);
  GtkWidget* min_word_desc = make_dim_label(
      "Диапазон: 1–10. Слова короче этого значения не анализируются.");
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_desc, 0, 3, 2, 1);

  // Min score
  GtkWidget* min_score_lbl = make_left_label("Мин. уверенность:");
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_lbl, 0, 4, 1, 1);
  GtkWidget* min_score_spin = gtk_spin_button_new_with_range(0.0, 20.0, 0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(min_score_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_score_spin), initial_settings.min_score);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(min_score_spin), 0.1, 0.1);
  gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(min_score_spin), FALSE);
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_spin, 1, 4, 1, 1);
  GtkWidget* min_score_desc = make_dim_label(
      "Диапазон: 0.0–20.0. Чем выше значение — тем осторожнее решение о переключении.");
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_desc, 0, 5, 2, 1);

  // Max rollback words
  GtkWidget* rollback_lbl = make_left_label("Макс. откат слов:");
  gtk_grid_attach(GTK_GRID(auto_grid), rollback_lbl, 0, 6, 1, 1);
  GtkWidget* rollback_spin = gtk_spin_button_new_with_range(1, 50, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rollback_spin),
                            initial_settings.max_rollback_words);
  gtk_grid_attach(GTK_GRID(auto_grid), rollback_spin, 1, 6, 1, 1);
  GtkWidget* rollback_desc = make_dim_label(
      "Диапазон: 1–50. Сколько последних слов можно откатывать, чтобы исправить слово даже при задержке анализа.");
  gtk_grid_attach(GTK_GRID(auto_grid), rollback_desc, 0, 7, 2, 1);

  ui_ctx.threshold_spin = GTK_SPIN_BUTTON(threshold_spin);
  ui_ctx.min_word_spin = GTK_SPIN_BUTTON(min_word_spin);
  ui_ctx.min_score_spin = GTK_SPIN_BUTTON(min_score_spin);
  ui_ctx.max_rollback_words_spin = GTK_SPIN_BUTTON(rollback_spin);

  g_signal_connect(threshold_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(min_word_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(min_score_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(rollback_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), auto_box,
                           gtk_label_new("Автопереключение"));

  // ===== Вкладка "Задержки" =====
  GtkWidget* delays_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(delays_box), 12);

  GtkWidget* delays_note = make_dim_label(
      "Задержки влияют на совместимость: слишком маленькие значения могут приводить к пропускам в отдельных приложениях.");
  gtk_box_pack_start(GTK_BOX(delays_box), delays_note, FALSE, FALSE, 0);

  GtkWidget* delays_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(delays_grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(delays_grid), 12);
  gtk_box_pack_start(GTK_BOX(delays_box), delays_grid, FALSE, FALSE, 0);

  // Key press
  GtkWidget* key_press_lbl = make_left_label("Нажатие клавиши (мс):");
  gtk_grid_attach(GTK_GRID(delays_grid), key_press_lbl, 0, 0, 1, 1);
  GtkWidget* key_press_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(key_press_spin), initial_settings.key_press);
  gtk_grid_attach(GTK_GRID(delays_grid), key_press_spin, 1, 0, 1, 1);
  GtkWidget* key_press_desc = make_dim_label(
      "Диапазон: 1–100. Задержка между нажатием и отпусканием клавиши при эмуляции.");
  gtk_grid_attach(GTK_GRID(delays_grid), key_press_desc, 0, 1, 2, 1);

  // Layout switch
  GtkWidget* layout_lbl = make_left_label("Переключение раскладки (мс):");
  gtk_grid_attach(GTK_GRID(delays_grid), layout_lbl, 0, 2, 1, 1);
  GtkWidget* layout_spin = gtk_spin_button_new_with_range(10, 500, 10);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(layout_spin), initial_settings.layout_switch);
  gtk_grid_attach(GTK_GRID(delays_grid), layout_spin, 1, 2, 1, 1);
  GtkWidget* layout_desc = make_dim_label(
      "Диапазон: 10–500. Пауза после отправки хоткея смены раскладки.");
  gtk_grid_attach(GTK_GRID(delays_grid), layout_desc, 0, 3, 2, 1);

  // Retype
  GtkWidget* retype_lbl = make_left_label("Перепечатывание (мс):");
  gtk_grid_attach(GTK_GRID(delays_grid), retype_lbl, 0, 4, 1, 1);
  GtkWidget* retype_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(retype_spin), initial_settings.retype);
  gtk_grid_attach(GTK_GRID(delays_grid), retype_spin, 1, 4, 1, 1);
  GtkWidget* retype_desc = make_dim_label(
      "Диапазон: 1–100. Пауза между символами при перепечатывании.");
  gtk_grid_attach(GTK_GRID(delays_grid), retype_desc, 0, 5, 2, 1);

  // Separator
  gtk_box_pack_start(GTK_BOX(delays_box),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                     FALSE, FALSE, 6);
  gtk_box_pack_start(GTK_BOX(delays_box), make_left_label("Турбо-режим:"), FALSE, FALSE, 0);

  GtkWidget* turbo_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(turbo_grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(turbo_grid), 12);
  gtk_box_pack_start(GTK_BOX(delays_box), turbo_grid, FALSE, FALSE, 4);

  // Turbo key press
  GtkWidget* turbo_key_lbl = make_left_label("Турбо нажатие (мс):");
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_key_lbl, 0, 0, 1, 1);
  GtkWidget* turbo_key_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(turbo_key_spin), initial_settings.turbo_key_press);
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_key_spin, 1, 0, 1, 1);
  GtkWidget* turbo_key_desc = make_dim_label(
      "Диапазон: 1–100. Задержка нажатия в turbo-режиме (быстрее, но менее надёжно).");
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_key_desc, 0, 1, 2, 1);

  // Turbo retype
  GtkWidget* turbo_retype_lbl = make_left_label("Турбо перепечатка (мс):");
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_retype_lbl, 0, 2, 1, 1);
  GtkWidget* turbo_retype_spin = gtk_spin_button_new_with_range(1, 100, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(turbo_retype_spin), initial_settings.turbo_retype);
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_retype_spin, 1, 2, 1, 1);
  GtkWidget* turbo_retype_desc = make_dim_label(
      "Диапазон: 1–100. Пауза между символами в turbo-режиме.");
  gtk_grid_attach(GTK_GRID(turbo_grid), turbo_retype_desc, 0, 3, 2, 1);

  ui_ctx.key_press_spin = GTK_SPIN_BUTTON(key_press_spin);
  ui_ctx.layout_spin = GTK_SPIN_BUTTON(layout_spin);
  ui_ctx.retype_spin = GTK_SPIN_BUTTON(retype_spin);
  ui_ctx.turbo_key_spin = GTK_SPIN_BUTTON(turbo_key_spin);
  ui_ctx.turbo_retype_spin = GTK_SPIN_BUTTON(turbo_retype_spin);

  g_signal_connect(key_press_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(layout_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(retype_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(turbo_key_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(turbo_retype_spin, "value-changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), delays_box,
                           gtk_label_new("Задержки"));

  // ===== Вкладка "Горячие клавиши" =====
  GtkWidget* hotkey_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(hotkey_box), 12);

  GtkWidget* builtin_label = make_left_label(
      "Встроенные горячие клавиши:\n"
      "  Pause — инвертировать раскладку слова\n"
      "  Shift+Pause — инвертировать раскладку выделения\n"
      "  Ctrl+Pause — инвертировать регистр слова\n"
      "  Alt+Pause — инвертировать регистр выделения\n"
      "  LCtrl+LAlt+Pause — транслитерировать выделение");
  gtk_label_set_line_wrap(GTK_LABEL(builtin_label), TRUE);
  gtk_box_pack_start(GTK_BOX(hotkey_box), builtin_label, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(hotkey_box),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                     FALSE, FALSE, 6);

  GtkWidget* hotkey_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(hotkey_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(hotkey_grid), 12);
  gtk_box_pack_start(GTK_BOX(hotkey_box), hotkey_grid, FALSE, FALSE, 0);

  // Modifier combo
  gtk_grid_attach(GTK_GRID(hotkey_grid), make_left_label("Модификатор:"), 0, 0, 1, 1);
  GtkWidget* modifier_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftctrl", "Left Ctrl");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightctrl", "Right Ctrl");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftalt", "Left Alt");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightalt", "Right Alt");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftshift", "Left Shift");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightshift", "Right Shift");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "leftmeta", "Left Super");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(modifier_combo), "rightmeta", "Right Super");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(modifier_combo), initial_settings.modifier.c_str());
  gtk_grid_attach(GTK_GRID(hotkey_grid), modifier_combo, 1, 0, 1, 1);

  // Key combo
  gtk_grid_attach(GTK_GRID(hotkey_grid), make_left_label("Клавиша:"), 0, 1, 1, 1);
  GtkWidget* key_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "grave", "` (Grave)");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "space", "Space");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "tab", "Tab");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "backslash", "\\ (Backslash)");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "capslock", "Caps Lock");

  // Модификаторы тоже могут выступать "второй клавишей" (Alt+Shift, Ctrl+Alt и т.п.)
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "leftshift", "Left Shift");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "rightshift", "Right Shift");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "leftalt", "Left Alt");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "rightalt", "Right Alt");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "leftctrl", "Left Ctrl");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "rightctrl", "Right Ctrl");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "leftmeta", "Left Super");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(key_combo), "rightmeta", "Right Super");

  gtk_combo_box_set_active_id(GTK_COMBO_BOX(key_combo), initial_settings.key.c_str());
  gtk_grid_attach(GTK_GRID(hotkey_grid), key_combo, 1, 1, 1, 1);

  ui_ctx.modifier_combo = GTK_COMBO_BOX(modifier_combo);
  ui_ctx.key_combo = GTK_COMBO_BOX(key_combo);

  g_signal_connect(modifier_combo, "changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(key_combo, "changed", G_CALLBACK(on_any_setting_changed), &ui_ctx);

  // Подсказка: какие комбинации применимы в GNOME/X11 + применимость выбранного значения.
  GtkWidget* hotkey_hint_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(hotkey_hint_label), 0);
  gtk_label_set_line_wrap(GTK_LABEL(hotkey_hint_label), TRUE);
  gtk_widget_set_margin_top(hotkey_hint_label, 8);
  gtk_box_pack_start(GTK_BOX(hotkey_box), hotkey_hint_label, FALSE, FALSE, 0);
  ui_ctx.hotkey_hint_label = hotkey_hint_label;

  GtkWidget* note_label = make_dim_label(
      "Примечание: это хоткей переключения раскладки, который punto эмулирует.\n"
      "Он должен совпадать с системными настройками.\n"
      "KDE/Plasma: автоматическая синхронизация пока не поддерживается.");
  gtk_widget_set_margin_top(note_label, 8);
  gtk_box_pack_start(GTK_BOX(hotkey_box), note_label, FALSE, FALSE, 0);

  // Текущий системный хоткей (информативно)
  const auto sys = SystemInputSettings::read_layout_toggle();
  std::string sys_text;
  if (sys.result == SystemInputResult::Ok && sys.toggle) {
    sys_text = "Сейчас в системе (" + sys.backend + "): " + sys.toggle->modifier +
               " + " + sys.toggle->key;
  } else if (sys.result == SystemInputResult::Unsupported) {
    sys_text = "Сейчас в системе (" + sys.backend + "): " +
               (sys.raw.empty() ? std::string{"<unknown>"} : sys.raw) +
               "\n" + sys.error;
  } else {
    sys_text = "Системный хоткей недоступен: " + sys.error;
  }

  GtkWidget* sys_label = make_left_label(sys_text.c_str());
  gtk_label_set_line_wrap(GTK_LABEL(sys_label), TRUE);
  gtk_widget_set_margin_top(sys_label, 8);
  gtk_box_pack_start(GTK_BOX(hotkey_box), sys_label, FALSE, FALSE, 0);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), hotkey_box,
                           gtk_label_new("Горячие клавиши"));

  // Первичное состояние кнопки "Сохранить" + подсказки.
  update_settings_dialog_state(&ui_ctx);

  // Показываем диалог
  gtk_widget_show_all(dialog);

  gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  bool saved = false;
  if (response == GTK_RESPONSE_ACCEPT) {
    SettingsData new_settings = initial_settings;

    // Читаем значения из виджетов (без "Звук" и без enable-флага авто-переключения).
    new_settings.threshold = gtk_spin_button_get_value(GTK_SPIN_BUTTON(threshold_spin));
    new_settings.min_word_len = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(min_word_spin));
    new_settings.min_score = gtk_spin_button_get_value(GTK_SPIN_BUTTON(min_score_spin));
    new_settings.max_rollback_words =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(rollback_spin));

    new_settings.key_press = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(key_press_spin));
    new_settings.layout_switch = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(layout_spin));
    new_settings.retype = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(retype_spin));
    new_settings.turbo_key_press = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(turbo_key_spin));
    new_settings.turbo_retype = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(turbo_retype_spin));

    const gchar* mod_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(modifier_combo));
    const gchar* key_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(key_combo));

    LayoutToggle selected_hotkey;
    if (mod_id) selected_hotkey.modifier = mod_id;
    if (key_id) selected_hotkey.key = key_id;

    const bool hotkey_changed = !selected_hotkey.modifier.empty() &&
                               !selected_hotkey.key.empty() &&
                               (selected_hotkey.modifier != initial_settings.modifier ||
                                selected_hotkey.key != initial_settings.key);

    const auto validation = SystemInputSettings::validate_layout_toggle(selected_hotkey);
    const bool backend_known = !validation.backend.empty();
    const bool hotkey_applicable = validation.result == SystemInputResult::Ok;

    // Если хоткей НЕ применим для текущего backend — не сохраняем изменения хоткея.
    const bool save_hotkey = hotkey_changed && (!backend_known || hotkey_applicable);
    if (save_hotkey) {
      new_settings.modifier = selected_hotkey.modifier;
      new_settings.key = selected_hotkey.key;
    }

    const bool dirty = non_hotkey_changed(new_settings, initial_settings) || save_hotkey;
    if (dirty) {
      saved = save_settings(new_settings);

      // Хоткей применяем в систему только если он изменён и применим.
      if (saved && hotkey_changed && backend_known && hotkey_applicable) {
        const auto res = SystemInputSettings::write_layout_toggle(
            LayoutToggle{new_settings.modifier, new_settings.key});
        if (res.result != SystemInputResult::Ok) {
          std::string msg = "Не удалось применить хоткей в системе.";
          if (!res.backend.empty()) {
            msg += "\nBackend: ";
            msg += res.backend;
          }
          if (!res.error.empty()) {
            msg += "\n";
            msg += res.error;
          }

          GtkWidget* warn = gtk_message_dialog_new(
              GTK_WINDOW(dialog),
              GTK_DIALOG_MODAL,
              GTK_MESSAGE_WARNING,
              GTK_BUTTONS_OK,
              "%s",
              msg.c_str());
          (void)gtk_dialog_run(GTK_DIALOG(warn));
          gtk_widget_destroy(warn);
        }
      }
    }
  }

  gtk_widget_destroy(dialog);
  s_dialog_instance = nullptr;
  return saved;
}

} // namespace punto
