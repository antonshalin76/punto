/**
 * @file tray_app.hpp
 * @brief Tray-приложение для управления punto сервисом
 *
 * Отображает иконку в системном трее и предоставляет
 * меню для управления сервисом.
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <gtk/gtk.h>

// Поддержка как Ayatana (Ubuntu 22.04+), так и legacy AppIndicator
#ifdef HAVE_AYATANA_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif

#include "punto/ipc_client.hpp"

namespace punto {

struct TrayAppTestAccess;

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
  TrayApp(const TrayApp &) = delete;
  TrayApp &operator=(const TrayApp &) = delete;

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
  friend struct TrayAppTestAccess;

  struct StatusPollState;
  struct StatusPollResult;

  // Callbacks для пунктов меню
  static void on_settings_clicked(GtkMenuItem *item, gpointer user_data);
  static void on_about_clicked(GtkMenuItem *item, gpointer user_data);
  static void on_quit_clicked(GtkMenuItem *item, gpointer user_data);
  static void on_auto_toggled(GtkCheckMenuItem *item, gpointer user_data);

  // Callback для периодического обновления статуса
  static gboolean on_status_update(gpointer user_data);
  static gboolean on_status_result(gpointer user_data);

  void request_status_update(std::optional<bool> requested_enabled = std::nullopt);

  /// Обновляет иконку в соответствии с текущим статусом
  void update_icon();

  /// Обновляет состояние пункта меню автопереключения
  void update_auto_toggle_state();

  /// Создаёт контекстное меню
  GtkWidget *create_menu();

  // GTK компоненты
  AppIndicator *indicator_ = nullptr;
  GtkWidget *menu_ = nullptr;
  GtkWidget *toggle_item_ = nullptr;
  GtkWidget *sound_settings_item_ = nullptr;

  // Текущий статус
  ServiceStatus current_status_ = ServiceStatus::Unknown;
  MutationCapability current_capability_ = MutationCapability::Unknown;
  bool updating_toggle_ = false;
  bool last_command_failed_ = false;

  // ID таймера обновления статуса
  guint status_timer_id_ = 0;

  std::shared_ptr<StatusPollState> status_poll_state_;
  std::function<IpcClientResult()> status_provider_;
  std::function<bool(bool)> status_setter_;
  std::function<bool(const std::string &)> config_reloader_;
};

} // namespace punto
