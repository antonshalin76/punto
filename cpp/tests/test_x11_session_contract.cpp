#include "punto/x11_session.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

punto::X11SessionInfo fake_info(std::string display = ":42") {
  punto::X11SessionInfo info;
  info.session_id = "7";
  info.username = "alice";
  info.uid = 1000;
  info.gid = 1000;
  info.display = std::move(display);
  info.xauthority_path = "/run/user/1000/Xauthority";
  info.home_dir = "/home/alice";
  info.xdg_runtime_dir = "/run/user/1000";
  info.xdg_config_home = "/home/alice/.config";
  info.observed_keyboard_layout = 1;
  return info;
}

void test_display_and_wayland_grammar() {
  using punto::x11_detail::is_valid_local_display;
  expect(is_valid_local_display(":0"), "local display zero is accepted");
  expect(is_valid_local_display(":12.3"), "local display and screen accepted");
  expect(!is_valid_local_display("localhost:0"), "TCP display is rejected");
  expect(!is_valid_local_display("unix/:0"), "alternate transport rejected");
  expect(!is_valid_local_display(":0."), "empty screen rejected");
  expect(!is_valid_local_display(":-1"), "negative display rejected");

  using punto::x11_detail::is_valid_wayland_display;
  expect(is_valid_wayland_display(""), "empty Wayland value is valid");
  expect(is_valid_wayland_display("wayland-0"),
         "Wayland socket basename accepted");
  expect(!is_valid_wayland_display("../wayland-0"),
         "Wayland traversal rejected");
  expect(!is_valid_wayland_display("/run/user/1000/wayland-0"),
         "Wayland absolute path rejected");
}

void test_retry_schedule_is_exact_and_bounded() {
  using punto::x11_detail::retry_delay_after_failure;
  expect(!retry_delay_after_failure(0), "zero failures has no retry");
  expect(retry_delay_after_failure(1) == 250ms, "first retry is 250ms");
  expect(retry_delay_after_failure(2) == 500ms, "second retry is 500ms");
  expect(retry_delay_after_failure(3) == 1000ms, "third retry is 1000ms");
  expect(!retry_delay_after_failure(4), "fourth failure is terminal");
}

void test_retry_wait_seam_is_deterministic() {
  std::atomic<int> calls{0};
  std::vector<std::chrono::milliseconds> waits;
  punto::X11Session session{[&] {
                              calls.fetch_add(1, std::memory_order_relaxed);
                              return punto::x11_detail::ProbeResult{
                                  punto::x11_detail::ProbeStatus::Failed, {}};
                            },
                            [&](std::chrono::milliseconds delay,
                                const std::atomic<bool> &cancel_requested) {
                              waits.push_back(delay);
                              return cancel_requested.load(
                                  std::memory_order_acquire);
                            }};

  session.start_background_refresh();
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  std::optional<punto::X11Session::RefreshResult> result;
  while (!result && std::chrono::steady_clock::now() < deadline) {
    result = session.poll_refresh_result();
    std::this_thread::yield();
  }
  expect(result == punto::X11Session::RefreshResult::Failed,
         "fourth deterministic failure is terminal");
  expect(calls.load(std::memory_order_relaxed) == 4,
         "initial attempt plus exactly three retries");
  expect(waits == std::vector<std::chrono::milliseconds>{250ms, 500ms, 1000ms},
         "retry seam observes the exact schedule without sleeping");
}

void test_prepare_commit_and_failure_revoke_write() {
  std::atomic<int> call_count{0};
  punto::X11Session session{[&] {
    const int call = call_count.fetch_add(1, std::memory_order_relaxed);
    if (call == 0) {
      return punto::x11_detail::ProbeResult{
          punto::x11_detail::ProbeStatus::Healthy, fake_info()};
    }
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Failed, {}};
  }};

  expect(session.initialize(), "initial healthy snapshot commits");
  expect(session.is_valid(), "committed session enables writes");
  expect(session.info().observed_keyboard_layout == 1,
         "probe-observed layout is committed with the session snapshot");
  session.start_background_refresh();

  const auto revoke_deadline = std::chrono::steady_clock::now() + 250ms;
  while (session.is_valid() &&
         std::chrono::steady_clock::now() < revoke_deadline) {
    std::this_thread::yield();
  }
  expect(!session.is_valid(),
         "first failed attempt immediately revokes writes");
  expect(!session.poll_refresh_result(),
         "retry sequence does not publish a premature terminal result");
  expect(session.shutdown_background_refresh(100ms),
         "cooperative retry wait cancels within shutdown bound");
}

void test_session_absence_is_distinct_from_failure() {
  punto::X11Session session{[] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::SessionAbsent, {}};
  }};
  expect(!session.initialize(), "absence does not initialize X11");
  expect(session.refresh() == punto::X11Session::RefreshResult::SessionAbsent,
         "absence has an exact public outcome");
  expect(!session.is_valid(), "absence keeps write path disabled");
}

void test_stale_generation_cannot_commit() {
  std::atomic<bool> release{false};
  punto::X11Session session{[&] {
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, fake_info(":99")};
  }};

  session.start_background_refresh();
  session.reset();
  release.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  std::optional<punto::X11Session::RefreshResult> result;
  while (!result && std::chrono::steady_clock::now() < deadline) {
    result = session.poll_refresh_result();
    std::this_thread::yield();
  }
  expect(result == punto::X11Session::RefreshResult::Failed,
         "stale prepared generation is discarded");
  expect(!session.is_valid(), "stale generation cannot re-enable writes");
}

void test_shutdown_is_bounded_for_uncooperative_probe() {
  std::atomic<bool> release{false};
  {
    punto::X11Session session{[&] {
      while (!release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      return punto::x11_detail::ProbeResult{
          punto::x11_detail::ProbeStatus::SessionAbsent, {}};
    }};
    session.start_background_refresh();
    const auto started = std::chrono::steady_clock::now();
    expect(!session.shutdown_background_refresh(10ms),
           "uncooperative probe reports bounded shutdown failure");
    expect(std::chrono::steady_clock::now() - started < 250ms,
           "shutdown never waits for the stuck provider");
  }
  release.store(true, std::memory_order_release);
  std::this_thread::sleep_for(50ms);
}

struct TempAuthority {
  std::string path;

  TempAuthority() {
    path = "/tmp/punto-x11-authority-XXXXXX";
    const int descriptor = ::mkstemp(path.data());
    expect(descriptor >= 0, "create temporary Xauthority");
    expect(::fchmod(descriptor, 0600) == 0, "protect temporary Xauthority");
    (void)::close(descriptor);
  }

  ~TempAuthority() { (void)::unlink(path.c_str()); }
};

struct NestedXServer {
  pid_t pid = -1;
  std::string display;
  bool stopped = false;

  ~NestedXServer() { shutdown(); }

  void shutdown() {
    if (pid > 0) {
      if (stopped) {
        (void)::kill(pid, SIGCONT);
      }
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
      pid = -1;
      stopped = false;
    }
  }

  void stop() {
    expect(pid > 0 && ::kill(pid, SIGSTOP) == 0, "stop nested X server");
    stopped = true;
  }

  void resume() {
    expect(pid > 0 && ::kill(pid, SIGCONT) == 0, "resume nested X server");
    stopped = false;
  }
};

NestedXServer start_nested_xvfb(bool disable_xfixes = false) {
  int descriptors[2]{};
  expect(::pipe(descriptors) == 0, "create Xvfb display pipe");
  const pid_t pid = ::fork();
  expect(pid >= 0, "fork nested Xvfb");
  if (pid == 0) {
    (void)::close(descriptors[0]);
    if (::dup2(descriptors[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    (void)::close(descriptors[1]);
    if (disable_xfixes) {
      ::execl("/usr/bin/Xvfb", "Xvfb", "-displayfd", "1", "-ac", "-nolisten",
              "tcp", "-extension", "XFIXES", static_cast<char *>(nullptr));
    } else {
      ::execl("/usr/bin/Xvfb", "Xvfb", "-displayfd", "1", "-ac", "-nolisten",
              "tcp", static_cast<char *>(nullptr));
    }
    _exit(127);
  }

  (void)::close(descriptors[1]);
  pollfd descriptor{descriptors[0], POLLIN, 0};
  expect(::poll(&descriptor, 1, 2000) == 1, "nested Xvfb startup timeout");
  char buffer[32]{};
  const ssize_t count = ::read(descriptors[0], buffer, sizeof(buffer) - 1U);
  (void)::close(descriptors[0]);
  expect(count > 1, "nested Xvfb did not publish display");
  std::string number{buffer, static_cast<std::size_t>(count)};
  while (!number.empty() && (number.back() == '\n' || number.back() == '\r')) {
    number.pop_back();
  }
  expect(!number.empty(), "nested Xvfb display is empty");
  return NestedXServer{pid, ":" + number, false};
}

void test_xkb_readiness_does_not_require_xfixes() {
  NestedXServer server = start_nested_xvfb(/*disable_xfixes=*/true);
  TempAuthority authority;
  punto::X11SessionInfo candidate = fake_info(server.display);
  candidate.uid = static_cast<std::uint32_t>(::geteuid());
  candidate.gid = static_cast<std::uint32_t>(::getegid());
  candidate.xauthority_path = authority.path;

  punto::X11Session session{[candidate] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, candidate};
  }};
  expect(session.initialize(), "XKB-only X11 snapshot commits");
  auto lease = session.acquire_write_lease();
  expect(lease.has_value(), "XKB-only session grants a lease");
  auto connection = lease->open_bounded_connection(1s);
  expect(connection.is_open(),
         "production X11 transport does not require XFixes");
  const int group = session.get_current_keyboard_layout();
  expect(group == 0 || group == 1,
         "XKB layout observation works without XFixes");
}

struct HangingLocalXServer {
  pid_t pid = -1;
  std::string display;
  std::string socket_path;

  ~HangingLocalXServer() { shutdown(); }

  void shutdown() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
    if (!socket_path.empty()) {
      (void)::unlink(socket_path.c_str());
      socket_path.clear();
    }
  }
};

HangingLocalXServer start_hanging_local_x_server() {
  int listener = -1;
  std::string socket_path;
  unsigned int display_number =
      30000U + static_cast<unsigned int>(::getpid()) % 10000U;
  for (unsigned int attempt = 0; attempt < 1000U; ++attempt) {
    const unsigned int candidate = display_number + attempt;
    socket_path = "/tmp/.X11-unix/X" + std::to_string(candidate);
    listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    expect(listener >= 0, "create hanging X server socket");

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    expect(socket_path.size() < sizeof(address.sun_path),
           "hanging X server socket path fits sockaddr_un");
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    if (::bind(listener, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) == 0) {
      display_number = candidate;
      break;
    }
    const int bind_error = errno;
    (void)::close(listener);
    listener = -1;
    if (bind_error != EADDRINUSE) {
      fail("bind hanging X server socket");
    }
  }
  expect(listener >= 0, "find unused local X display");
  expect(::listen(listener, 1) == 0, "listen on hanging X server socket");

  const pid_t pid = ::fork();
  expect(pid >= 0, "fork hanging X server");
  if (pid == 0) {
    int client = -1;
    do {
      client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    } while (client < 0 && errno == EINTR);
    (void)::close(listener);
    if (client < 0) {
      _exit(126);
    }
    std::array<char, 4096> bytes{};
    while (true) {
      const ssize_t count = ::read(client, bytes.data(), bytes.size());
      if (count > 0 || (count < 0 && errno == EINTR)) {
        continue;
      }
      break;
    }
    (void)::close(client);
    _exit(0);
  }

  (void)::close(listener);
  return HangingLocalXServer{pid, ":" + std::to_string(display_number),
                             std::move(socket_path)};
}

void test_unresponsive_handshake_is_cancelled_and_next_connect_recovers() {
  HangingLocalXServer hanging = start_hanging_local_x_server();
  NestedXServer healthy = start_nested_xvfb();
  TempAuthority authority;
  const char *authority_before_raw = std::getenv("XAUTHORITY");
  const std::optional<std::string> authority_before =
      authority_before_raw == nullptr
          ? std::nullopt
          : std::optional<std::string>{authority_before_raw};

  punto::X11SessionInfo hanging_info = fake_info(hanging.display);
  hanging_info.uid = static_cast<std::uint32_t>(::geteuid());
  hanging_info.gid = static_cast<std::uint32_t>(::getegid());
  hanging_info.xauthority_path = authority.path;
  punto::X11Session hanging_session{[hanging_info] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, hanging_info};
  }};
  expect(hanging_session.initialize(), "hanging X snapshot commits");
  auto hanging_lease = hanging_session.acquire_write_lease();
  expect(hanging_lease.has_value(), "hanging X snapshot grants lease");

  const auto started = std::chrono::steady_clock::now();
  auto stalled = hanging_lease->open_bounded_connection(100ms);
  const bool timeout_is_bounded =
      !stalled.is_open() && std::chrono::steady_clock::now() - started < 500ms;
  const char *authority_during_raw = std::getenv("XAUTHORITY");
  const std::optional<std::string> authority_during =
      authority_during_raw == nullptr
          ? std::nullopt
          : std::optional<std::string>{authority_during_raw};

  punto::X11SessionInfo healthy_info = fake_info(healthy.display);
  healthy_info.uid = static_cast<std::uint32_t>(::geteuid());
  healthy_info.gid = static_cast<std::uint32_t>(::getegid());
  healthy_info.xauthority_path = authority.path;
  punto::X11Session healthy_session{[healthy_info] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, healthy_info};
  }};
  expect(healthy_session.initialize(), "healthy recovery snapshot commits");
  auto healthy_lease = healthy_session.acquire_write_lease();
  expect(healthy_lease.has_value(), "healthy recovery snapshot grants lease");
  auto recovered = healthy_lease->open_bounded_connection(1s);
  const bool recovery_succeeded = recovered.is_open();
  recovered.close();
  hanging.shutdown();
  healthy.shutdown();

  expect(timeout_is_bounded,
         "accepted X socket without handshake is cancelled by deadline");
  expect(authority_during == authority_before,
         "connector never overrides process-global XAUTHORITY");
  expect(recovery_succeeded,
         "healthy connection succeeds immediately after cancelled handshake");
}

void test_bounded_transport_and_linearizable_write_gate() {
  NestedXServer server = start_nested_xvfb();
  TempAuthority authority;
  punto::X11SessionInfo candidate = fake_info(server.display);
  candidate.uid = static_cast<std::uint32_t>(::geteuid());
  candidate.gid = static_cast<std::uint32_t>(::getegid());
  candidate.xauthority_path = authority.path;

  punto::X11Session session{[candidate] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, candidate};
  }};
  expect(session.initialize(), "test X11 snapshot commits");

  auto lease = session.acquire_write_lease();
  expect(lease.has_value(), "healthy session grants a write lease");
  auto connection = lease->open_bounded_connection(500ms);
  expect(connection.is_open(), "bounded connector reaches healthy Xvfb");
  auto checked_connection = lease->open_bounded_connection(500ms);
  expect(checked_connection.is_open(),
         "second bounded connection reaches healthy Xvfb");

  const auto reentrant_started = std::chrono::steady_clock::now();
  expect(session.is_valid(),
         "session status remains readable inside a write transaction");
  const int nested_group = session.get_current_keyboard_layout();
  expect(nested_group == 0 || nested_group == 1,
         "layout remains readable inside a write transaction");
  expect(std::chrono::steady_clock::now() - reentrant_started < 1s,
         "nested session reads cannot deadlock a write transaction");

  punto::x11_detail::XcbOperationResult healthy_check_result{};
  const auto healthy_noop = xcb_no_operation_checked(connection.get());
  expect(connection.check_request(healthy_noop,
                                  std::chrono::steady_clock::now() + 500ms,
                                  healthy_check_result) &&
             healthy_check_result ==
                 punto::x11_detail::XcbOperationResult::Success,
         "checked void request completes behind a bounded barrier");

  server.stop();
  const auto reply_cookie = xcb_get_input_focus(connection.get());
  punto::x11_detail::XcbOperationResult reply_result{};
  const auto reply_started = std::chrono::steady_clock::now();
  void *reply = connection.wait_for_reply(reply_cookie.sequence,
                                          reply_started + 100ms, reply_result);
  const bool reply_timed_out =
      reply == nullptr &&
      reply_result == punto::x11_detail::XcbOperationResult::TimedOut;
  std::free(reply);
  expect(reply_timed_out,
         "stopped X server times out a reply and closes the connection");
  expect(std::chrono::steady_clock::now() - reply_started < 500ms,
         "reply timeout is bounded on the calling thread");
  expect(!connection.is_open(), "reply timeout fails the connection closed");

  const auto checked_noop = xcb_no_operation_checked(checked_connection.get());
  punto::x11_detail::XcbOperationResult checked_result{};
  const auto checked_started = std::chrono::steady_clock::now();
  expect(!checked_connection.check_request(
             checked_noop, checked_started + 100ms, checked_result) &&
             checked_result == punto::x11_detail::XcbOperationResult::TimedOut,
         "stopped X server times out a checked request");
  expect(std::chrono::steady_clock::now() - checked_started < 500ms,
         "checked request timeout is bounded on the calling thread");
  expect(!checked_connection.is_open(),
         "checked request timeout fails the connection closed");

  const auto connect_started = std::chrono::steady_clock::now();
  auto stalled = lease->open_bounded_connection(100ms);
  expect(!stalled.is_open(), "stopped X server cannot block bounded connect");
  expect(std::chrono::steady_clock::now() - connect_started < 500ms,
         "connect timeout is bounded on the calling thread");

  std::atomic<bool> reset_done{false};
  std::thread resetter{[&] {
    session.reset();
    reset_done.store(true, std::memory_order_release);
  }};
  std::this_thread::sleep_for(20ms);
  expect(!reset_done.load(std::memory_order_acquire),
         "revocation waits for an acquired transaction lease");
  lease.reset();
  resetter.join();
  expect(reset_done.load(std::memory_order_acquire),
         "revocation completes after the transaction releases its lease");
  expect(!session.acquire_write_lease(),
         "revoked generation cannot grant another write lease");

  server.resume();
  expect(session.refresh() == punto::X11Session::RefreshResult::HealthyUpdated,
         "healthy refresh reopens a new write generation");
  auto recovered_lease = session.acquire_write_lease();
  expect(recovered_lease.has_value(), "new generation grants a fresh lease");
  auto recovered = recovered_lease->open_bounded_connection(1s);
  expect(recovered.is_open(),
         "single connector worker recovers after X resumes");
}

} // namespace

int main() {
  test_display_and_wayland_grammar();
  test_retry_schedule_is_exact_and_bounded();
  test_retry_wait_seam_is_deterministic();
  test_prepare_commit_and_failure_revoke_write();
  test_session_absence_is_distinct_from_failure();
  test_stale_generation_cannot_commit();
  test_shutdown_is_bounded_for_uncooperative_probe();
  test_xkb_readiness_does_not_require_xfixes();
  test_unresponsive_handshake_is_cancelled_and_next_connect_recovers();
  test_bounded_transport_and_linearizable_write_gate();
  std::cout << "punto-x11-session-contract: OK\n";
}
