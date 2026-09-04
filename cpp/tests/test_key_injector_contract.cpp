#include "punto/key_injector.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <thread>

namespace {

using namespace std::chrono_literals;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void expect(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

void close_fd(int &fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

void fill_pipe(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  expect(flags >= 0, "get pipe flags");
  expect(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
         "make fill pipe nonblocking");
  std::array<char, 4096> bytes{};
  while (true) {
    const ssize_t count = ::write(fd, bytes.data(), bytes.size());
    if (count > 0) {
      continue;
    }
    expect(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
           "fill pipe reached capacity");
    break;
  }
  expect(::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0,
         "restore blocking output pipe");
}

struct ChildResult {
  std::int64_t elapsed_ms;
  int error;
  bool fatal;
};

struct InjectionCapture {
  punto::InjectionResult result;
  std::vector<input_event> events;
};

InjectionCapture capture_injection(
    std::size_t cancel_wait,
    const std::function<punto::InjectionResult(punto::KeyInjector &)> &run) {
  int output_pipe[2]{-1, -1};
  expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create injection pipe");
  const int saved_stdout = ::dup(STDOUT_FILENO);
  expect(saved_stdout >= 0, "save injection stdout");
  expect(::dup2(output_pipe[1], STDOUT_FILENO) >= 0,
         "redirect injection stdout");
  close_fd(output_pipe[1]);

  punto::KeyInjector injector;
  std::size_t waits = 0;
  injector.set_wait_func([&](std::chrono::microseconds) {
    ++waits;
    return cancel_wait == 0 || waits != cancel_wait;
  });
  const punto::InjectionResult result = run(injector);

  expect(::dup2(saved_stdout, STDOUT_FILENO) >= 0, "restore injection stdout");
  (void)::close(saved_stdout);

  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, 1024> chunk{};
  while (true) {
    const ssize_t count = ::read(output_pipe[0], chunk.data(), chunk.size());
    if (count == 0) {
      break;
    }
    expect(count > 0, "read captured injection");
    bytes.insert(bytes.end(), chunk.begin(),
                 chunk.begin() + static_cast<std::size_t>(count));
  }
  close_fd(output_pipe[0]);
  expect(bytes.size() % sizeof(input_event) == 0,
         "captured injection contains whole events");
  std::vector<input_event> events(bytes.size() / sizeof(input_event));
  if (!bytes.empty()) {
    std::memcpy(events.data(), bytes.data(), bytes.size());
  }
  return {result, std::move(events)};
}

void expect_all_keys_released(const std::vector<input_event> &events,
                              std::string_view context) {
  std::unordered_map<std::uint16_t, bool> pressed;
  for (const auto &event : events) {
    if (event.type != EV_KEY) {
      continue;
    }
    if (event.value == static_cast<std::int32_t>(punto::KeyState::Press)) {
      pressed[event.code] = true;
    } else if (event.value ==
               static_cast<std::int32_t>(punto::KeyState::Release)) {
      pressed[event.code] = false;
    }
  }
  for (const auto &[code, remains_pressed] : pressed) {
    (void)code;
    expect(!remains_pressed, context);
  }
}

void test_cancellation_aware_injection_releases_every_key() {
  const auto clipboard_paste = [](punto::KeyInjector &injector) {
    return injector.send_clipboard_paste();
  };
  const auto clipboard_copy = [](punto::KeyInjector &injector) {
    return injector.send_clipboard_copy();
  };
  const auto terminal_paste = [](punto::KeyInjector &injector) {
    return injector.send_paste(true);
  };
  const auto legacy_gui_paste = [](punto::KeyInjector &injector) {
    return injector.send_paste(false);
  };
  const auto layout = [](punto::KeyInjector &injector) {
    return injector.send_layout_hotkey(KEY_LEFTCTRL, KEY_SPACE);
  };

  const std::array<std::function<punto::InjectionResult(punto::KeyInjector &)>,
                   5>
      operations{clipboard_paste, clipboard_copy, terminal_paste,
                 legacy_gui_paste, layout};
  for (const auto &operation : operations) {
    const auto complete = capture_injection(0, operation);
    expect(complete.result == punto::InjectionResult::Completed,
           "uncancelled injection completes");
    expect_all_keys_released(complete.events,
                             "completed injection releases every key");
    for (std::size_t cancel_wait = 1; cancel_wait <= 5; ++cancel_wait) {
      const auto cancelled = capture_injection(cancel_wait, operation);
      expect(cancelled.result != punto::InjectionResult::Completed,
             "injection reports cancellation at every delay");
      expect_all_keys_released(cancelled.events,
                               "cancelled injection releases every key");
    }
  }

  for (std::size_t cancel_wait = 1; cancel_wait <= 5; ++cancel_wait) {
    int output_pipe[2]{-1, -1};
    expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create tap pipe");
    const int saved_stdout = ::dup(STDOUT_FILENO);
    expect(saved_stdout >= 0 && ::dup2(output_pipe[1], STDOUT_FILENO) >= 0,
           "redirect tap stdout");
    close_fd(output_pipe[1]);
    punto::KeyInjector injector;
    std::size_t waits = 0;
    injector.set_wait_func([&](std::chrono::microseconds) {
      ++waits;
      return waits != cancel_wait;
    });
    expect(injector.tap_key(KEY_LEFT, true, true) !=
               punto::InjectionResult::Completed,
           "tap reports cancellation at every delay");
    expect(::dup2(saved_stdout, STDOUT_FILENO) >= 0, "restore tap stdout");
    (void)::close(saved_stdout);
    std::vector<input_event> events;
    input_event event{};
    while (::read(output_pipe[0], &event, sizeof(event)) == sizeof(event)) {
      events.push_back(event);
    }
    close_fd(output_pipe[0]);
    expect_all_keys_released(events, "cancelled tap releases every key");
  }
}

void test_writable_stdout_preserves_exact_event_bytes() {
  int output_pipe[2]{-1, -1};
  expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create writable output pipe");
  const int saved_stdout = ::dup(STDOUT_FILENO);
  expect(saved_stdout >= 0, "save writable stdout");
  expect(::dup2(output_pipe[1], STDOUT_FILENO) >= 0,
         "redirect writable stdout");
  close_fd(output_pipe[1]);

  std::array<input_event, 2> events{};
  events[0].type = EV_KEY;
  events[0].code = KEY_A;
  events[0].value = 1;
  events[1].type = EV_SYN;
  events[1].code = SYN_REPORT;

  punto::KeyInjector injector;
  injector.emit_events(events);
  expect(::dup2(saved_stdout, STDOUT_FILENO) >= 0, "restore writable stdout");
  (void)::close(saved_stdout);
  expect(!injector.has_fatal_io_error(), "writable stdout remains healthy");

  std::array<input_event, 2> observed{};
  std::size_t offset = 0;
  auto *destination = reinterpret_cast<std::uint8_t *>(observed.data());
  while (offset < sizeof(observed)) {
    const ssize_t count =
        ::read(output_pipe[0], destination + offset, sizeof(observed) - offset);
    expect(count > 0, "read exact emitted events");
    offset += static_cast<std::size_t>(count);
  }
  close_fd(output_pipe[0]);
  expect(std::memcmp(events.data(), observed.data(), sizeof(events)) == 0,
         "event bytes are unchanged");
}

void test_full_stdout_pipe_is_bounded() {
  int output_pipe[2]{-1, -1};
  int result_pipe[2]{-1, -1};
  expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create output pipe");
  expect(::pipe2(result_pipe, O_CLOEXEC) == 0, "create result pipe");
  (void)::fcntl(output_pipe[1], F_SETPIPE_SZ, 4096);
  fill_pipe(output_pipe[1]);

  const pid_t child = ::fork();
  expect(child >= 0, "fork timeout probe");
  if (child == 0) {
    close_fd(output_pipe[0]);
    close_fd(result_pipe[0]);
    if (::dup2(output_pipe[1], STDOUT_FILENO) < 0) {
      _exit(91);
    }
    close_fd(output_pipe[1]);
    (void)::signal(SIGPIPE, SIG_IGN);

    punto::KeyInjector injector;
    input_event event{};
    event.type = EV_SYN;
    event.code = SYN_REPORT;
    const auto started = std::chrono::steady_clock::now();
    injector.emit_event(event);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const ChildResult result{elapsed.count(), injector.fatal_io_errno(),
                             injector.has_fatal_io_error()};
    const ssize_t written = ::write(result_pipe[1], &result, sizeof(result));
    close_fd(result_pipe[1]);
    _exit(written == static_cast<ssize_t>(sizeof(result)) ? 0 : 92);
  }

  close_fd(output_pipe[1]);
  close_fd(result_pipe[1]);
  pollfd descriptor{result_pipe[0], POLLIN, 0};
  const int ready = ::poll(&descriptor, 1, 2750);
  if (ready != 1 || !(descriptor.revents & POLLIN)) {
    (void)::kill(child, SIGKILL);
    (void)::waitpid(child, nullptr, 0);
    close_fd(output_pipe[0]);
    close_fd(result_pipe[0]);
    fail("full stdout pipe blocked beyond the runtime deadline");
  }

  ChildResult result{};
  const ssize_t count = ::read(result_pipe[0], &result, sizeof(result));
  int status = 0;
  expect(::waitpid(child, &status, 0) == child, "wait timeout probe");
  close_fd(output_pipe[0]);
  close_fd(result_pipe[0]);

  expect(count == static_cast<ssize_t>(sizeof(result)), "read timeout result");
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "timeout probe exited cleanly");
  expect(result.fatal, "stdout timeout latches fatal I/O");
  expect(result.error == ETIMEDOUT, "stdout timeout preserves ETIMEDOUT");
  expect(result.elapsed_ms >= 1750 && result.elapsed_ms < 2600,
         "stdout timeout uses the documented bounded window");
}

void test_closed_stdout_is_immediate_epipe() {
  int output_pipe[2]{-1, -1};
  expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create EPIPE output pipe");
  close_fd(output_pipe[0]);

  const int saved_stdout = ::dup(STDOUT_FILENO);
  expect(saved_stdout >= 0, "save stdout");
  expect(::dup2(output_pipe[1], STDOUT_FILENO) >= 0, "redirect stdout");
  close_fd(output_pipe[1]);
  (void)::signal(SIGPIPE, SIG_IGN);

  punto::KeyInjector injector;
  input_event event{};
  event.type = EV_SYN;
  event.code = SYN_REPORT;
  const auto started = std::chrono::steady_clock::now();
  injector.emit_event(event);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  expect(::dup2(saved_stdout, STDOUT_FILENO) >= 0, "restore stdout");
  (void)::close(saved_stdout);
  expect(injector.has_fatal_io_error(), "EPIPE latches fatal I/O");
  expect(injector.fatal_io_errno() == EPIPE, "EPIPE errno is retained");
  expect(elapsed < 250ms, "EPIPE failure is immediate");
}

void test_action_result_rejects_output_failure_before_target_press() {
  const std::array<std::function<punto::InjectionResult(punto::KeyInjector &)>,
                   6>
      operations{
          [](punto::KeyInjector &injector) {
            return injector.send_clipboard_paste();
          },
          [](punto::KeyInjector &injector) {
            return injector.send_clipboard_copy();
          },
          [](punto::KeyInjector &injector) {
            return injector.send_paste(true);
          },
          [](punto::KeyInjector &injector) {
            return injector.send_paste(false);
          },
          [](punto::KeyInjector &injector) {
            return injector.send_layout_hotkey(KEY_LEFTCTRL, KEY_SPACE);
          },
          [](punto::KeyInjector &injector) {
            return injector.tap_key(KEY_LEFT, true, true);
          },
      };

  for (const auto &operation : operations) {
    int output_pipe[2]{-1, -1};
    expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create failed-action pipe");
    close_fd(output_pipe[0]);

    const int saved_stdout = ::dup(STDOUT_FILENO);
    expect(saved_stdout >= 0 && ::dup2(output_pipe[1], STDOUT_FILENO) >= 0,
           "redirect failed-action stdout");
    close_fd(output_pipe[1]);
    (void)::signal(SIGPIPE, SIG_IGN);

    punto::KeyInjector injector;
    injector.set_wait_func([](std::chrono::microseconds) { return true; });
    const punto::InjectionResult result = operation(injector);

    expect(::dup2(saved_stdout, STDOUT_FILENO) >= 0,
           "restore failed-action stdout");
    (void)::close(saved_stdout);
    expect(result == punto::InjectionResult::OutputFailedBeforeAction,
           "EPIPE before target press is reported before action");
    expect(!punto::injection_action_dispatched(result),
           "EPIPE before target press is never classified as dispatched");
    expect(injector.has_fatal_io_error(),
           "failed high-level action retains fatal output state");
  }
}

void test_action_result_reports_output_failure_after_target_press() {
  int output_pipe[2]{-1, -1};
  expect(::pipe2(output_pipe, O_CLOEXEC) == 0, "create partial-action pipe");
  const int saved_stdout = ::dup(STDOUT_FILENO);
  expect(saved_stdout >= 0 && ::dup2(output_pipe[1], STDOUT_FILENO) >= 0,
         "redirect partial-action stdout");
  close_fd(output_pipe[1]);
  (void)::signal(SIGPIPE, SIG_IGN);

  std::atomic<bool> target_press_seen{false};
  std::thread reader{[&] {
    input_event event{};
    while (::read(output_pipe[0], &event, sizeof(event)) == sizeof(event)) {
      if (event.type == EV_KEY && event.code == KEY_V && event.value == 1) {
        close_fd(output_pipe[0]);
        // Publish readiness only after the last reader is closed. Publishing
        // first lets the writer race one more successful event into the pipe,
        // making the intended post-action EPIPE probe flaky.
        target_press_seen.store(true, std::memory_order_release);
        return;
      }
    }
    close_fd(output_pipe[0]);
  }};

  punto::KeyInjector injector;
  std::size_t waits = 0;
  injector.set_wait_func([&](std::chrono::microseconds) {
    ++waits;
    if (waits == 3) {
      const auto deadline = std::chrono::steady_clock::now() + 500ms;
      while (!target_press_seen.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
      }
    }
    return true;
  });
  const punto::InjectionResult result = injector.send_clipboard_paste();

  expect(::dup2(saved_stdout, STDOUT_FILENO) >= 0,
         "restore partial-action stdout");
  (void)::close(saved_stdout);
  reader.join();
  expect(target_press_seen.load(std::memory_order_acquire),
         "partial-output fixture observed target press");
  expect(result == punto::InjectionResult::OutputFailedAfterAction,
         "EPIPE after target press is reported after action");
  expect(punto::injection_action_dispatched(result),
         "completed target press remains conservatively dispatched");
  expect(injector.has_fatal_io_error(),
         "partial high-level action retains fatal output state");
}

} // namespace

int main() {
  test_writable_stdout_preserves_exact_event_bytes();
  test_full_stdout_pipe_is_bounded();
  test_closed_stdout_is_immediate_epipe();
  test_action_result_rejects_output_failure_before_target_press();
  test_action_result_reports_output_failure_after_target_press();
  test_cancellation_aware_injection_releases_every_key();
  std::cout << "punto-key-injector-contract: OK\n";
}
