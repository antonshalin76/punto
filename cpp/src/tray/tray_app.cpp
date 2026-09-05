/**
 * @file tray_app.cpp
 * @brief Реализация tray-приложения
 */

#include "punto/tray_app.hpp"
#include "punto/settings_dialog.hpp"

#include <atomic>
#include <cstdint>
#include <new>
#include <string>
#include <thread>
#include <utility>

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

} // namespace

struct TrayApp::StatusPollState {
  std::atomic<TrayApp *> owner{nullptr};
  std::atomic<bool> in_flight{false};
  std::atomic<std::uint64_t> generation{0};
};

struct TrayApp::StatusPollResult {
  std::shared_ptr<StatusPollState> state;
  IpcClientResult snapshot;
  std::optional<bool> command_failed;
  std::uint64_t generation = 0;
};

TrayApp::TrayApp()
    : status_poll_state_{std::make_shared<StatusPollState>()},
      status_provider_{[] { return IpcClient::get_runtime_snapshot(); }},
      status_setter_{[](bool enabled) { return IpcClient::set_auto_enabled(enabled); }},
      config_reloader_{[](const std::string &path) { return IpcClient::reload_config(path); }} {
  status_poll_state_->owner.store(this, std::memory_order_release);
}

TrayApp::~TrayApp() {
  status_poll_state_->owner.store(nullptr, std::memory_order_release);
  (void)status_poll_state_->generation.fetch_add(1, std::memory_order_acq_rel);

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

  // Первый IPC poll, как и последующие, не блокирует GTK main thread.
  request_status_update();
  update_icon();
  update_auto_toggle_state();

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

  toggle_item_ =
      gtk_check_menu_item_new_with_label("Автокоррекция: состояние неизвестно");
  gtk_widget_set_sensitive(toggle_item_, FALSE);
  g_signal_connect(toggle_item_, "toggled", G_CALLBACK(on_auto_toggled), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle_item_);

  sound_settings_item_ = gtk_menu_item_new_with_label("Звук исправлений...");
  g_signal_connect(sound_settings_item_, "activate", G_CALLBACK(on_settings_clicked), this);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sound_settings_item_);

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
  if (indicator_ == nullptr) {
    return;
  }
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

  auto *item = GTK_CHECK_MENU_ITEM(toggle_item_);
  updating_toggle_ = true;
  const char *label = "Автокоррекция: состояние неизвестно";
  if (current_capability_ == MutationCapability::Disabled) {
    label = "Изменение текста отключено в сервисе";
  } else if (current_capability_ == MutationCapability::X11) {
    label = "Автокоррекция слов";
  }
  gtk_menu_item_set_label(GTK_MENU_ITEM(item), label);
  gtk_widget_set_sensitive(
      toggle_item_, current_capability_ == MutationCapability::X11 &&
                        current_status_ != ServiceStatus::Unknown &&
                        !status_poll_state_->in_flight.load(std::memory_order_acquire));
  gtk_widget_set_tooltip_text(
      toggle_item_, last_command_failed_
                        ? "Команда не подтверждена. Показано прочитанное состояние сервиса."
                        : "Только автоматические исправления в поддерживаемых X11-редакторах. Ручные команды независимы.");

  if (current_status_ == ServiceStatus::Unknown) {
    gtk_check_menu_item_set_inconsistent(item, TRUE);
    gtk_check_menu_item_set_active(item, FALSE);
  } else {
    gtk_check_menu_item_set_inconsistent(item, FALSE);
    gtk_check_menu_item_set_active(item,
                                   current_status_ == ServiceStatus::Enabled);
  }
  updating_toggle_ = false;
}

void TrayApp::on_auto_toggled(GtkCheckMenuItem *item, gpointer user_data) {
  auto *app = static_cast<TrayApp *>(user_data);
  if (app->updating_toggle_) {
    return;
  }
  if (app->current_capability_ != MutationCapability::X11 ||
      app->current_status_ == ServiceStatus::Unknown ||
      app->status_poll_state_->in_flight.load(std::memory_order_acquire)) {
    app->update_auto_toggle_state();
    return;
  }
  app->request_status_update(gtk_check_menu_item_get_active(item) != FALSE);
}

void TrayApp::on_settings_clicked(GtkMenuItem *item, gpointer user_data) {
  auto *app = static_cast<TrayApp *>(user_data);

  // Показываем диалог настроек
  const auto section = GTK_WIDGET(item) == app->sound_settings_item_
                           ? SettingsDialog::Section::Sound
                           : SettingsDialog::Section::General;
  bool saved = SettingsDialog::show(nullptr, section);

  if (saved) {
    // Автоматически применяем настройки после сохранения
    const std::string cfg_path = SettingsDialog::get_user_config_path();
    bool success = app->config_reloader_(cfg_path);
    if (success) {
      app->request_status_update();
    }
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
      "Version " PUNTO_VERSION "\n"
      "Исправление слов и выделенного текста в X11\n"
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
  app->request_status_update();
  return G_SOURCE_CONTINUE;
}

gboolean TrayApp::on_status_result(gpointer user_data) {
  auto *result = static_cast<StatusPollResult *>(user_data);
  const auto state = result->state;
  TrayApp *app = state->owner.load(std::memory_order_acquire);
  state->in_flight.store(false, std::memory_order_release);
  if (app != nullptr &&
      state->generation.load(std::memory_order_acquire) == result->generation) {
    app->current_status_ = result->snapshot.status;
    app->current_capability_ = result->snapshot.capability;
    if (result->command_failed) {
      app->last_command_failed_ = *result->command_failed;
    }
    app->update_icon();
    app->update_auto_toggle_state();
  }
  return G_SOURCE_REMOVE;
}

void TrayApp::request_status_update(std::optional<bool> requested_enabled) {
  bool expected = false;
  if (!status_poll_state_->in_flight.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }

  const auto state = status_poll_state_;
  update_auto_toggle_state();
  const std::uint64_t generation =
      state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
  try {
    auto provider = status_provider_;
    auto setter = status_setter_;
    std::thread{[state, provider = std::move(provider), setter = std::move(setter),
                 generation, requested_enabled] {
      IpcClientResult snapshot;
      std::optional<bool> command_failed;
      if (requested_enabled) {
        try {
          command_failed = !setter(*requested_enabled);
        } catch (...) {
          command_failed = true;
        }
      }
      try {
        snapshot = provider();
      } catch (...) {
        snapshot = {};
      }
      if (!snapshot.ok()) {
        snapshot.status = ServiceStatus::Unknown;
        snapshot.capability = MutationCapability::Unknown;
      }
      auto *result =
          new (std::nothrow) StatusPollResult{state, std::move(snapshot), command_failed, generation};
      if (result == nullptr) {
        state->in_flight.store(false, std::memory_order_release);
        return;
      }
      (void)g_idle_add_full(
          G_PRIORITY_DEFAULT, &TrayApp::on_status_result, result,
          [](gpointer data) { delete static_cast<StatusPollResult *>(data); });
    }}.detach();
  } catch (...) {
    state->in_flight.store(false, std::memory_order_release);
    update_auto_toggle_state();
  }
}

} // namespace punto
