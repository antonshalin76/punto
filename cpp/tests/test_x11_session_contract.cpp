#include "punto/x11_session.hpp"

#include <X11/Xauth.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
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
constexpr unsigned short kFamilyInternet = 0;

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error{std::string{message}};
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
  struct Record {
    unsigned short family = FamilyLocal;
    std::string address;
    std::string number;
    std::string protocol = "MIT-MAGIC-COOKIE-1";
    std::string cookie;
  };

  std::string path;

  struct ReplacementCleanup {
    std::string path;
    ~ReplacementCleanup() {
      if (!path.empty()) {
        (void)::unlink(path.c_str());
      }
    }
  };

  struct StreamCloser {
    void operator()(FILE *file) const noexcept {
      if (file != nullptr) {
        (void)::fclose(file);
      }
    }
  };

  TempAuthority() {
    path = "/tmp/punto-x11-authority-XXXXXX";
    const int descriptor = ::mkstemp(path.data());
    expect(descriptor >= 0, "create temporary Xauthority");
    expect(::fchmod(descriptor, 0600) == 0, "protect temporary Xauthority");
    (void)::close(descriptor);
  }

  ~TempAuthority() { (void)::unlink(path.c_str()); }

  void replace(const std::vector<Record> &records,
               std::string_view trailing_bytes = {}) const {
    std::string replacement = path + ".replacement-XXXXXX";
    const int descriptor = ::mkstemp(replacement.data());
    expect(descriptor >= 0, "create replacement Xauthority");
    ReplacementCleanup cleanup{replacement};
    if (::fchmod(descriptor, 0600) != 0) {
      (void)::close(descriptor);
      fail("protect replacement Xauthority");
    }
    FILE *raw_file = ::fdopen(descriptor, "wb");
    if (raw_file == nullptr) {
      (void)::close(descriptor);
      fail("open replacement Xauthority stream");
    }
    std::unique_ptr<FILE, StreamCloser> file{raw_file};
    for (const auto &record : records) {
      Xauth auth{};
      auth.family = record.family;
      auth.address_length =
          static_cast<unsigned short>(record.address.size());
      auth.address = const_cast<char *>(record.address.data());
      auth.number_length = static_cast<unsigned short>(record.number.size());
      auth.number = const_cast<char *>(record.number.data());
      auth.name_length = static_cast<unsigned short>(record.protocol.size());
      auth.name = const_cast<char *>(record.protocol.data());
      auth.data_length = static_cast<unsigned short>(record.cookie.size());
      auth.data = const_cast<char *>(record.cookie.data());
      expect(::XauWriteAuth(file.get(), &auth) == 1,
             "write Xauthority record");
    }
    expect(trailing_bytes.empty() ||
               std::fwrite(trailing_bytes.data(), trailing_bytes.size(), 1,
                           file.get()) == 1,
           "write truncated Xauthority tail");
    expect(std::fflush(file.get()) == 0, "flush replacement Xauthority");
    expect(::fsync(::fileno(file.get())) == 0, "sync replacement Xauthority");
    expect(std::fclose(file.release()) == 0,
           "close replacement Xauthority");
    expect(::rename(replacement.c_str(), path.c_str()) == 0,
           "atomically replace Xauthority");
    cleanup.path.clear();
  }
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

std::string local_hostname() {
  std::array<char, 256> hostname{};
  expect(::gethostname(hostname.data(), hostname.size() - 1U) == 0,
         "read local hostname");
  return hostname.data();
}

std::string find_unused_display_number() {
  const unsigned int base =
      20000U + static_cast<unsigned int>(::getpid()) % 10000U;
  for (unsigned int offset = 0; offset < 1000U; ++offset) {
    const std::string number = std::to_string(base + offset);
    const std::string socket = "/tmp/.X11-unix/X" + number;
    struct stat metadata {};
    if (::lstat(socket.c_str(), &metadata) != 0 && errno == ENOENT) {
      return number;
    }
  }
  fail("find unused authenticated X display");
}

NestedXServer start_authenticated_xvfb(const TempAuthority &server_authority) {
  const std::string number = find_unused_display_number();
  const std::string display = ":" + number;
  const pid_t pid = ::fork();
  expect(pid >= 0, "fork authenticated Xvfb");
  if (pid == 0) {
    const int null_descriptor = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_descriptor >= 0) {
      (void)::dup2(null_descriptor, STDOUT_FILENO);
      (void)::dup2(null_descriptor, STDERR_FILENO);
      (void)::close(null_descriptor);
    }
    ::execl("/usr/bin/Xvfb", "Xvfb", display.c_str(), "-auth",
            server_authority.path.c_str(), "-nolisten", "tcp",
            static_cast<char *>(nullptr));
    _exit(127);
  }

  const std::string socket = "/tmp/.X11-unix/X" + number;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      fail("authenticated Xvfb exited during startup");
    }
    struct stat metadata {};
    if (::lstat(socket.c_str(), &metadata) == 0 && S_ISSOCK(metadata.st_mode)) {
      return NestedXServer{pid, display, false};
    }
    std::this_thread::sleep_for(10ms);
  }
  (void)::kill(pid, SIGTERM);
  (void)::waitpid(pid, nullptr, 0);
  fail("authenticated Xvfb startup timeout");
}

punto::X11SessionInfo real_candidate(const NestedXServer &server,
                                     const TempAuthority &authority) {
  punto::X11SessionInfo candidate = fake_info(server.display);
  candidate.uid = static_cast<std::uint32_t>(::geteuid());
  candidate.gid = static_cast<std::uint32_t>(::getegid());
  candidate.xauthority_path = authority.path;
  return candidate;
}

bool xkb_connection_succeeds(const punto::X11SessionInfo &candidate) {
  const auto started = std::chrono::steady_clock::now();
  punto::X11Session session{[candidate] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, candidate};
  }};
  if (!session.initialize()) {
    return false;
  }
  auto lease = session.acquire_write_lease();
  if (!lease) {
    return false;
  }
  auto connection = lease->open_bounded_connection(1s);
  if (!connection.is_open()) {
    return false;
  }
  const int group = session.get_current_keyboard_layout();
  return (group == 0 || group == 1) &&
         std::chrono::steady_clock::now() - started < 1500ms;
}

std::string display_number(const NestedXServer &server) {
  expect(server.display.starts_with(":"), "authenticated display is local");
  return server.display.substr(1);
}

TempAuthority::Record authority_record(unsigned short family,
                                       std::string address,
                                       std::string number,
                                       std::string cookie) {
  return TempAuthority::Record{family, std::move(address), std::move(number),
                               "MIT-MAGIC-COOKIE-1", std::move(cookie)};
}

void test_xauthority_metadata_policy_and_production_bridge() {
  using punto::x11_detail::XauthorityMetadata;
  using punto::x11_detail::xauthority_metadata_is_trusted;
  const std::uint32_t uid = static_cast<std::uint32_t>(::geteuid());
  expect(xauthority_metadata_is_trusted(
             XauthorityMetadata{uid, S_IFREG | 0600U, 16}, uid),
         "protected regular authority owned by the session is trusted");
  expect(!xauthority_metadata_is_trusted(
             XauthorityMetadata{uid + 1U, S_IFREG | 0600U, 16}, uid),
         "authority with the wrong owner is rejected");
  expect(!xauthority_metadata_is_trusted(
             XauthorityMetadata{uid, S_IFREG | 0620U, 16}, uid),
         "group-writable authority is rejected");
  expect(!xauthority_metadata_is_trusted(
             XauthorityMetadata{uid, S_IFREG | 0602U, 16}, uid),
         "world-writable authority is rejected");
  expect(!xauthority_metadata_is_trusted(
             XauthorityMetadata{uid, S_IFIFO | 0600U, 16}, uid),
         "non-regular authority is rejected");
  expect(!xauthority_metadata_is_trusted(
             XauthorityMetadata{uid, S_IFREG | 0600U, -1}, uid),
         "negative authority size is rejected");
  expect(!xauthority_metadata_is_trusted(
             XauthorityMetadata{
                 uid, S_IFREG | 0600U,
                 static_cast<std::int64_t>(
                     punto::x11_detail::kMaxXauthorityBytes) +
                     1},
             uid),
         "oversized authority is rejected");

  const std::string cookie(16, '\x31');
  TempAuthority server_authority;
  server_authority.replace(
      {authority_record(FamilyWild, "", "", cookie)});
  NestedXServer server = start_authenticated_xvfb(server_authority);
  TempAuthority client_authority;
  client_authority.replace({authority_record(
      FamilyLocal, local_hostname(), display_number(server), cookie)});
  expect(::chmod(client_authority.path.c_str(), 0620) == 0,
         "make client authority group-writable");
  expect(!xkb_connection_succeeds(real_candidate(server, client_authority)),
         "production connector applies the metadata policy");
  expect(::chmod(client_authority.path.c_str(), 0600) == 0,
         "restore protected client authority");
  expect(xkb_connection_succeeds(real_candidate(server, client_authority)),
         "same server accepts the protected authority control");
}

void test_authenticated_xauthority_empty_number_and_precedence() {
  const std::string cookie_a(16, '\x41');
  const std::string cookie_b(16, '\x42');
  TempAuthority server_authority;
  server_authority.replace(
      {authority_record(FamilyWild, "", "", cookie_a)});
  NestedXServer server = start_authenticated_xvfb(server_authority);
  const std::string number = display_number(server);
  const std::string hostname = local_hostname();
  TempAuthority client_authority;
  const auto candidate = [&] {
    return real_candidate(server, client_authority);
  };

  client_authority.replace({});
  expect(!xkb_connection_succeeds(candidate()),
         "authenticated Xvfb rejects a client without a cookie");
  client_authority.replace(
      {authority_record(FamilyLocal, hostname, "", cookie_a)});
  expect(xkb_connection_succeeds(candidate()),
         "GDM-style empty display number authorizes the production connector");

  const auto empty_wrong =
      authority_record(FamilyLocal, hostname, "", cookie_b);
  const auto exact_correct =
      authority_record(FamilyWild, "", number, cookie_a);
  const auto empty_correct =
      authority_record(FamilyLocal, hostname, "", cookie_a);
  const auto exact_wrong =
      authority_record(FamilyWild, "", number, cookie_b);
  for (const bool exact_first : {false, true}) {
    client_authority.replace(exact_first
                                 ? std::vector{exact_correct, empty_wrong}
                                 : std::vector{empty_wrong, exact_correct});
    expect(xkb_connection_succeeds(candidate()),
           "exact display number outranks a higher-family empty fallback");
    client_authority.replace(exact_first
                                 ? std::vector{exact_wrong, empty_correct}
                                 : std::vector{empty_correct, exact_wrong});
    expect(!xkb_connection_succeeds(candidate()),
           "rejected exact cookie is not retried with the empty fallback");
  }
}

void test_xauthority_family_precedence() {
  const std::string cookie_a(16, '\x51');
  const std::string cookie_b(16, '\x52');
  TempAuthority server_authority;
  server_authority.replace(
      {authority_record(FamilyWild, "", "", cookie_a)});
  NestedXServer server = start_authenticated_xvfb(server_authority);
  const std::string hostname = local_hostname();
  const std::string exact = display_number(server);
  TempAuthority client_authority;

  const auto expect_family_priority =
      [&](const TempAuthority::Record &higher_family,
          const TempAuthority::Record &lower_family,
          std::string_view rejection_message,
          std::string_view success_message) {
        for (const bool higher_first : {false, true}) {
          auto higher_wrong = higher_family;
          higher_wrong.cookie = cookie_b;
          auto lower_correct = lower_family;
          lower_correct.cookie = cookie_a;
          client_authority.replace(
              higher_first ? std::vector{higher_wrong, lower_correct}
                           : std::vector{lower_correct, higher_wrong});
          expect(!xkb_connection_succeeds(
                     real_candidate(server, client_authority)),
                 rejection_message);

          auto higher_correct = higher_family;
          higher_correct.cookie = cookie_a;
          auto lower_wrong = lower_family;
          lower_wrong.cookie = cookie_b;
          client_authority.replace(
              higher_first ? std::vector{higher_correct, lower_wrong}
                           : std::vector{lower_wrong, higher_correct});
          expect(xkb_connection_succeeds(
                     real_candidate(server, client_authority)),
                 success_message);
        }
      };
  for (const std::string &number : {std::string{}, exact}) {
    expect_family_priority(
        authority_record(FamilyLocal, hostname, number, cookie_a),
        authority_record(FamilyLocalHost, hostname, number, cookie_a),
        "FamilyLocal outranks FamilyLocalHost regardless of record order",
        "valid FamilyLocal wins over FamilyLocalHost");
    expect_family_priority(
        authority_record(FamilyLocalHost, hostname, number, cookie_a),
        authority_record(FamilyWild, "", number, cookie_a),
        "FamilyLocalHost outranks FamilyWild regardless of record order",
        "valid FamilyLocalHost wins over FamilyWild");
  }
}

void test_xauthority_invalid_records_and_truncated_tail_fail_closed() {
  const std::string cookie_a(16, '\x61');
  TempAuthority server_authority;
  server_authority.replace(
      {authority_record(FamilyWild, "", "", cookie_a)});
  NestedXServer server = start_authenticated_xvfb(server_authority);
  const std::string hostname = local_hostname();
  const std::string exact = display_number(server);
  TempAuthority client_authority;

  auto invalid_protocol =
      authority_record(FamilyLocal, hostname, exact, cookie_a);
  invalid_protocol.protocol = "XDM-AUTHORIZATION-1";
  auto invalid_cookie = authority_record(FamilyLocal, hostname, exact,
                                         std::string(15, '\x61'));
  auto invalid_address =
      authority_record(FamilyLocal, "not-this-host", exact, cookie_a);
  auto invalid_family =
      authority_record(kFamilyInternet, hostname, exact, cookie_a);
  for (const auto &invalid : {invalid_protocol, invalid_cookie,
                              invalid_address, invalid_family}) {
    const auto fallback =
        authority_record(FamilyWild, "", "", cookie_a);
    for (const bool invalid_first : {false, true}) {
      client_authority.replace(
          invalid_first ? std::vector{invalid, fallback}
                        : std::vector{fallback, invalid});
      expect(xkb_connection_succeeds(real_candidate(server, client_authority)),
             "invalid exact record cannot suppress a valid empty fallback");
    }
  }

  client_authority.replace({invalid_family});
  expect(!xkb_connection_succeeds(real_candidate(server, client_authority)),
         "unsupported FamilyInternet is never selected");

  client_authority.replace(
      {authority_record(FamilyWild, "", "", cookie_a)},
      std::string_view{"\0\1\0", 3});
  expect(!xkb_connection_succeeds(real_candidate(server, client_authority)),
         "truncated record after a valid cookie rejects the whole snapshot");
}

void test_xauthority_atomic_replacement_recovers_same_session() {
  const std::string cookie(16, '\x71');
  TempAuthority server_authority;
  server_authority.replace(
      {authority_record(FamilyWild, "", "", cookie)});
  NestedXServer server = start_authenticated_xvfb(server_authority);
  TempAuthority client_authority;
  client_authority.replace(
      {authority_record(FamilyWild, "", "99999", cookie)});
  const punto::X11SessionInfo candidate =
      real_candidate(server, client_authority);
  const char *authority_before_raw = std::getenv("XAUTHORITY");
  const std::optional<std::string> authority_before =
      authority_before_raw == nullptr
          ? std::nullopt
          : std::optional<std::string>{authority_before_raw};

  punto::X11Session session{[candidate] {
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, candidate};
  }};
  expect(session.initialize(), "recovery X11 snapshot commits");
  auto lease = session.acquire_write_lease();
  expect(lease.has_value(), "recovery snapshot grants one stable lease");
  auto rejected = lease->open_bounded_connection(1s);
  expect(!rejected.is_open(), "mismatched display record is rejected");
  const char *authority_after_failure_raw = std::getenv("XAUTHORITY");
  const std::optional<std::string> authority_after_failure =
      authority_after_failure_raw == nullptr
          ? std::nullopt
          : std::optional<std::string>{authority_after_failure_raw};
  expect(authority_after_failure == authority_before,
         "failed auth never mutates process-global XAUTHORITY");

  client_authority.replace(
      {authority_record(FamilyWild, "", "", cookie)});
  auto recovered = lease->open_bounded_connection(1s);
  expect(recovered.is_open(),
         "same X11 session recovers after atomic Xauthority replacement");
  const int group = session.get_current_keyboard_layout();
  expect(group == 0 || group == 1,
         "recovered connection reads the XKB layout");
  const char *authority_after_recovery_raw = std::getenv("XAUTHORITY");
  const std::optional<std::string> authority_after_recovery =
      authority_after_recovery_raw == nullptr
          ? std::nullopt
          : std::optional<std::string>{authority_after_recovery_raw};
  expect(authority_after_recovery == authority_before,
         "successful auth never mutates process-global XAUTHORITY");
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
  try {
    test_display_and_wayland_grammar();
    test_retry_schedule_is_exact_and_bounded();
    test_retry_wait_seam_is_deterministic();
    test_prepare_commit_and_failure_revoke_write();
    test_session_absence_is_distinct_from_failure();
    test_stale_generation_cannot_commit();
    test_shutdown_is_bounded_for_uncooperative_probe();
    test_xauthority_metadata_policy_and_production_bridge();
    test_authenticated_xauthority_empty_number_and_precedence();
    test_xauthority_family_precedence();
    test_xauthority_invalid_records_and_truncated_tail_fail_closed();
    test_xauthority_atomic_replacement_recovers_same_session();
    test_xkb_readiness_does_not_require_xfixes();
    test_unresponsive_handshake_is_cancelled_and_next_connect_recovers();
    test_bounded_transport_and_linearizable_write_gate();
    std::cout << "punto-x11-session-contract: OK\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
