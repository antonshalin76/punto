#include "punto/tray_app.hpp"
#include "punto/config.hpp"
#include "punto/settings_dialog.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace punto {

struct TrayAppTestAccess {
  static void set_status_provider(TrayApp &app,
                                  std::function<ServiceStatus()> provider) {
    app.status_provider_ = [provider = std::move(provider)] {
      const auto status = provider();
      return IpcClientResult{IpcClientError::None, status, {},
                             status == ServiceStatus::Unknown
                                 ? MutationCapability::Unknown
                                 : MutationCapability::X11};
    };
  }

  static void set_snapshot_provider(TrayApp &app,
                                   std::function<IpcClientResult()> provider) {
    app.status_provider_ = std::move(provider);
  }

  static void set_status_setter(TrayApp &app, std::function<bool(bool)> setter) {
    app.status_setter_ = std::move(setter);
  }

  static void request_status_update(TrayApp &app) {
    app.request_status_update();
  }

  static ServiceStatus current_status(const TrayApp &app) {
    return app.current_status_;
  }

  static GtkWidget *create_menu(TrayApp &app) {
    app.menu_ = app.create_menu();
    return app.toggle_item_;
  }

  static GtkWidget *sound_item(TrayApp &app) {
    return app.sound_settings_item_;
  }

  static void set_config_reloader(TrayApp &app,
                                  std::function<bool(const std::string &)> reload) {
    app.config_reloader_ = std::move(reload);
  }

  static bool command_failed(TrayApp &app) {
    return app.last_command_failed_;
  }

  static void open_about(TrayApp &app) {
    app.on_about_clicked(nullptr, &app);
  }
};

} // namespace punto

namespace {

using namespace std::chrono_literals;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

void pump_for(std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
    }
    std::this_thread::sleep_for(1ms);
  }
}

GtkWidget *find_sound_checkbox(GtkWidget *widget) {
  if (GTK_IS_CHECK_BUTTON(widget) &&
      std::string_view{gtk_button_get_label(GTK_BUTTON(widget))} ==
          "Звук при смене раскладки") return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *found = nullptr;
  for (GList *child = children; child && !found; child = child->next) {
    found = find_sound_checkbox(GTK_WIDGET(child->data));
  }
  g_list_free(children);
  return found;
}

struct PrivateSoundConfig {
  std::filesystem::path root;
  std::filesystem::path path;
  std::optional<std::string> previous_home;
  PrivateSoundConfig() {
    gchar *directory = g_dir_make_tmp("punto-tray-sound-XXXXXX", nullptr);
    expect(directory != nullptr, "private sound config directory created");
    root = directory;
    g_free(directory);
    if (const char *value = std::getenv("HOME")) previous_home = value;
    expect(::setenv("HOME", root.c_str(), 1) == 0, "private config home installed");
    path = punto::SettingsDialog::get_user_config_path();
    std::filesystem::create_directories(path.parent_path());
  }
  ~PrivateSoundConfig() {
    if (previous_home) (void)::setenv("HOME", previous_home->c_str(), 1);
    else (void)::unsetenv("HOME");
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};

struct SoundDialogAction {
  bool toggle = false;
  bool accept = false;
  bool found = false;
  bool initial_enabled = false;
  bool save_enabled = false;
  bool error_seen = false;
  std::filesystem::path fail_path;
  std::string error;
};

void exercise_sound_menu(punto::TrayApp &app, SoundDialogAction &action) {
  GtkWidget *item = punto::TrayAppTestAccess::sound_item(app);
  expect(gtk_widget_get_sensitive(item), "sound preferences are available from tray");
  const guint error_closer = g_timeout_add(5, [](gpointer data) -> gboolean {
    auto &state = *static_cast<SoundDialogAction *>(data);
    GList *windows = gtk_window_list_toplevels();
    for (GList *window = windows; window; window = window->next) {
      if (GTK_IS_MESSAGE_DIALOG(window->data)) {
        GtkMessageType type = GTK_MESSAGE_INFO;
        gchar *text = nullptr;
        g_object_get(window->data, "message-type", &type, "text", &text, nullptr);
        state.error_seen = type == GTK_MESSAGE_ERROR && text &&
            std::string_view{text}.find("Не удалось подтвердить сохранение настроек") !=
                std::string_view::npos;
        g_free(text);
        gtk_dialog_response(GTK_DIALOG(window->data), GTK_RESPONSE_CLOSE);
      }
    }
    g_list_free(windows);
    return G_SOURCE_CONTINUE;
  }, &action);
  const guint driver = g_idle_add([](gpointer data) -> gboolean {
    auto &state = *static_cast<SoundDialogAction *>(data);
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *dialog = nullptr;
    for (GList *window = windows; window; window = window->next) {
      const char *title = gtk_window_get_title(GTK_WINDOW(window->data));
      if (title && std::string_view{title} == "Настройки Punto Switcher") {
        dialog = GTK_WIDGET(window->data);
        break;
      }
    }
    g_list_free(windows);
    if (!dialog) return G_SOURCE_CONTINUE;
    try {
      GtkWidget *sound = find_sound_checkbox(dialog);
      state.found = sound != nullptr;
      if (sound) {
        state.initial_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sound));
        if (state.toggle) {
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sound), !state.initial_enabled);
        }
        state.save_enabled = gtk_widget_get_sensitive(
            gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT));
        if (!state.fail_path.empty()) {
          std::filesystem::rename(state.fail_path, state.fail_path.string() + ".saved");
          std::filesystem::create_directory(state.fail_path);
        }
      }
      gtk_dialog_response(GTK_DIALOG(dialog), sound && state.accept
          ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_CANCEL);
    } catch (const std::exception &error) {
      state.error = error.what();
      gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    }
    return G_SOURCE_REMOVE;
  }, &action);
  gtk_menu_item_activate(GTK_MENU_ITEM(item));
  if (g_main_context_find_source_by_id(nullptr, driver)) g_source_remove(driver);
  g_source_remove(error_closer);
  expect(action.error.empty(), action.error);
  expect(action.found, "sound action opens real settings with sound checkbox");
}

void test_sound_menu_uses_settings_persistence_and_failure_readback() {
  PrivateSoundConfig fixture;
  punto::Config expected;
  expected.auto_switch.enabled = false;
  expected.auto_switch.threshold = 4.2;
  expected.runtime.analysis_threads = 2;
  expected.runtime.max_analysis_threads_per_daemon = 3;
  expected.logging.level = punto::LogLevel::Debug;
  expected.hotkey.modifier = KEY_RIGHTCTRL;
  const auto original = punto::serialize_config(expected);
  expect(original.has_value(), "sound fixture config serializes");
  { std::ofstream output{fixture.path}; output << *original; }
  punto::TrayApp app;
  punto::TrayAppTestAccess::create_menu(app);
  unsigned int reloads = 0;
  std::string reload_path;
  punto::TrayAppTestAccess::set_config_reloader(app, [&](const std::string &path) {
    reload_path = path;
    ++reloads;
    return false; // No host IPC: model an unavailable service after a saved preference.
  });
  SoundDialogAction save;
  save.toggle = save.accept = true;
  exercise_sound_menu(app, save);
  expect(save.initial_enabled && save.save_enabled && !save.error_seen,
         "sound edit enables Save without an error");
  expected.sound.enabled = false;
  auto loaded = punto::load_config_checked(fixture.path);
  expect(loaded.result == punto::ConfigResult::Ok &&
             punto::serialize_config(loaded.config) == punto::serialize_config(expected),
         "sound save preserves every other serialized config field");
  expect(reloads == 1 && reload_path == fixture.path,
         "successful save requests exactly one reload of the private config");
  SoundDialogAction cancel;
  cancel.toggle = true;
  exercise_sound_menu(app, cancel);
  expect(!cancel.initial_enabled && reloads == 1 && !cancel.error_seen,
         "Cancel has no reload, error, or optimistic state");
  SoundDialogAction failure;
  failure.toggle = failure.accept = true;
  failure.fail_path = fixture.path;
  exercise_sound_menu(app, failure);
  expect(failure.error_seen && reloads == 1, "failed save reports error without reload");
  std::filesystem::remove(fixture.path);
  std::filesystem::rename(fixture.path.string() + ".saved", fixture.path);
  SoundDialogAction reopen;
  exercise_sound_menu(app, reopen);
  expect(!reopen.initial_enabled && !reopen.save_enabled && reloads == 1 && !reopen.error_seen,
         "reopening reads persisted sound after failed save");
  loaded = punto::load_config_checked(fixture.path);
  expect(punto::serialize_config(loaded.config) == punto::serialize_config(expected),
         "Cancel and failure preserve persisted config fields");
}

void test_status_poll_is_single_flight_and_does_not_block_main_context() {
  std::atomic<bool> release{false};
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::atomic<unsigned int> calls{0};
  unsigned int heartbeats = 0;

  auto app = std::make_unique<punto::TrayApp>();
  punto::TrayAppTestAccess::set_status_provider(*app, [&] {
    calls.fetch_add(1, std::memory_order_relaxed);
    started.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(1ms);
    }
    finished.store(true, std::memory_order_release);
    return punto::ServiceStatus::Unknown;
  });

  punto::TrayAppTestAccess::request_status_update(*app);
  const auto start_deadline = std::chrono::steady_clock::now() + 500ms;
  while (!started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < start_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  expect(started.load(std::memory_order_acquire), "status worker starts");

  const guint heartbeat_id = g_timeout_add(
      2,
      [](gpointer data) -> gboolean {
        ++*static_cast<unsigned int *>(data);
        return G_SOURCE_CONTINUE;
      },
      &heartbeats);
  for (int attempt = 0; attempt < 20; ++attempt) {
    punto::TrayAppTestAccess::request_status_update(*app);
  }
  pump_for(30ms);
  expect(heartbeats >= 5, "GTK/GLib context stays live behind stalled IPC");
  expect(calls.load(std::memory_order_relaxed) == 1,
         "periodic status polls coalesce to one in-flight request");
  (void)g_source_remove(heartbeat_id);

  const auto destroy_started = std::chrono::steady_clock::now();
  app.reset();
  expect(std::chrono::steady_clock::now() - destroy_started < 50ms,
         "tray destruction never waits for the IPC peer");

  release.store(true, std::memory_order_release);
  const auto finish_deadline = std::chrono::steady_clock::now() + 500ms;
  while (!finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < finish_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  expect(finished.load(std::memory_order_acquire), "detached poll completes");
  pump_for(10ms);
}

void test_completed_status_is_applied_on_main_context() {
  punto::TrayApp app;
  punto::TrayAppTestAccess::set_status_provider(
      app, [] { return punto::ServiceStatus::Disabled; });
  punto::TrayAppTestAccess::request_status_update(app);

  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (punto::TrayAppTestAccess::current_status(app) !=
             punto::ServiceStatus::Disabled &&
         std::chrono::steady_clock::now() < deadline) {
    pump_for(2ms);
  }
  expect(punto::TrayAppTestAccess::current_status(app) ==
             punto::ServiceStatus::Disabled,
         "latest status is applied on the main context");
}

void wait_for(std::function<bool()> predicate, std::string_view message) {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    pump_for(2ms);
  }
  expect(predicate(), message);
}

void test_capability_is_projected_separately_from_auto_status() {
  using Capability = punto::MutationCapability;
  using Status = punto::ServiceStatus;
  struct Case { Capability capability; Status status; const char *label; bool sensitive; };
  for (const auto &test : {
           Case{Capability::Disabled, Status::Disabled,
                "Изменение текста отключено в сервисе", false},
           Case{Capability::X11, Status::Disabled,
                "Автокоррекция слов", true},
           Case{Capability::X11, Status::Enabled,
                "Автокоррекция слов", true},
           Case{Capability::Unknown, Status::Unknown,
                "Автокоррекция: состояние неизвестно", false}}) {
    punto::TrayApp app;
    GtkWidget *toggle = punto::TrayAppTestAccess::create_menu(app);
    std::atomic<unsigned int> calls{0};
    punto::TrayAppTestAccess::set_status_setter(app, [&](bool) { ++calls; return true; });
    punto::TrayAppTestAccess::set_snapshot_provider(app, [test] {
      return punto::IpcClientResult{punto::IpcClientError::None, test.status, {}, test.capability};
    });
    punto::TrayAppTestAccess::request_status_update(app);
    wait_for([&] {
      return std::string_view{gtk_menu_item_get_label(GTK_MENU_ITEM(toggle))} == test.label &&
             (gtk_widget_get_sensitive(toggle) != FALSE) == test.sensitive &&
             punto::TrayAppTestAccess::current_status(app) == test.status;
    }, "snapshot projects exact capability label and sensitivity");
    expect((gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(toggle)) != FALSE) ==
               (test.status == Status::Enabled), "checkbox follows runtime automation");
    expect(gtk_widget_get_sensitive(punto::TrayAppTestAccess::sound_item(app)) != FALSE,
           "sound preference editor stays available independently of runtime capability");
    if (!test.sensitive) {
      gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(toggle), TRUE);
      pump_for(5ms);
      expect(calls.load() == 0, "legacy and unknown capability cannot submit SET");
    }
  }
}

void test_toggle_uses_readback_even_after_rejected_command() {
  for (int scenario = 0; scenario < 4; ++scenario) {
    auto enabled = std::make_shared<std::atomic<bool>>(false);
    auto setter_calls = std::make_shared<std::atomic<unsigned int>>(0);
    auto last_requested = std::make_shared<std::atomic<bool>>(false);
    punto::TrayApp app;
    GtkWidget *toggle = punto::TrayAppTestAccess::create_menu(app);
    punto::TrayAppTestAccess::set_snapshot_provider(app, [=] {
      if (scenario == 3 && setter_calls->load() != 0) {
        return punto::IpcClientResult{punto::IpcClientError::TimedOut,
                                     punto::ServiceStatus::Unknown, {},
                                     punto::MutationCapability::Unknown};
      }
      return punto::IpcClientResult{
          punto::IpcClientError::None,
          enabled->load() ? punto::ServiceStatus::Enabled : punto::ServiceStatus::Disabled,
          {}, punto::MutationCapability::X11};
    });
    punto::TrayAppTestAccess::set_status_setter(app, [=](bool requested) {
      ++*setter_calls;
      last_requested->store(requested);
      std::this_thread::sleep_for(30ms);
      if (scenario != 1) {
        enabled->store(requested);
      }
      return scenario == 0 || scenario == 3;
    });
    punto::TrayAppTestAccess::request_status_update(app);
    wait_for([&] { return gtk_widget_get_sensitive(toggle) != FALSE; }, "initial modern snapshot");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(toggle), TRUE);
    expect(gtk_widget_get_sensitive(toggle) == FALSE, "toggle is disabled while command/readback is pending");
    expect(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(toggle)) == FALSE,
           "toggle does not present optimistic success");
    wait_for([&] {
      return scenario == 3
                 ? punto::TrayAppTestAccess::current_status(app) == punto::ServiceStatus::Unknown
                 : gtk_widget_get_sensitive(toggle) != FALSE;
    }, "command result is reconciled through authoritative snapshot");
    expect(setter_calls->load() == 1 && last_requested->load(), "one enable command was submitted");
    const bool expected_checked = scenario == 0 || scenario == 2;
    expect((gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(toggle)) != FALSE) == expected_checked,
           "checkbox shows readback including nondurable ERROR with visible commit");
    expect(punto::TrayAppTestAccess::command_failed(app) == (scenario == 1 || scenario == 2),
           "failed command feedback survives successful readback");
    if (scenario == 1 || scenario == 2) {
      char *tooltip = gtk_widget_get_tooltip_text(toggle);
      const bool feedback = tooltip != nullptr &&
                            std::string_view{tooltip}.find("Команда не подтверждена") != std::string_view::npos;
      g_free(tooltip);
      expect(feedback, "command error remains visible in tooltip");
    }
  }
}

void test_about_describes_limited_word_capability() {
  punto::TrayApp app;
  punto::TrayAppTestAccess::open_about(app);
  GList *windows = gtk_window_list_toplevels();
  bool correct = false;
  for (GList *window = windows; window != nullptr; window = window->next) {
    const char *title = gtk_window_get_title(GTK_WINDOW(window->data));
    if (title == nullptr || std::string_view{title} != "О программе") {
      continue;
    }
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(window->data));
    GList *children = gtk_container_get_children(GTK_CONTAINER(content));
    for (GList *child = children; child != nullptr; child = child->next) {
      if (GTK_IS_LABEL(child->data)) {
        const std::string_view text{gtk_label_get_text(GTK_LABEL(child->data))};
        correct = text.find("Исправление слов и выделенного текста в X11") != std::string_view::npos &&
                  text.find("изменение текста отключено") == std::string_view::npos;
      }
    }
    g_list_free(children);
    gtk_dialog_response(GTK_DIALOG(window->data), GTK_RESPONSE_CLOSE);
  }
  g_list_free(windows);
  expect(correct, "About describes restored X11 word and selection corrections");
}

[[gnu::noinline]] void inject_owned_leak_for_sanitizer_negative_control() {
  // Explicit opt-in proves the external Fontconfig exception cannot hide our leaks.
  auto *leaked = new volatile unsigned char[73];
  leaked[0] = 1;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const bool negative_control = argc == 2 &&
                                  std::string_view{argv[1]} == "--lsan-negative-control";
    expect(argc == 1 || negative_control, "unknown tray contract argument");
    expect(gtk_init_check(nullptr, nullptr) != FALSE,
           "tray widget contract requires an isolated X11 display");
    test_sound_menu_uses_settings_persistence_and_failure_readback();
    test_status_poll_is_single_flight_and_does_not_block_main_context();
    test_completed_status_is_applied_on_main_context();
    test_capability_is_projected_separately_from_auto_status();
    test_toggle_uses_readback_even_after_rejected_command();
    test_about_describes_limited_word_capability();
    pump_for(10ms);
    pango_cairo_font_map_set_default(nullptr);
    if (negative_control) {
      std::thread{inject_owned_leak_for_sanitizer_negative_control}.join();
    }
    std::cout << "test_tray_app_contract: OK\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_tray_app_contract: FAIL: " << error.what() << '\n';
    return 1;
  }
}
