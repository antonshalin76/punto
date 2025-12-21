/**
 * @file tray_app.cpp
 * @brief Реализация tray-приложения
 */

#include "punto/tray_app.hpp"
#include "punto/settings_dialog.hpp"

#include <cstdlib>
#include <fstream>

namespace punto {

namespace {

/// Путь к системному конфигурационному файлу
constexpr const char* kSystemConfigPath = "/etc/punto/config.yaml";

/// Интервал обновления статуса (мс)
constexpr guint kStatusUpdateIntervalMs = 2000;

/// Имена иконок (используем стандартные темы)
constexpr const char* kIconEnabled = "input-keyboard";
constexpr const char* kIconDisabled = "input-keyboard-symbolic";
constexpr const char* kIconUnknown = "dialog-question";

/// ID приложения для AppIndicator
constexpr const char* kAppIndicatorId = "punto-switcher";

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
  indicator_ = app_indicator_new(
      kAppIndicatorId,
      kIconUnknown,
      APP_INDICATOR_CATEGORY_APPLICATION_STATUS
  );

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
  update_toggle_label();

  // Запускаем периодическое обновление статуса
  status_timer_id_ = g_timeout_add(kStatusUpdateIntervalMs, on_status_update, this);

  return true;
}

int TrayApp::run() {
  gtk_main();
  return 0;
}

GtkWidget* TrayApp::create_menu() {
  GtkWidget* menu = gtk_menu_new();

  // Toggle (Включить/Выключить)
  toggle_item_ = gtk_menu_item_new_with_label("Выключить");
  g_signal_connect(toggle_item_, "activate", G_CALLBACK(on_toggle_clicked), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle_item_);

  // Разделитель
  GtkWidget* sep1 = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep1);

  // Настройки
  GtkWidget* settings_item = gtk_menu_item_new_with_label("Настройки...");
  g_signal_connect(settings_item, "activate", G_CALLBACK(on_settings_clicked), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings_item);

  // Разделитель
  GtkWidget* sep2 = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep2);

  // Выход
  GtkWidget* quit_item = gtk_menu_item_new_with_label("Выход");
  g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_clicked), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

  gtk_widget_show_all(menu);
  return menu;
}

void TrayApp::update_icon() {
  const char* icon_name = kIconUnknown;

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

void TrayApp::update_toggle_label() {
  if (!toggle_item_) {
    return;
  }

  const char* label = "Переключить";

  switch (current_status_) {
  case ServiceStatus::Enabled:
    label = "Выключить";
    break;
  case ServiceStatus::Disabled:
    label = "Включить";
    break;
  case ServiceStatus::Unknown:
    label = "Переключить";
    break;
  }

  gtk_menu_item_set_label(GTK_MENU_ITEM(toggle_item_), label);
}

void TrayApp::on_toggle_clicked(GtkMenuItem* item, gpointer user_data) {
  (void)item;
  auto* app = static_cast<TrayApp*>(user_data);

  ServiceStatus new_status = IpcClient::toggle_status();
  if (new_status == ServiceStatus::Unknown) {
    return;  // переключение не удалось, сохраняем текущее состояние
  }

  // Обновляем UI только после успешного ответа от сервиса
  app->current_status_ = new_status;
  app->update_icon();
  app->update_toggle_label();
}

void TrayApp::on_settings_clicked(GtkMenuItem* item, gpointer user_data) {
  (void)item;
  auto* app = static_cast<TrayApp*>(user_data);

  // Показываем диалог настроек
  bool saved = SettingsDialog::show(nullptr);
  
  if (saved) {
    // Автоматически применяем настройки после сохранения
    bool success = IpcClient::reload_config();
    if (success) {
      // Обновляем статус
      app->current_status_ = IpcClient::get_status();
      app->update_icon();
      app->update_toggle_label();
    }
  }
}

void TrayApp::on_apply_clicked(GtkMenuItem* item, gpointer user_data) {
  (void)item;
  auto* app = static_cast<TrayApp*>(user_data);

  bool success = IpcClient::reload_config();
  
  if (success) {
    // Обновляем статус после перезагрузки
    app->current_status_ = IpcClient::get_status();
    app->update_icon();
    app->update_toggle_label();
  }
}

void TrayApp::on_quit_clicked(GtkMenuItem* item, gpointer user_data) {
  (void)item;
  (void)user_data;

  gtk_main_quit();
}

gboolean TrayApp::on_status_update(gpointer user_data) {
  auto* app = static_cast<TrayApp*>(user_data);

  ServiceStatus new_status = IpcClient::get_status();
  
  if (new_status != app->current_status_) {
    app->current_status_ = new_status;
    app->update_icon();
    app->update_toggle_label();
  }

  return G_SOURCE_CONTINUE;
}

} // namespace punto
