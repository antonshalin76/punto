#include "punto/tray_app.hpp"

#include <atomic>
#include <chrono>
#include <functional>
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
    app.status_provider_ = std::move(provider);
  }

  static void request_status_update(TrayApp &app) {
    app.request_status_update();
  }

  static ServiceStatus current_status(const TrayApp &app) {
    return app.current_status_;
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

} // namespace

int main() {
  try {
    test_status_poll_is_single_flight_and_does_not_block_main_context();
    test_completed_status_is_applied_on_main_context();
    std::cout << "test_tray_app_contract: OK\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_tray_app_contract: FAIL: " << error.what() << '\n';
    return 1;
  }
}
