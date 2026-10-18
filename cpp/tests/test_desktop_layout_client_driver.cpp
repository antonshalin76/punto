#include "punto/desktop_layout_client.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <systemd/sd-bus.h>
#include <thread>

namespace {
std::atomic<unsigned int> connect_calls{0};
std::atomic<bool> pump_complete{false};
std::atomic<unsigned int> pump_generation{0};

void mark_test_event(const char *path) {
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd >= 0)
    ::close(fd);
}
} // namespace

extern "C" void punto_test_pump_started() {
  pump_generation.fetch_add(1, std::memory_order_acq_rel);
  pump_complete.store(false, std::memory_order_release);
}

extern "C" void punto_test_pending_call_completed() {
  pump_complete.store(true, std::memory_order_release);
  if (pump_generation.load(std::memory_order_acquire) == 2) {
    if (const char *marker = std::getenv("PUNTO_TEST_CALLBACK_MARKER"))
      mark_test_event(marker);
  }
}

extern "C" int __real_sd_bus_process(sd_bus *, sd_bus_message **);
extern "C" int __wrap_sd_bus_process(sd_bus *bus, sd_bus_message **message) {
  const char *flood_marker = std::getenv("PUNTO_TEST_FLOOD_MARKER");
  const bool flood_switch =
      flood_marker != nullptr &&
      pump_generation.load(std::memory_order_acquire) == 2 &&
      std::filesystem::exists(flood_marker);
  if (!flood_switch)
    return __real_sd_bus_process(bus, message);
  if (pump_complete.load(std::memory_order_acquire)) {
    if (const char *marker = std::getenv("PUNTO_TEST_POISON_MARKER"))
      mark_test_event(marker);
    return 1;
  }
  const int result = __real_sd_bus_process(bus, message);
  return result == 0 ? 1 : result;
}

extern "C" int __real_connect(int, const sockaddr *, socklen_t);
extern "C" int __wrap_connect(int fd, const sockaddr *address,
                              socklen_t length) {
  connect_calls.fetch_add(1, std::memory_order_relaxed);
  if (std::getenv("PUNTO_TEST_STALLED_CONNECT") != nullptr) {
    errno = EINPROGRESS;
    return -1;
  }
  return __real_connect(fd, address, length);
}

extern "C" int __real_poll(pollfd *, nfds_t, int);
extern "C" int __wrap_poll(pollfd *fds, nfds_t count, int timeout) {
  if (std::getenv("PUNTO_TEST_STALLED_CONNECT") != nullptr) {
    if (timeout != 0)
      std::abort();
    return 0;
  }
  return __real_poll(fds, count, timeout);
}

int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: CLIENT RUNTIME_DIR UID SOURCE TARGET [MODE]\n";
    return 64;
  }
  punto::X11SessionInfo session;
  session.xdg_runtime_dir = argv[1];
  try {
    session.uid = static_cast<std::uint32_t>(std::stoul(argv[2]));
    const int source = std::stoi(argv[3]);
    const int target = std::stoi(argv[4]);
    const std::string mode = argc == 6 ? argv[5] : "success";
    const char *cancel_marker = std::getenv("PUNTO_TEST_CANCEL_MARKER");
    punto::DesktopLayoutClient client;
    if (mode == "fifo-recovery") {
      unsigned int wait_calls = 0;
      const auto rejected_at = std::chrono::steady_clock::now();
      const auto rejected = client.switch_to(
          session, source, target, rejected_at + std::chrono::milliseconds{500},
          [&wait_calls](auto deadline) {
            ++wait_calls;
            if (deadline > std::chrono::steady_clock::now())
              std::this_thread::sleep_until(deadline);
            return true;
          });
      if (rejected.status != punto::DesktopLayoutStatus::Rejected ||
          std::chrono::steady_clock::now() - rejected_at >
              std::chrono::milliseconds{250} ||
          connect_calls.load(std::memory_order_relaxed) != 0 ||
          wait_calls != 0) {
        std::cerr << "FIFO descriptor crossed the validation boundary\n";
        return 4;
      }
      std::cout << "FIFO_REJECTED" << std::endl;
      const auto restore_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{2};
      const auto descriptor =
          std::filesystem::path{argv[1]} / "punto" / "session-bridge.json";
      while (std::chrono::steady_clock::now() < restore_deadline) {
        std::error_code error;
        if (std::filesystem::is_regular_file(descriptor, error))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      std::error_code error;
      if (!std::filesystem::is_regular_file(descriptor, error))
        return 5;
      const auto recovered = client.switch_to(
          session, source, target,
          std::chrono::steady_clock::now() + std::chrono::milliseconds{900},
          [&wait_calls](auto deadline) {
            ++wait_calls;
            if (deadline > std::chrono::steady_clock::now())
              std::this_thread::sleep_until(deadline);
            return true;
          });
      if (recovered.status != punto::DesktopLayoutStatus::Verified ||
          recovered.observed_layout != target)
        return 6;
      std::cout << recovered.stage << '\n';
      return 0;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto result = client.switch_to(
        session, source, target,
        std::chrono::steady_clock::now() + std::chrono::milliseconds{3400},
        [cancel_marker, &mode](auto deadline) {
          if (mode == "connect-cancel")
            return false;
          if (cancel_marker != nullptr &&
              std::filesystem::exists(cancel_marker))
            return false;
          if (deadline > std::chrono::steady_clock::now())
            std::this_thread::sleep_until(deadline);
          return true;
        });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto expected_status =
        mode == "cancel" ? punto::DesktopLayoutStatus::RequestedUnresolved
        : mode == "reject" || mode == "connect-cancel"
            ? punto::DesktopLayoutStatus::Rejected
        : source == target ? punto::DesktopLayoutStatus::Verified
                           : punto::DesktopLayoutStatus::Applied;
    if (result.status != expected_status ||
        (expected_status != punto::DesktopLayoutStatus::Rejected &&
         expected_status != punto::DesktopLayoutStatus::RequestedUnresolved &&
         result.observed_layout != target)) {
      std::cerr << "desktop layout client rejected at " << result.stage << '\n';
      return 2;
    }
    if ((mode == "cancel" || mode == "connect-cancel") &&
        elapsed > std::chrono::milliseconds{250}) {
      std::cerr << "desktop layout cancellation exceeded bound\n";
      return 3;
    }
    std::cout << result.stage << '\n';
  } catch (...) {
    return 65;
  }
  return 0;
}
