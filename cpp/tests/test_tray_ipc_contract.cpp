#include "punto/ipc_client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TestFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string{message});
  }
}

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_{fd} {}
  ~UniqueFd() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        (void)::close(fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

class TempDirectory {
public:
  TempDirectory() {
    std::string pattern = "/tmp/punto-tray-ipc-XXXXXX";
    pattern.push_back('\0');
    char *created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
      throw TestFailure("mkdtemp failed");
    }
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::string socket_path() const { return path_ + "/s"; }
  [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
  std::string path_;
};

using ServerHandler = std::function<void(int)>;

class OneShotServer {
public:
  explicit OneShotServer(ServerHandler handler)
      : path_{directory_.socket_path()}, handler_{std::move(handler)} {
    listener_ = UniqueFd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (listener_.get() < 0) {
      throw TestFailure("server socket failed");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    require(path_.size() < sizeof(address.sun_path),
            "test socket path too long");
    std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1U);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path_.size() + 1U);
    if (::bind(listener_.get(), reinterpret_cast<sockaddr *>(&address),
               address_size) != 0 ||
        ::listen(listener_.get(), 1) != 0) {
      throw TestFailure("server bind/listen failed");
    }

    worker_ = std::thread([this]() {
      try {
        pollfd descriptor{listener_.get(), POLLIN, 0};
        if (::poll(&descriptor, 1, 3000) <= 0) {
          throw TestFailure("server accept timed out");
        }
        UniqueFd client{
            ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
        if (client.get() < 0) {
          throw TestFailure("server accept failed");
        }
        handler_(client.get());
      } catch (...) {
        failure_ = std::current_exception();
      }
    });
  }

  ~OneShotServer() {
    if (worker_.joinable()) {
      worker_.join();
    }
    (void)::unlink(path_.c_str());
  }

  OneShotServer(const OneShotServer &) = delete;
  OneShotServer &operator=(const OneShotServer &) = delete;

  [[nodiscard]] const std::string &path() const noexcept { return path_; }

  void verify() {
    if (worker_.joinable()) {
      worker_.join();
    }
    if (failure_) {
      std::rethrow_exception(failure_);
    }
  }

private:
  TempDirectory directory_;
  std::string path_;
  ServerHandler handler_;
  UniqueFd listener_;
  std::thread worker_;
  std::exception_ptr failure_;
};

std::string read_request(int fd) {
  std::string request;
  while (request.size() <= 254U) {
    char byte = '\0';
    const ssize_t count = ::recv(fd, &byte, 1, 0);
    if (count == 1) {
      request.push_back(byte);
      if (byte == '\n') {
        return request;
      }
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    throw TestFailure("server did not receive a complete request");
  }
  throw TestFailure("client sent an oversized request");
}

void send_all(int fd, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EPIPE || errno == ECONNRESET)) {
      return;
    }
    throw TestFailure("server send failed");
  }
}

punto::IpcClientResult run_exchange(std::string_view response,
                                    std::string_view command = "GET_STATUS") {
  OneShotServer server{[response](int fd) {
    (void)read_request(fd);
    send_all(fd, response);
  }};
  auto result =
      punto::IpcClient::exchange_for_test(std::string{command}, server.path());
  server.verify();
  return result;
}

void test_fragmented_status_response_and_request_frame() {
  std::string request;
  OneShotServer server{[&request](int fd) {
    request = read_request(fd);
    for (const char byte : std::string{"OK ENABLED\n"}) {
      send_all(fd, std::string_view{&byte, 1});
      std::this_thread::sleep_for(2ms);
    }
  }};

  const auto diagnostic = punto::IpcClient::diagnose_socket(server.path());
  server.verify();

  require(diagnostic.error == punto::IpcClientError::None,
          "fragmented response must be accepted");
  require(diagnostic.status == punto::ServiceStatus::Enabled,
          "diagnostic status mismatch");
  require(diagnostic.response == "OK ENABLED", "response payload mismatch");
  require(request == "GET_STATUS\n", "request must have exactly one LF");
}

std::vector<int> open_descriptors() {
  std::vector<int> descriptors;
  {
    const std::filesystem::directory_iterator entries{"/proc/self/fd"};
    for (const auto &entry : entries) {
      const std::string name = entry.path().filename().string();
      char *end = nullptr;
      errno = 0;
      const long parsed = std::strtol(name.c_str(), &end, 10);
      if (errno == 0 && end != name.c_str() && *end == '\0' && parsed >= 0 &&
          parsed <= std::numeric_limits<int>::max()) {
        descriptors.push_back(static_cast<int>(parsed));
      }
    }
  }
  std::erase_if(descriptors,
                [](int fd) { return ::fcntl(fd, F_GETFD, 0) < 0; });
  return descriptors;
}

void test_client_descriptor_is_nonblocking_and_close_on_exec() {
  std::mutex mutex;
  std::condition_variable condition;
  bool accepted = false;
  bool release = false;

  OneShotServer server{[&](int fd) {
    (void)read_request(fd);
    {
      const std::lock_guard lock{mutex};
      accepted = true;
    }
    condition.notify_all();
    {
      std::unique_lock lock{mutex};
      require(condition.wait_for(lock, 2s, [&]() { return release; }),
              "descriptor inspection timed out");
    }
    send_all(fd, "OK ENABLED\n");
  }};

  const std::vector<int> before = open_descriptors();
  punto::IpcClientResult result;
  std::thread client{
      [&]() { result = punto::IpcClient::diagnose_socket(server.path()); }};

  {
    std::unique_lock lock{mutex};
    require(condition.wait_for(lock, 2s, [&]() { return accepted; }),
            "client did not connect");
  }

  bool found_hardened_client = false;
  for (const int fd : open_descriptors()) {
    if (std::find(before.begin(), before.end(), fd) != before.end()) {
      continue;
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || !S_ISSOCK(status.st_mode)) {
      continue;
    }
    const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
    const int status_flags = ::fcntl(fd, F_GETFL, 0);
    if (descriptor_flags >= 0 && status_flags >= 0 &&
        (descriptor_flags & FD_CLOEXEC) != 0 &&
        (status_flags & O_NONBLOCK) != 0) {
      found_hardened_client = true;
      break;
    }
  }

  {
    const std::lock_guard lock{mutex};
    release = true;
  }
  condition.notify_all();
  client.join();
  server.verify();

  require(found_hardened_client,
          "connected client fd must be O_NONBLOCK and FD_CLOEXEC");
  require(result.ok(), "descriptor inspection must not disturb the exchange");
}

void test_typed_success_and_server_error() {
  const auto empty_ok = run_exchange("OK\n", "RELOAD");
  require(empty_ok.ok(), "exact OK must succeed");

  const auto message_ok = run_exchange("OK Reloaded\n", "RELOAD");
  require(message_ok.ok(), "OK plus a space-delimited message must succeed");

  const auto rejected = run_exchange("ERROR Rejected\n", "RELOAD");
  require(rejected.error == punto::IpcClientError::ServerRejected,
          "ERROR response must be classified as server rejection");

  for (const std::string_view false_positive :
       {"NOT OK\n", "OKAY\n", "prefix OK\n", "BROKEN ENABLED\n"}) {
    const auto result = run_exchange(false_positive, "RELOAD");
    require(result.error == punto::IpcClientError::ProtocolError,
            "success substrings must not be accepted");
  }
}

void test_status_is_exact() {
  for (const std::string_view invalid :
       {"OK\n", "OK ENABLED extra\n", "OK enabled\n", "OK DISABLED \n"}) {
    OneShotServer server{[invalid](int fd) {
      (void)read_request(fd);
      send_all(fd, invalid);
    }};
    const auto result = punto::IpcClient::diagnose_socket(server.path());
    server.verify();
    require(result.error == punto::IpcClientError::ProtocolError,
            "GET_STATUS must accept only exact typed states");
    require(result.status == punto::ServiceStatus::Unknown,
            "invalid status response must remain Unknown");
  }
}

void test_runtime_capability_schema_is_exact() {
  const std::string legacy =
      "OK x11_health=ready analysis_health=ready input_health=ready "
      "x11_last_progress_ms=0 analysis_last_progress_ms=0 input_last_progress_ms=0 "
      "analysis_outstanding=0 input_in_flight=0 log_dropped=0 text_mutation=disabled "
      "enabled=0 configured_enabled=1 config_pending=0 config_generation=1 config_result=ok "
      "analyzed=0 need_switch=0 corrections=0 pending_words=0 ready_results=0 worker_threads=1 "
      "daemon_peers=1 analysis_mode=auto control_plane=primary queued_tasks=0 "
      "avg_queue_us=0 avg_analysis_us=0 avg_macro_us=0 avg_tail_len=0";
  const auto replace = [](std::string text, std::string_view from, std::string_view to) {
    const auto offset = text.find(from);
    require(offset != std::string::npos, "mutation fixture field exists");
    text.replace(offset, from.size(), to);
    return text;
  };
  const auto modern = replace(replace(legacy, "text_mutation=disabled",
                                      "text_mutation=x11"),
                              "corrections=0", "corrections=0 word_dispatches=7");
  const auto exchange = [](const std::string &payload) {
    OneShotServer server{[&](int fd) {
      require(read_request(fd) == "STATS\n", "runtime snapshot requests exactly STATS");
      send_all(fd, payload + "\n");
    }};
    auto result = punto::IpcClient::diagnose_runtime_socket(server.path());
    server.verify();
    return result;
  };
  auto result = exchange(legacy);
  require(result.ok() && result.capability == punto::MutationCapability::Disabled &&
              result.status == punto::ServiceStatus::Disabled, "legacy disabled snapshot is supported");
  for (const bool enabled : {false, true}) {
    result = exchange(enabled ? replace(modern, "enabled=0", "enabled=1") : modern);
    require(result.ok() && result.capability == punto::MutationCapability::X11 &&
                result.status == (enabled ? punto::ServiceStatus::Enabled : punto::ServiceStatus::Disabled),
            "experimental capability is independent of runtime enabled");
  }
  for (const auto &payload : {
           replace(modern, "text_mutation=x11", "text_mutation=unknown"),
           replace(modern, "text_mutation=x11", "text_mutation=disabled"),
           replace(modern, " word_dispatches=7", ""),
           modern + " word_dispatches=7",
           replace(modern, "word_dispatches=7", "word_dispatches=18446744073709551616"),
           replace(modern, "word_dispatches=7", "word_dispatches=00"),
           replace(modern, "enabled=0", "enabled=2"),
           replace(legacy, "enabled=0", "enabled=1"),
           replace(modern, "corrections=0 word_dispatches=7", "word_dispatches=7 corrections=0"),
           modern + " "}) {
    result = exchange(payload);
    require(result.error == punto::IpcClientError::ProtocolError &&
                result.status == punto::ServiceStatus::Unknown &&
                result.capability == punto::MutationCapability::Unknown,
            "malformed or hybrid schema fails closed without a usable status");
  }
}

void test_auto_status_command_and_acknowledgement_are_exact() {
  for (const bool enabled : {false, true}) {
    const std::string expected = enabled ? "OK ENABLED\n" : "OK DISABLED\n";
    const std::string opposite = enabled ? "OK DISABLED\n" : "OK ENABLED\n";
    for (const auto &response : {expected, opposite, std::string{"OK unrelated\n"},
                                 std::string{"ERROR State publication not durable\n"}}) {
      OneShotServer server{[&](int fd) {
        require(read_request(fd) == (enabled ? "SET_STATUS 1\n" : "SET_STATUS 0\n"),
                "toggle submits the exact runtime status command");
        send_all(fd, response);
      }};
      const bool accepted = punto::IpcClient::set_auto_enabled_for_test(enabled, server.path());
      server.verify();
      require(accepted == (response == expected),
              "only exact matching status acknowledgement confirms the command");
    }
  }
}

void test_malformed_frames_are_rejected() {
  std::string with_nul{"OK ENABLED\0\n", 12U};
  std::string non_ascii{"OK ENABLED ", 11U};
  non_ascii.push_back(static_cast<char>(0xC3));
  non_ascii.push_back(static_cast<char>(0xA9));
  non_ascii.push_back('\n');

  const std::vector<std::string> invalid{"OK ENABLED",
                                         "OK ENABLED\r\n",
                                         "OK ENABLED\nEXTRA\n",
                                         "OK ENABLED\n\n",
                                         std::move(with_nul),
                                         std::move(non_ascii),
                                         std::string{"OK\tENABLED\n"}};

  for (const auto &frame : invalid) {
    const auto result = run_exchange(frame);
    require(result.error == punto::IpcClientError::ProtocolError,
            "malformed response frame must be rejected");
  }
}

void test_oversized_response_is_rejected() {
  std::string response = "OK ";
  response.append(punto::IpcClient::kMaxResponseBytes, 'A');
  response.push_back('\n');
  const auto result = run_exchange(response);
  require(result.error == punto::IpcClientError::ProtocolError,
          "oversized response must be rejected");
}

void test_request_validation_precedes_connect() {
  const std::string missing = "/tmp/punto-tray-ipc-definitely-missing.sock";
  require(punto::IpcClient::exchange_for_test("", missing).error ==
              punto::IpcClientError::InvalidRequest,
          "empty request must be rejected locally");
  require(punto::IpcClient::exchange_for_test("GET\nSTATUS", missing).error ==
              punto::IpcClientError::InvalidRequest,
          "embedded LF must be rejected locally");
  require(punto::IpcClient::exchange_for_test("GET\rSTATUS", missing).error ==
              punto::IpcClientError::InvalidRequest,
          "embedded CR must be rejected locally");
  require(punto::IpcClient::exchange_for_test(std::string{"GET\0STATUS", 10U},
                                              missing)
                  .error == punto::IpcClientError::InvalidRequest,
          "embedded NUL must be rejected locally");
  require(punto::IpcClient::exchange_for_test(std::string(255U, 'A'), missing)
                  .error == punto::IpcClientError::InvalidRequest,
          "oversized request must be rejected locally");
}

void test_path_and_connect_errors_are_classified() {
  const std::string too_long(sizeof(sockaddr_un::sun_path), 'x');
  require(punto::IpcClient::diagnose_socket(too_long).error ==
              punto::IpcClientError::PathTooLong,
          "overlong AF_UNIX path must not be truncated");

  const std::string missing = "/tmp/punto-tray-ipc-definitely-missing.sock";
  require(punto::IpcClient::diagnose_socket(missing).error ==
              punto::IpcClientError::Unavailable,
          "missing endpoint must be classified unavailable");

  if (::geteuid() != 0) {
    TempDirectory directory;
    const std::string socket_path = directory.socket_path();
    require(::chmod(directory.path().c_str(), 0000) == 0,
            "chmod test directory failed");
    const auto result = punto::IpcClient::diagnose_socket(socket_path);
    require(::chmod(directory.path().c_str(), 0700) == 0,
            "restore directory mode failed");
    require(result.error == punto::IpcClientError::PermissionDenied,
            "inaccessible endpoint must be classified permission denied");
  }
}

void noop_signal_handler(int) {}

void test_one_absolute_deadline_survives_eintr() {
  struct sigaction action {};
  action.sa_handler = noop_signal_handler;
  sigemptyset(&action.sa_mask);
  struct sigaction previous {};
  require(::sigaction(SIGUSR1, &action, &previous) == 0,
          "sigaction install failed");

  OneShotServer server{[](int fd) {
    (void)read_request(fd);
    send_all(fd, "O");
    std::this_thread::sleep_for(450ms);
    send_all(fd, "K");
    std::this_thread::sleep_for(450ms);
    send_all(fd, " ");
    std::this_thread::sleep_for(450ms);
    send_all(fd, "ENABLED\n");
  }};

  const pthread_t caller = ::pthread_self();
  std::atomic<bool> stop_signals{false};
  std::thread interrupter{[&]() {
    while (!stop_signals.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(25ms);
      (void)::pthread_kill(caller, SIGUSR1);
    }
  }};

  const auto started = std::chrono::steady_clock::now();
  const auto result = punto::IpcClient::diagnose_socket(server.path());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  stop_signals.store(true, std::memory_order_relaxed);
  interrupter.join();
  require(::sigaction(SIGUSR1, &previous, nullptr) == 0,
          "sigaction restore failed");
  server.verify();

  require(result.error == punto::IpcClientError::TimedOut,
          "slow fragmented response must time out");
  require(elapsed >= 850ms && elapsed < 1250ms,
          "EINTR or partial reads must not reset the absolute deadline");
}

void test_peer_close_does_not_raise_sigpipe() {
  OneShotServer server{[](int fd) {
    linger reset{1, 0};
    require(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0,
            "setsockopt SO_LINGER failed");
  }};

  const auto result = punto::IpcClient::exchange_for_test(
      std::string(254U, 'A'), server.path());
  server.verify();
  require(result.error == punto::IpcClientError::Unavailable ||
              result.error == punto::IpcClientError::IoError,
          "peer reset must be a typed transport failure");
}

void test_diagnostic_fallback_listing_is_explicit_and_sorted() {
  const auto paths = punto::IpcClient::list_diagnostic_socket_paths();
  require(std::is_sorted(paths.begin(), paths.end()),
          "diagnostic paths must be deterministic");
  require(std::find(paths.begin(), paths.end(),
                    punto::IpcClient::kSocketPath) == paths.end(),
          "primary endpoint must not be disguised as a fallback diagnostic");
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    void (*run)();
  };

  const std::vector<TestCase> tests{
      {"fragmented status", test_fragmented_status_response_and_request_frame},
      {"descriptor flags",
       test_client_descriptor_is_nonblocking_and_close_on_exec},
      {"typed success", test_typed_success_and_server_error},
      {"exact status", test_status_is_exact},
      {"runtime capability schema", test_runtime_capability_schema_is_exact},
      {"exact auto status command", test_auto_status_command_and_acknowledgement_are_exact},
      {"malformed frames", test_malformed_frames_are_rejected},
      {"oversized response", test_oversized_response_is_rejected},
      {"request validation", test_request_validation_precedes_connect},
      {"error classification", test_path_and_connect_errors_are_classified},
      {"absolute deadline", test_one_absolute_deadline_survives_eintr},
      {"SIGPIPE safety", test_peer_close_does_not_raise_sigpipe},
      {"diagnostic listing",
       test_diagnostic_fallback_listing_is_explicit_and_sorted},
  };

  std::size_t passed = 0;
  for (const auto &test : tests) {
    try {
      test.run();
      ++passed;
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &error) {
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
      return 1;
    }
  }

  std::cout << passed << "/" << tests.size() << " tests passed\n";
  return 0;
}
