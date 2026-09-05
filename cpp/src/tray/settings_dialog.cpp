/**
 * @file settings_dialog.cpp
 * @brief Реализация диалога настроек
 */

#include "punto/settings_dialog.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <glib.h>

#include "punto/config.hpp"
#include "punto/scancode_map.hpp"
#include "punto/types.hpp"

namespace punto {

namespace {

constexpr const char *kSystemConfigPath = "/etc/punto/config.yaml";

[[nodiscard]] GtkWidget *make_left_label(const char *text) {
  GtkWidget *lbl = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0);
  return lbl;
}

[[nodiscard]] GtkWidget *make_dim_label(const char *text) {
  GtkWidget *lbl = make_left_label(text);
  gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(lbl),
                              GTK_STYLE_CLASS_DIM_LABEL);
  return lbl;
}

struct SettingsDialogUiContext {
  // Auto-switch
  GtkSpinButton *threshold_spin = nullptr;
  GtkSpinButton *min_word_spin = nullptr;
  GtkSpinButton *min_score_spin = nullptr;
  GtkSpinButton *max_rollback_words_spin = nullptr;

  // Typo correction
  GtkToggleButton *typo_correction_check = nullptr;
  GtkSpinButton *max_typo_diff_spin = nullptr;
  GtkToggleButton *sticky_shift_check = nullptr;
  GtkToggleButton *sound_check = nullptr;

  // UI
  GtkWidget *save_button = nullptr;

  SettingsData initial{};
};

[[nodiscard]] bool nearly_equal_double(double a, double b) {
  return std::abs(a - b) < 1e-9;
}

[[nodiscard]] bool non_hotkey_changed(const SettingsData &a,
                                      const SettingsData &b) {
  if (!nearly_equal_double(a.threshold, b.threshold))
    return true;
  if (a.min_word_len != b.min_word_len)
    return true;
  if (!nearly_equal_double(a.min_score, b.min_score))
    return true;
  if (a.max_rollback_words != b.max_rollback_words)
    return true;

  // Typo correction
  if (a.typo_correction_enabled != b.typo_correction_enabled)
    return true;
  if (a.max_typo_diff != b.max_typo_diff)
    return true;
  if (a.sticky_shift_correction_enabled != b.sticky_shift_correction_enabled)
    return true;
  if (a.sound_enabled != b.sound_enabled)
    return true;

  return false;
}

[[nodiscard]] SettingsData
read_non_hotkey_from_ui(const SettingsDialogUiContext &ctx) {
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

  // Typo correction
  if (ctx.typo_correction_check) {
    out.typo_correction_enabled =
        gtk_toggle_button_get_active(ctx.typo_correction_check);
  }
  if (ctx.max_typo_diff_spin) {
    out.max_typo_diff =
        gtk_spin_button_get_value_as_int(ctx.max_typo_diff_spin);
  }
  if (ctx.sticky_shift_check) {
    out.sticky_shift_correction_enabled =
        gtk_toggle_button_get_active(ctx.sticky_shift_check);
  }
  if (ctx.sound_check) {
    out.sound_enabled = gtk_toggle_button_get_active(ctx.sound_check);
  }

  return out;
}

static void update_settings_dialog_state(SettingsDialogUiContext *ctx) {
  if (!ctx || !ctx->save_button) {
    return;
  }

  const SettingsData candidate = read_non_hotkey_from_ui(*ctx);
  const bool dirty = non_hotkey_changed(candidate, ctx->initial);
  gtk_widget_set_sensitive(ctx->save_button, dirty ? TRUE : FALSE);
}

static void on_any_setting_changed(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  auto *ctx = static_cast<SettingsDialogUiContext *>(user_data);
  update_settings_dialog_state(ctx);
}

[[nodiscard]] SettingsData settings_from_config(const Config &config) {
  SettingsData settings;
  settings.auto_enabled = config.auto_switch.enabled;
  settings.threshold = config.auto_switch.threshold;
  settings.min_word_len = static_cast<int>(config.auto_switch.min_word_len);
  settings.min_score = config.auto_switch.min_score;
  settings.max_rollback_words =
      static_cast<int>(config.auto_switch.max_rollback_words);
  settings.typo_correction_enabled = config.auto_switch.typo_correction_enabled;
  settings.max_typo_diff = static_cast<int>(config.auto_switch.max_typo_diff);
  settings.sticky_shift_correction_enabled =
      config.auto_switch.sticky_shift_correction_enabled;
  settings.sound_enabled = config.sound.enabled;
  if (const auto modifier = key_code_to_name(config.hotkey.modifier)) {
    settings.modifier = *modifier;
  }
  if (const auto key = key_code_to_name(config.hotkey.key)) {
    settings.key = *key;
  }
  return settings;
}

[[nodiscard]] bool merge_settings(const SettingsData &settings,
                                  Config &config) {
  const auto modifier = key_name_to_code(settings.modifier);
  const auto key = key_name_to_code(settings.key);
  if (!modifier || !key) {
    return false;
  }

  config.hotkey.modifier = *modifier;
  config.hotkey.key = *key;
  config.auto_switch.enabled = settings.auto_enabled;
  config.auto_switch.threshold = settings.threshold;
  config.auto_switch.min_word_len =
      static_cast<std::size_t>(settings.min_word_len);
  config.auto_switch.min_score = settings.min_score;
  config.auto_switch.max_rollback_words =
      static_cast<std::size_t>(settings.max_rollback_words);
  config.auto_switch.typo_correction_enabled = settings.typo_correction_enabled;
  config.auto_switch.max_typo_diff =
      static_cast<std::size_t>(settings.max_typo_diff);
  config.auto_switch.sticky_shift_correction_enabled =
      settings.sticky_shift_correction_enabled;
  config.sound.enabled = settings.sound_enabled;
  return validate_config(config);
}

[[nodiscard]] bool write_config_atomically(const std::filesystem::path &path,
                                           std::string_view contents) {
  GError *error = nullptr;
  // GLib defines this enum as a bitmask, so combined enumerators are valid.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto flags = static_cast<GFileSetContentsFlags>(
      G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE);
  const gboolean written = g_file_set_contents_full(
      path.c_str(), contents.data(), static_cast<gssize>(contents.size()),
      flags, 0600, &error);
  if (error != nullptr) {
    g_error_free(error);
  }
  return written == TRUE;
}

} // namespace

std::string SettingsDialog::get_user_config_path() {
  const char *home = std::getenv("HOME");
  if (!home || std::string(home).empty()) {
    // На всякий случай используем glib, если переменная HOME не прокинута
    const char *ghome = g_get_home_dir();
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
  const std::filesystem::path user_path = get_user_config_path();
  if (user_path.empty()) {
    return false;
  }

  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(user_path, error);
  if (!error && status.type() != std::filesystem::file_type::not_found) {
    return std::filesystem::is_regular_file(status);
  }
  if (error && error != std::errc::no_such_file_or_directory) {
    return false;
  }

  error.clear();
  std::filesystem::create_directories(user_path.parent_path(), error);
  if (error) {
    return false;
  }

  Config initial;
  const ConfigLoadOutcome system = load_config_checked(kSystemConfigPath);
  if (system.result == ConfigResult::Ok) {
    initial = system.config;
  } else if (system.result != ConfigResult::FileNotFound) {
    return false;
  }
  const std::optional<std::string> contents = serialize_config(initial);
  return contents && write_config_atomically(user_path, *contents);
}

SettingsData SettingsDialog::load_settings() {
  if (!ensure_user_config()) {
    return {};
  }

  const std::filesystem::path config_path = get_user_config_path();
  const ConfigLoadOutcome loaded = load_config_checked(config_path);
  if (loaded.result != ConfigResult::Ok) {
    return {};
  }
  return settings_from_config(loaded.config);
}

bool SettingsDialog::save_settings(const SettingsData &settings) {
  Config validation_candidate;
  if (!merge_settings(settings, validation_candidate)) {
    return false;
  }
  if (!ensure_user_config()) {
    return false;
  }

  const std::filesystem::path config_path = get_user_config_path();
  if (config_path.empty()) {
    return false;
  }

  const ConfigLoadOutcome loaded = load_config_checked(config_path);
  if (loaded.result != ConfigResult::Ok) {
    return false;
  }

  Config merged = loaded.config;
  if (!merge_settings(settings, merged)) {
    return false;
  }
  const std::optional<std::string> contents = serialize_config(merged);
  return contents && write_config_atomically(config_path, *contents);
}

bool SettingsDialog::show(GtkWidget *parent) {
  static GtkWidget *s_dialog_instance = nullptr;
  if (s_dialog_instance) {
    gtk_window_present(GTK_WINDOW(s_dialog_instance));
    return false;
  }

  // Загружаем текущие настройки
  const SettingsData initial_settings = load_settings();

  // Создаём диалог
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Настройки Punto Switcher", parent ? GTK_WINDOW(parent) : nullptr,
      GTK_DIALOG_MODAL, "_Отмена", GTK_RESPONSE_CANCEL, "_Сохранить",
      GTK_RESPONSE_ACCEPT, nullptr);

  s_dialog_instance = dialog;

  gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);

  GtkWidget *save_button = gtk_dialog_get_widget_for_response(
      GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  if (save_button) {
    gtk_widget_set_sensitive(save_button, FALSE);
  }

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  // Notebook для вкладок
  GtkWidget *notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);

  SettingsDialogUiContext ui_ctx;
  ui_ctx.initial = initial_settings;
  ui_ctx.save_button = save_button;

  // ===== Вкладка "Автопереключение" =====
  GtkWidget *auto_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(auto_box), 12);

  GtkWidget *auto_note = make_dim_label(
      "Исправление слов и выделенного текста работает в X11. "
      "Автоматический режим и ручные команды независимы; "
      "возможности зависят от версии запущенного сервиса.");
  gtk_box_pack_start(GTK_BOX(auto_box), auto_note, FALSE, FALSE, 0);

  // Grid для параметров
  GtkWidget *auto_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(auto_grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(auto_grid), 12);
  gtk_box_pack_start(GTK_BOX(auto_box), auto_grid, FALSE, FALSE, 8);

  // Threshold
  GtkWidget *threshold_lbl = make_left_label("Порог срабатывания:");
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_lbl, 0, 0, 1, 1);
  GtkWidget *threshold_spin = gtk_spin_button_new_with_range(0.5, 10.0, 0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(threshold_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(threshold_spin),
                            initial_settings.threshold);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(threshold_spin), 0.1, 0.1);
  gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(threshold_spin), FALSE);
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_spin, 1, 0, 1, 1);
  GtkWidget *threshold_desc =
      make_dim_label("Диапазон: 0.5–10.0. Чем выше значение — тем реже "
                     "анализ предлагает переключение.");
  gtk_grid_attach(GTK_GRID(auto_grid), threshold_desc, 0, 1, 2, 1);

  // Min word len
  GtkWidget *min_word_lbl = make_left_label("Мин. длина слова:");
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_lbl, 0, 2, 1, 1);
  GtkWidget *min_word_spin = gtk_spin_button_new_with_range(1, 10, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_word_spin),
                            initial_settings.min_word_len);
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_spin, 1, 2, 1, 1);
  GtkWidget *min_word_desc = make_dim_label(
      "Диапазон: 1–10. Слова короче этого значения не анализируются.");
  gtk_grid_attach(GTK_GRID(auto_grid), min_word_desc, 0, 3, 2, 1);

  // Min score
  GtkWidget *min_score_lbl = make_left_label("Мин. уверенность:");
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_lbl, 0, 4, 1, 1);
  GtkWidget *min_score_spin = gtk_spin_button_new_with_range(0.0, 20.0, 0.1);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(min_score_spin), 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_score_spin),
                            initial_settings.min_score);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(min_score_spin), 0.1, 0.1);
  gtk_spin_button_set_snap_to_ticks(GTK_SPIN_BUTTON(min_score_spin), FALSE);
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_spin, 1, 4, 1, 1);
  GtkWidget *min_score_desc =
      make_dim_label("Диапазон: 0.0–20.0. Чем выше значение — тем осторожнее "
                     "диагностическое решение анализа.");
  gtk_grid_attach(GTK_GRID(auto_grid), min_score_desc, 0, 5, 2, 1);

  // Max rollback words
  GtkWidget *rollback_lbl = make_left_label("Макс. откат слов:");
  gtk_grid_attach(GTK_GRID(auto_grid), rollback_lbl, 0, 6, 1, 1);
  GtkWidget *rollback_spin = gtk_spin_button_new_with_range(1, 50, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(rollback_spin),
                            initial_settings.max_rollback_words);
  gtk_grid_attach(GTK_GRID(auto_grid), rollback_spin, 1, 6, 1, 1);
  GtkWidget *rollback_desc = make_dim_label(
      "Диапазон: 1–50. Сколько последних слов можно повторно ввести "
      "при запоздавшем исправлении.");
  gtk_grid_attach(GTK_GRID(auto_grid), rollback_desc, 0, 7, 2, 1);

  // ===== Секция исправления опечаток =====
  gtk_box_pack_start(GTK_BOX(auto_box),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 6);
  gtk_box_pack_start(GTK_BOX(auto_box), make_left_label("Анализ ошибок:"),
                     FALSE, FALSE, 0);

  GtkWidget *typo_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(typo_grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(typo_grid), 12);
  gtk_box_pack_start(GTK_BOX(auto_box), typo_grid, FALSE, FALSE, 4);

  // Sticky shift correction
  GtkWidget *sticky_check = gtk_check_button_new_with_label(
      "Исправлять залипший Shift в поддерживаемых редакторах");
  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(sticky_check),
      initial_settings.sticky_shift_correction_enabled);
  gtk_grid_attach(GTK_GRID(typo_grid), sticky_check, 0, 0, 2, 1);

  // Typo correction
  GtkWidget *typo_check = gtk_check_button_new_with_label(
      "Исправлять опечатки в поддерживаемых редакторах (beta)");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(typo_check),
                               initial_settings.typo_correction_enabled);
  gtk_grid_attach(GTK_GRID(typo_grid), typo_check, 0, 1, 2, 1);

  // Max typo diff
  GtkWidget *typo_diff_lbl = make_left_label("Макс. расстояние:");
  gtk_grid_attach(GTK_GRID(typo_grid), typo_diff_lbl, 0, 2, 1, 1);
  GtkWidget *typo_diff_spin = gtk_spin_button_new_with_range(1, 2, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(typo_diff_spin),
                            initial_settings.max_typo_diff);
  gtk_grid_attach(GTK_GRID(typo_grid), typo_diff_spin, 1, 2, 1, 1);
  GtkWidget *typo_diff_desc = make_dim_label(
      "1 = только однобуквенные ошибки, 2 = включая двухбуквенные.");
  gtk_grid_attach(GTK_GRID(typo_grid), typo_diff_desc, 0, 3, 2, 1);

  GtkWidget *sound_check = gtk_check_button_new_with_label("Звук при смене раскладки");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound_check), initial_settings.sound_enabled);
  gtk_box_pack_start(GTK_BOX(auto_box), sound_check, FALSE, FALSE, 8);

  ui_ctx.threshold_spin = GTK_SPIN_BUTTON(threshold_spin);
  ui_ctx.min_word_spin = GTK_SPIN_BUTTON(min_word_spin);
  ui_ctx.min_score_spin = GTK_SPIN_BUTTON(min_score_spin);
  ui_ctx.max_rollback_words_spin = GTK_SPIN_BUTTON(rollback_spin);
  ui_ctx.sticky_shift_check = GTK_TOGGLE_BUTTON(sticky_check);
  ui_ctx.typo_correction_check = GTK_TOGGLE_BUTTON(typo_check);
  ui_ctx.max_typo_diff_spin = GTK_SPIN_BUTTON(typo_diff_spin);
  ui_ctx.sound_check = GTK_TOGGLE_BUTTON(sound_check);
  g_signal_connect(sound_check, "toggled", G_CALLBACK(on_any_setting_changed), &ui_ctx);

  g_signal_connect(threshold_spin, "value-changed",
                   G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(min_word_spin, "value-changed",
                   G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(min_score_spin, "value-changed",
                   G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(rollback_spin, "value-changed",
                   G_CALLBACK(on_any_setting_changed), &ui_ctx);
  g_signal_connect(sticky_check, "toggled", G_CALLBACK(on_any_setting_changed),
                   &ui_ctx);
  g_signal_connect(typo_check, "toggled", G_CALLBACK(on_any_setting_changed),
                   &ui_ctx);
  g_signal_connect(typo_diff_spin, "value-changed",
                   G_CALLBACK(on_any_setting_changed), &ui_ctx);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), auto_box,
                           gtk_label_new("Автопереключение"));

  // Первичное состояние кнопки "Сохранить".
  update_settings_dialog_state(&ui_ctx);

  // Показываем диалог
  gtk_widget_show_all(dialog);

  gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  bool saved = false;
  if (response == GTK_RESPONSE_ACCEPT) {
    const SettingsData new_settings = read_non_hotkey_from_ui(ui_ctx);

    const bool dirty = non_hotkey_changed(new_settings, initial_settings);
    if (dirty) {
      saved = save_settings(new_settings);
      if (!saved) {
        GtkWidget *error = gtk_message_dialog_new(
            GTK_WINDOW(dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE, "%s", "Не удалось подтвердить сохранение настроек.");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(error), "%s",
            "Сервису не отправлена команда применения. Откройте настройки повторно, "
            "чтобы проверить сохранённые значения.");
        (void)gtk_dialog_run(GTK_DIALOG(error));
        gtk_widget_destroy(error);
      }
    }
  }

  gtk_widget_destroy(dialog);
  s_dialog_instance = nullptr;
  return saved;
}

} // namespace punto
