/**
 * @file tray_app.hpp
 * @brief Tray-приложение для управления punto сервисом
 *
 * Отображает иконку в системном трее и предоставляет
 * меню для управления сервисом.
 */

#pragma once

#include <gtk/gtk.h>

// Поддержка как Ayatana (Ubuntu 22.04+), так и legacy AppIndicator
#ifdef HAVE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif

#include "punto/ipc_client.hpp"

namespace punto {

/**
 * @brief Класс tray-приложения
 *
 * Управляет иконкой в трее, контекстным меню и
 * периодическим обновлением статуса.
 */
class TrayApp {
public:
  TrayApp();
  ~TrayApp();

  // Запрет копирования
  TrayApp(const TrayApp&) = delete;
  TrayApp& operator=(const TrayApp&) = delete;

  /**
   * @brief Инициализирует приложение
   * @return true при успехе
   */
  bool initialize();

  /**
   * @brief Запускает главный цикл GTK
   * @return Код возврата
   */
  int run();

private:
  // Callbacks для пунктов меню
  static void on_toggle_clicked(GtkMenuItem* item, gpointer user_data);
  static void on_sound_toggle_clicked(GtkMenuItem* item, gpointer user_data);
  static void on_restart_service_clicked(GtkMenuItem* item, gpointer user_data);
  static void on_settings_clicked(GtkMenuItem* item, gpointer user_data);
  static void on_apply_clicked(GtkMenuItem* item, gpointer user_data);
  static void on_quit_clicked(GtkMenuItem* item, gpointer user_data);

  // Callback для периодического обновления статуса
  static gboolean on_status_update(gpointer user_data);

  /// Обновляет иконку в соответствии с текущим статусом
  void update_icon();

  /// Обновляет текст пункта меню Toggle
  void update_toggle_label();

  /// Обновляет текст пункта меню Sound Toggle
  void update_sound_toggle_label();

  /// Обновляет текст пункта меню Service
  void update_service_label();

  /// Создаёт контекстное меню
  GtkWidget* create_menu();

  // GTK компоненты
  AppIndicator* indicator_ = nullptr;
  GtkWidget* menu_ = nullptr;
  GtkWidget* toggle_item_ = nullptr;
  GtkWidget* sound_toggle_item_ = nullptr;
  GtkWidget* service_item_ = nullptr;

  // Текущий статус
  ServiceStatus current_status_ = ServiceStatus::Unknown;

  // Текущий статус звука (берём из user config)
  bool sound_enabled_ = true;

  // ID таймера обновления статуса
  guint status_timer_id_ = 0;
};

} // namespace punto
