/**
 * @file tray_app.cpp
 * @brief Реализация tray-приложения
 */

#include "punto/tray_app.hpp"
#include "punto/settings_dialog.hpp"

#include <cstdlib>
#include <string>

#include <glib.h>

namespace punto {

namespace {

void on_about_dialog_response(GtkDialog *dialog, gint response_id,
                              gpointer user_data) {
  (void)response_id;
  auto **instance = static_cast<GtkWidget **>(user_data);
  if (instance) {
    *instance = nullptr;
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

gboolean on_about_label_activate_link(GtkLabel *label, gchar *uri,
                                      gpointer user_data) {
  (void)label;
  auto *parent = GTK_WINDOW(user_data);

  GError *error = nullptr;
  (void)gtk_show_uri_on_window(parent, uri, GDK_CURRENT_TIME, &error);
  if (error) {
    g_error_free(error);
  }

  return TRUE; // мы обработали ссылку
}

/// Интервал обновления статуса (мс)
constexpr guint kStatusUpdateIntervalMs = 2000;

/// Имена иконок (используем стандартные темы)
constexpr const char *kIconEnabled = "input-keyboard";
constexpr const char *kIconDisabled = "input-keyboard-symbolic";
constexpr const char *kIconUnknown = "dialog-question";

/// ID приложения для AppIndicator
constexpr const char *kAppIndicatorId = "punto-switcher";

bool apply_settings_change_with_reload(const SettingsData &old_settings,
                                       const SettingsData &new_settings) {
  if (!SettingsDialog::save_settings(new_settings)) {
    return false;
  }

  const std::string cfg_path = SettingsDialog::get_user_config_path();
  if (cfg_path.empty()) {
    (void)SettingsDialog::save_settings(old_settings);
    return false;
  }

  if (!IpcClient::reload_config(cfg_path)) {
    (void)SettingsDialog::save_settings(old_settings);
    return false;
  }

  return true;
}

} // namespace

TrayApp::TrayApp() = default;

TrayApp::~TrayApp() {
  if (status_timer_id_ != 0) {
    g_source_remove(status_timer_id_);
  }

  if (menu_) {
    gtk_widget_destroy(menu_);
  }

  if (indicator_) {
    g_object_unref(indicator_);
  }
}

bool TrayApp::initialize() {
  // Создаём AppIndicator
  indicator_ = app_indicator_new(kAppIndicatorId, kIconUnknown,
                                 APP_INDICATOR_CATEGORY_APPLICATION_STATUS);

  if (!indicator_) {
    return false;
  }

  // Устанавливаем статус (видимый)
  app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);

  // Устанавливаем заголовок
  app_indicator_set_title(indicator_, "Punto Switcher");

  // Создаём меню
  menu_ = create_menu();
  app_indicator_set_menu(indicator_, GTK_MENU(menu_));

  // Получаем начальный статус
  current_status_ = IpcClient::get_status();
  update_icon();
  update_auto_toggle_state();

  // Загружаем текущую настройку звука из user config
  sound_enabled_ = SettingsDialog::load_settings().sound_enabled;
  update_sound_toggle_state();

  // Запускаем периодическое обновление статуса
  status_timer_id_ =
      g_timeout_add(kStatusUpdateIntervalMs, on_status_update, this);

  return true;
}

int TrayApp::run() {
  gtk_main();
  return 0;
}

GtkWidget *TrayApp::create_menu() {
  GtkWidget *menu = gtk_menu_new();

  // Автопереключение (toggle)
  toggle_item_ = gtk_check_menu_item_new_with_label("Автопереключение");
  g_signal_connect(toggle_item_, "toggled", G_CALLBACK(on_auto_toggle_changed),
                   this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle_item_);

  // Звук (toggle)
  sound_toggle_item_ = gtk_check_menu_item_new_with_label("Звук");
  g_signal_connect(sound_toggle_item_, "toggled",
                   G_CALLBACK(on_sound_toggle_changed), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sound_toggle_item_);

  // Разделитель
  GtkWidget *sep1 = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep1);

  // Настройки
  GtkWidget *settings_item = gtk_menu_item_new_with_label("Настройки...");
  g_signal_connect(settings_item, "activate", G_CALLBACK(on_settings_clicked),
                   this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings_item);

  // О программе
  GtkWidget *about_item = gtk_menu_item_new_with_label("О программе");
  g_signal_connect(about_item, "activate", G_CALLBACK(on_about_clicked), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), about_item);

  // Разделитель
  GtkWidget *sep2 = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep2);

  // Выход
  GtkWidget *quit_item = gtk_menu_item_new_with_label("Выход");
  g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_clicked), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

  gtk_widget_show_all(menu);
  return menu;
}

void TrayApp::update_icon() {
  const char *icon_name = kIconUnknown;

  switch (current_status_) {
  case ServiceStatus::Enabled:
    icon_name = kIconEnabled;
    break;
  case ServiceStatus::Disabled:
    icon_name = kIconDisabled;
    break;
  case ServiceStatus::Unknown:
    icon_name = kIconUnknown;
    break;
  }

  app_indicator_set_icon(indicator_, icon_name);
}

void TrayApp::update_auto_toggle_state() {
  if (!toggle_item_) {
    return;
  }

  suppress_menu_signals_ = true;

  auto *item = GTK_CHECK_MENU_ITEM(toggle_item_);

  if (current_status_ == ServiceStatus::Unknown) {
    gtk_check_menu_item_set_inconsistent(item, TRUE);
    gtk_check_menu_item_set_active(item, FALSE);
  } else {
    gtk_check_menu_item_set_inconsistent(item, FALSE);
    gtk_check_menu_item_set_active(item,
                                   current_status_ == ServiceStatus::Enabled);
  }

  suppress_menu_signals_ = false;
}

void TrayApp::update_sound_toggle_state() {
  if (!sound_toggle_item_) {
    return;
  }

  suppress_menu_signals_ = true;

  auto *item = GTK_CHECK_MENU_ITEM(sound_toggle_item_);
  gtk_check_menu_item_set_inconsistent(item, FALSE);
  gtk_check_menu_item_set_active(item, sound_enabled_);

  suppress_menu_signals_ = false;
}

void TrayApp::on_auto_toggle_changed(GtkCheckMenuItem *item,
                                     gpointer user_data) {
  auto *app = static_cast<TrayApp *>(user_data);
  if (!app || app->suppress_menu_signals_) {
    return;
  }

  const bool enabled = gtk_check_menu_item_get_active(item);

  SettingsData old_settings = SettingsDialog::load_settings();
  SettingsData new_settings = old_settings;
  new_settings.auto_enabled = enabled;

  if (!apply_settings_change_with_reload(old_settings, new_settings)) {
    // Откатываем UI в исходное состояние.
    app->suppress_menu_signals_ = true;
    gtk_check_menu_item_set_active(item, old_settings.auto_enabled);
    app->suppress_menu_signals_ = false;
    return;
  }

  app->current_status_ = IpcClient::get_status();
  app->update_icon();
  app->update_auto_toggle_state();
}

void TrayApp::on_sound_toggle_changed(GtkCheckMenuItem *item,
                                      gpointer user_data) {
  auto *app = static_cast<TrayApp *>(user_data);
  if (!app || app->suppress_menu_signals_) {
    return;
  }

  const bool enabled = gtk_check_menu_item_get_active(item);

  SettingsData old_settings = SettingsDialog::load_settings();
  SettingsData new_settings = old_settings;
  new_settings.sound_enabled = enabled;

  if (!apply_settings_change_with_reload(old_settings, new_settings)) {
    // Откатываем UI в исходное состояние.
    app->suppress_menu_signals_ = true;
    gtk_check_menu_item_set_active(item, old_settings.sound_enabled);
    app->suppress_menu_signals_ = false;
    return;
  }

  app->sound_enabled_ = new_settings.sound_enabled;
  app->update_sound_toggle_state();

  // RELOAD может также синхронизировать статус автопереключения с конфигом.
  ServiceStatus new_status = IpcClient::get_status();
  if (new_status != ServiceStatus::Unknown &&
      new_status != app->current_status_) {
    app->current_status_ = new_status;
    app->update_icon();
    app->update_auto_toggle_state();
  }
}

void TrayApp::on_settings_clicked(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  auto *app = static_cast<TrayApp *>(user_data);

  // Показываем диалог настроек
  bool saved = SettingsDialog::show(nullptr);

  if (saved) {
    // Автоматически применяем настройки после сохранения
    const std::string cfg_path = SettingsDialog::get_user_config_path();
    bool success = IpcClient::reload_config(cfg_path);
    if (success) {
      // Обновляем статус
      app->current_status_ = IpcClient::get_status();
      app->update_icon();
      app->update_auto_toggle_state();
    }

    // Обновляем статус звука из конфига (даже если сервис сейчас недоступен)
    app->sound_enabled_ = SettingsDialog::load_settings().sound_enabled;
    app->update_sound_toggle_state();
  }
}

void TrayApp::on_about_clicked(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  (void)user_data;

  static GtkWidget *s_about_dialog = nullptr;
  if (s_about_dialog) {
    gtk_window_present(GTK_WINDOW(s_about_dialog));
    return;
  }

  GtkWidget *dialog =
      gtk_dialog_new_with_buttons("О программе", nullptr, GTK_DIALOG_MODAL,
                                  "_Закрыть", GTK_RESPONSE_CLOSE, nullptr);

  s_about_dialog = dialog;

  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

  // Не показываем иконку приложения в заголовке (часто выглядит как
  // красная/дефолтная).
  gtk_window_set_icon(GTK_WINDOW(dialog), nullptr);
  gtk_window_set_icon_name(GTK_WINDOW(dialog), nullptr);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 14);

  const char *markup =
      "<b>Punto Switcher for Linux</b>\n"
      "Version 2.8.4\n"
      "Лицензия: Personal Use Only\n"
      "Автор: Anton Shalin\n"
      "email: <a "
      "href=\"mailto:anton.shalin@gmail.com\">anton.shalin@gmail.com</a>\n";

  GtkWidget *label = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

  g_signal_connect(label, "activate-link",
                   G_CALLBACK(on_about_label_activate_link), dialog);

  g_signal_connect(dialog, "response", G_CALLBACK(on_about_dialog_response),
                   &s_about_dialog);

  gtk_widget_show_all(dialog);
}

void TrayApp::on_quit_clicked(GtkMenuItem *item, gpointer user_data) {
  (void)item;
  (void)user_data;

  gtk_main_quit();
}

gboolean TrayApp::on_status_update(gpointer user_data) {
  auto *app = static_cast<TrayApp *>(user_data);

  ServiceStatus new_status = IpcClient::get_status();

  if (new_status != app->current_status_) {
    app->current_status_ = new_status;
    app->update_icon();
    app->update_auto_toggle_state();
  }

  return G_SOURCE_CONTINUE;
}

} // namespace punto
