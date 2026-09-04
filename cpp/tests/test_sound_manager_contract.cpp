#include "punto/config.hpp"
#include "punto/sound_manager.hpp"
#include "punto/x11_session.hpp"

#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class TestRunner {
public:
  void expect(bool condition, std::string_view message) {
    if (condition) {
      return;
    }
    ++failures_;
    std::cerr << "FAIL: " << message << '\n';
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

private:
  int failures_ = 0;
};

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

std::string make_temp_path() {
  std::string pattern = "/tmp/punto-sound-contract-XXXXXX";
  const int fd = ::mkstemp(pattern.data());
  if (fd < 0) {
    return {};
  }
  (void)::close(fd);
  (void)::unlink(pattern.c_str());
  return pattern;
}

std::optional<punto::X11SessionInfo> test_account() {
  const uid_t preferred_uid =
      ::geteuid() == 0 ? static_cast<uid_t>(65534) : ::geteuid();
  long requested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  std::size_t size =
      requested > 0 ? static_cast<std::size_t>(requested) : std::size_t{16384};
  size = std::min<std::size_t>(size, 1024U * 1024U);
  std::vector<char> buffer(size);
  passwd value{};
  passwd *result = nullptr;
  if (::getpwuid_r(preferred_uid, &value, buffer.data(), buffer.size(),
                   &result) != 0 ||
      result == nullptr || value.pw_name == nullptr) {
    return std::nullopt;
  }

  punto::X11SessionInfo info;
  info.username = value.pw_name;
  info.uid = static_cast<std::uint32_t>(value.pw_uid);
  info.gid = static_cast<std::uint32_t>(value.pw_gid);
  info.supplementary_groups = {info.gid};
  info.home_dir = value.pw_dir == nullptr ? "/tmp" : value.pw_dir;
  info.xdg_runtime_dir = "/run/user/" + std::to_string(info.uid);
  info.display = ":99";
  info.xauthority_path = info.home_dir + "/.Xauthority";
  return info;
}

punto::SoundManagerResolvedUser fake_user() {
  return punto::SoundManagerResolvedUser{
      .username = "punto-test",
      .home_dir = "/tmp/punto-test",
      .uid = static_cast<uid_t>(1234),
      .gid = static_cast<gid_t>(2345),
      .groups = {static_cast<gid_t>(2345), static_cast<gid_t>(3456)},
  };
}

punto::X11SessionInfo fake_session() {
  punto::X11SessionInfo info;
  info.username = "punto-test";
  info.uid = 1234;
  info.gid = 2345;
  info.supplementary_groups = {2345, 3456};
  info.home_dir = "/tmp/punto-test";
  info.xdg_runtime_dir = "/run/user/1234";
  info.display = ":42";
  info.xauthority_path = "/tmp/punto-test/.Xauthority";
  return info;
}

void test_identity_resolution_is_bounded_and_fail_closed(TestRunner &test) {
  const auto account = test_account();
  test.expect(account.has_value(),
              "a non-root local test account is available");
  if (!account) {
    return;
  }

  const auto resolved = punto::SoundManager::resolve_user_for_test(*account);
  test.expect(resolved.has_value(), "validated session identity is accepted");
  if (resolved) {
    test.expect(resolved->uid == static_cast<uid_t>(account->uid),
                "resolved uid matches the X11 snapshot");
    test.expect(resolved->gid == static_cast<gid_t>(account->gid),
                "resolved gid matches the X11 snapshot");
    test.expect(resolved->groups.size() <= 1024,
                "supplementary group result is bounded");
  }

  auto invalid = *account;
  invalid.uid = 0;
  test.expect(!punto::SoundManager::resolve_user_for_test(invalid),
              "root desktop identity fails closed");

  invalid = *account;
  invalid.supplementary_groups = {invalid.gid + 1U};
  test.expect(!punto::SoundManager::resolve_user_for_test(invalid),
              "group snapshot without primary gid is rejected");

  invalid = *account;
  invalid.username.assign(257, 'x');
  test.expect(!punto::SoundManager::resolve_user_for_test(invalid),
              "oversized username is rejected");

  invalid = *account;
  invalid.username.push_back('\0');
  invalid.username += "suffix";
  test.expect(!punto::SoundManager::resolve_user_for_test(invalid),
              "embedded NUL username is rejected");
}

void test_identity_resolution_never_blocks_owner(TestRunner &test) {
  struct ResolverState {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    std::atomic<int> launches{0};
  };
  const auto state = std::make_shared<ResolverState>();

  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.shutdown_wait = 50ms;
  options.resolve_user = [state](const punto::X11SessionInfo &,
                                 std::stop_token) {
    state->entered.store(true, std::memory_order_release);
    while (!state->release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(2ms);
    }
    state->finished.store(true, std::memory_order_release);
    return std::optional<punto::SoundManagerResolvedUser>{fake_user()};
  };
  options.launch = [state](const punto::SoundLaunchRequest &, std::stop_token) {
    state->launches.fetch_add(1, std::memory_order_relaxed);
    return punto::SoundLaunchResult::Completed;
  };

  punto::SoundConfig config;
  config.enabled = true;
  const auto construct_started = std::chrono::steady_clock::now();
  auto manager =
      std::make_unique<punto::SoundManager>(fake_session(), config, options);
  const auto construct_elapsed =
      std::chrono::steady_clock::now() - construct_started;
  test.expect(construct_elapsed < 250ms,
              "constructor does not wait for an unresponsive NSS resolver");
  test.expect(
      wait_until([&] { return state->entered.load(std::memory_order_acquire); },
                 1s),
      "identity lookup starts on the background worker");

  manager->play_for_layout(1);
  const auto shutdown_started = std::chrono::steady_clock::now();
  manager.reset();
  const auto shutdown_elapsed =
      std::chrono::steady_clock::now() - shutdown_started;
  test.expect(shutdown_elapsed < 500ms,
              "shutdown detaches a stuck identity resolver within its bound");

  state->release.store(true, std::memory_order_release);
  test.expect(
      wait_until(
          [&] { return state->finished.load(std::memory_order_acquire); }, 1s),
      "detached identity resolver can retire its shared state safely");
  std::this_thread::sleep_for(30ms);
  test.expect(state->launches.load(std::memory_order_relaxed) == 0,
              "retired identity completion cannot launch queued sound");
}

void test_no_launch_when_disabled_or_invalid(TestRunner &test) {
  std::atomic<int> launches{0};
  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.resolved_user = fake_user();
  options.launch = [&](const punto::SoundLaunchRequest &, std::stop_token) {
    launches.fetch_add(1, std::memory_order_relaxed);
    return punto::SoundLaunchResult::Completed;
  };

  punto::SoundConfig disabled;
  disabled.enabled = false;
  {
    punto::SoundManager manager(fake_session(), disabled, options);
    manager.play_for_layout(0);
    manager.play_for_layout(1);
  }
  test.expect(launches.load(std::memory_order_relaxed) == 0,
              "disabled sound never launches a player");

  punto::SoundConfig enabled;
  enabled.enabled = true;
  {
    punto::SoundManager manager(fake_session(), enabled, options);
    manager.play_for_layout(-1);
    manager.play_for_layout(2);
    std::this_thread::sleep_for(30ms);
  }
  test.expect(launches.load(std::memory_order_relaxed) == 0,
              "invalid layouts never launch a player");

  options.session_valid = false;
  {
    punto::SoundManager manager(fake_session(), enabled, options);
    manager.play_for_layout(1);
    std::this_thread::sleep_for(30ms);
  }
  test.expect(launches.load(std::memory_order_relaxed) == 0,
              "invalid X11 snapshot never launches a player");

  options.session_valid = true;
  options.resolved_user.reset();
  {
    punto::SoundManager manager(fake_session(), enabled, options);
    manager.play_for_layout(1);
    std::this_thread::sleep_for(30ms);
  }
  test.expect(launches.load(std::memory_order_relaxed) == 0,
              "unresolved credentials disable sound fail closed");

  options.resolved_user = fake_user();
  auto unsafe_session = fake_session();
  unsafe_session.xdg_runtime_dir += "\nLD_PRELOAD=/tmp/attack.so";
  {
    punto::SoundManager manager(unsafe_session, enabled, options);
    manager.play_for_layout(1);
    std::this_thread::sleep_for(30ms);
  }
  test.expect(launches.load(std::memory_order_relaxed) == 0,
              "control characters in session environment fail closed");
}

void test_burst_is_latest_wins_and_bounded(TestRunner &test) {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<punto::SoundLaunchRequest> requests;
  bool release_first = false;

  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.resolved_user = fake_user();
  options.minimum_launch_interval = 20ms;
  options.launch = [&](const punto::SoundLaunchRequest &request,
                       std::stop_token stop) {
    std::unique_lock lock(mutex);
    requests.push_back(request);
    cv.notify_all();
    if (requests.size() == 1) {
      cv.wait(lock, [&] { return release_first || stop.stop_requested(); });
    }
    return stop.stop_requested() ? punto::SoundLaunchResult::Stopped
                                 : punto::SoundLaunchResult::Completed;
  };

  punto::SoundConfig config;
  config.enabled = true;
  {
    punto::SoundManager manager(fake_session(), config, options);
    manager.play_for_layout(1);
    {
      std::unique_lock lock(mutex);
      test.expect(cv.wait_for(lock, 1s, [&] { return requests.size() == 1; }),
                  "the first sound starts asynchronously");
    }

    manager.play_for_layout(0);
    manager.play_for_layout(1);
    manager.play_for_layout(0);

    {
      std::lock_guard lock(mutex);
      release_first = true;
    }
    cv.notify_all();

    {
      std::unique_lock lock(mutex);
      test.expect(cv.wait_for(lock, 1s, [&] { return requests.size() == 2; }),
                  "one coalesced sound follows the active sound");
    }
    std::this_thread::sleep_for(80ms);
  }

  std::lock_guard lock(mutex);
  test.expect(requests.size() == 2,
              "a burst consumes at most the active and latest pending sounds");
  if (requests.size() == 2) {
    test.expect(requests[0].sound_path.find("en_ru.wav") != std::string::npos,
                "layout 1 maps to the EN-to-RU sound");
    test.expect(requests[1].sound_path.find("ru_en.wav") != std::string::npos,
                "the final layout 0 request wins coalescing");
    test.expect(requests[0].uid == static_cast<uid_t>(1234) &&
                    requests[0].gid == static_cast<gid_t>(2345),
                "launch request carries the validated target credentials");
    test.expect(requests[0].drop_privileges,
                "root-mode launch requires an explicit privilege drop");
    test.expect(requests[0].environment.size() == 6,
                "only the six allowlisted session variables are inherited");
    test.expect(std::none_of(requests[0].environment.begin(),
                             requests[0].environment.end(),
                             [](const std::string &entry) {
                               return entry.starts_with("LD_");
                             }),
                "loader-control variables are never inherited");
  }
}

void test_disabled_manager_can_be_enabled(TestRunner &test) {
  std::atomic<int> launches{0};
  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.resolved_user = fake_user();
  options.launch = [&](const punto::SoundLaunchRequest &, std::stop_token) {
    launches.fetch_add(1, std::memory_order_relaxed);
    return punto::SoundLaunchResult::Completed;
  };

  punto::SoundConfig config;
  config.enabled = false;
  {
    punto::SoundManager manager(fake_session(), config, options);
    manager.play_for_layout(1);
    std::this_thread::sleep_for(30ms);
    manager.set_enabled(true);
    manager.play_for_layout(1);
    test.expect(
        wait_until(
            [&] { return launches.load(std::memory_order_relaxed) == 1; }, 1s),
        "runtime enable starts accepting sound requests");
  }
  test.expect(launches.load(std::memory_order_relaxed) == 1,
              "disabled startup request is not replayed after enable");
}

void test_launch_rate_is_capped(TestRunner &test) {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::chrono::steady_clock::time_point> starts;

  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.resolved_user = fake_user();
  options.minimum_launch_interval = 100ms;
  options.launch = [&](const punto::SoundLaunchRequest &, std::stop_token) {
    std::lock_guard lock(mutex);
    starts.push_back(std::chrono::steady_clock::now());
    cv.notify_all();
    return punto::SoundLaunchResult::Completed;
  };

  punto::SoundConfig config;
  config.enabled = true;
  {
    punto::SoundManager manager(fake_session(), config, options);
    manager.play_for_layout(1);
    {
      std::unique_lock lock(mutex);
      test.expect(cv.wait_for(lock, 1s, [&] { return starts.size() == 1; }),
                  "first rate-limit sample starts");
    }
    manager.play_for_layout(0);
    {
      std::unique_lock lock(mutex);
      test.expect(cv.wait_for(lock, 1s, [&] { return starts.size() == 2; }),
                  "second rate-limit sample starts");
    }
  }

  std::lock_guard lock(mutex);
  if (starts.size() == 2) {
    test.expect(starts[1] - starts[0] >= 95ms,
                "launch starts obey the configured minimum interval");
  }
}

void test_disable_drops_pending_work(TestRunner &test) {
  std::mutex mutex;
  std::condition_variable cv;
  int launches = 0;
  bool release = false;

  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.resolved_user = fake_user();
  options.launch = [&](const punto::SoundLaunchRequest &,
                       std::stop_token stop) {
    std::unique_lock lock(mutex);
    ++launches;
    cv.notify_all();
    cv.wait(lock, [&] { return release || stop.stop_requested(); });
    return punto::SoundLaunchResult::Completed;
  };

  punto::SoundConfig config;
  config.enabled = true;
  {
    punto::SoundManager manager(fake_session(), config, options);
    manager.play_for_layout(1);
    {
      std::unique_lock lock(mutex);
      test.expect(cv.wait_for(lock, 1s, [&] { return launches == 1; }),
                  "active sound entered before disable");
    }
    manager.play_for_layout(0);
    manager.set_enabled(false);
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    cv.notify_all();
    std::this_thread::sleep_for(150ms);
  }
  test.expect(launches == 1, "disable atomically discards pending sound");
}

void test_shutdown_interrupts_active_work(TestRunner &test) {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;

  punto::SoundManagerTestOptions options;
  options.session_valid = true;
  options.player_path = "/bin/true";
  options.resolved_user = fake_user();
  options.launch = [&](const punto::SoundLaunchRequest &,
                       std::stop_token stop) {
    {
      std::lock_guard lock(mutex);
      entered = true;
    }
    cv.notify_all();
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(2ms);
    }
    return punto::SoundLaunchResult::Stopped;
  };

  punto::SoundConfig config;
  config.enabled = true;
  auto manager =
      std::make_unique<punto::SoundManager>(fake_session(), config, options);
  manager->play_for_layout(1);
  {
    std::unique_lock lock(mutex);
    test.expect(cv.wait_for(lock, 1s, [&] { return entered; }),
                "active launch entered before shutdown");
  }

  const auto started = std::chrono::steady_clock::now();
  manager.reset();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  test.expect(elapsed < 3s, "SoundManager shutdown joins within 3000 ms");
}

void test_child_runner_contract(TestRunner &test,
                                const std::string &self_path) {
  const std::string marker = make_temp_path();
  test.expect(!marker.empty(), "temporary marker path is available");
  if (marker.empty()) {
    return;
  }

  punto::SoundLaunchRequest request;
  request.uid = static_cast<uid_t>(1234);
  request.gid = static_cast<gid_t>(2345);
  request.groups = {static_cast<gid_t>(2345), static_cast<gid_t>(3456)};
  request.drop_privileges = true;
  request.player_path = "/bin/true";
  request.sound_path = marker;
  request.environment = {"HOME=/tmp", "USER=punto-test", "LOGNAME=punto-test",
                         "PUNTO_SOUND_SENTINEL=present"};

  std::stop_source source;
  const auto result = punto::SoundManager::run_process_for_test(
      request, self_path, 500ms, source.get_token());
  test.expect(result == punto::SoundLaunchResult::Completed,
              "checked child wrapper reports child success");

  std::ifstream input(marker);
  const std::string record((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
  test.expect(record.find("--reuid=1234\n") != std::string::npos,
              "privilege helper receives the target uid");
  test.expect(record.find("--regid=2345\n") != std::string::npos,
              "privilege helper receives the target gid");
  test.expect(record.find("--groups=2345,3456\n") != std::string::npos,
              "supplementary groups are set explicitly");
  test.expect(record.find("--no-new-privs\n") != std::string::npos,
              "spawned player cannot regain privileges");
  test.expect(record.find("--pdeathsig=keep\n") != std::string::npos,
              "privilege helper preserves the daemon parent-death signal");
  test.expect(record.find("sentinel=present\n") != std::string::npos,
              "the explicit environment is passed to exec");
  (void)::unlink(marker.c_str());

  request.groups.clear();
  request.sound_path = make_temp_path();
  const std::string empty_group_marker = request.sound_path;
  const auto empty_group_result = punto::SoundManager::run_process_for_test(
      request, self_path, 500ms, source.get_token());
  test.expect(empty_group_result == punto::SoundLaunchResult::Completed,
              "empty supplementary-group launch succeeds in the seam");
  std::ifstream empty_input(empty_group_marker);
  const std::string empty_record((std::istreambuf_iterator<char>(empty_input)),
                                 std::istreambuf_iterator<char>());
  test.expect(empty_record.find("--clear-groups\n") != std::string::npos,
              "empty groups still issue a mandatory clear-groups operation");
  (void)::unlink(empty_group_marker.c_str());

  request.sound_path = "/tmp/unused-punto-sound";
  const auto missing = punto::SoundManager::run_process_for_test(
      request, "/definitely/missing/punto-setpriv", 500ms, source.get_token());
  test.expect(missing == punto::SoundLaunchResult::SpawnFailed,
              "exec failure is synchronously reported by the child wrapper");

  request.drop_privileges = false;
  request.player_path = "/bin/true";
  const auto same_user = punto::SoundManager::run_process_for_test(
      request, "/definitely/missing/punto-setpriv", 500ms, source.get_token());
  test.expect(same_user == punto::SoundLaunchResult::Completed,
              "same-user mode safely execs the absolute player directly");

  request.drop_privileges = true;
  request.sound_path = "/tmp/__PUNTO_SOUND_FAIL__";
  const auto child_failure = punto::SoundManager::run_process_for_test(
      request, self_path, 500ms, source.get_token());
  test.expect(child_failure == punto::SoundLaunchResult::ExitedFailure,
              "privilege-helper and player failures are observed by the owner");
}

void test_child_runner_rejects_unsafe_inputs(TestRunner &test,
                                             const std::string &self_path) {
  punto::SoundLaunchRequest request;
  request.uid = static_cast<uid_t>(1234);
  request.gid = static_cast<gid_t>(2345);
  request.groups = {static_cast<gid_t>(2345)};
  request.drop_privileges = true;
  request.player_path = "/bin/true";
  request.sound_path = "/tmp/punto-safe-sound";
  request.environment = {"HOME=/tmp", "USER=punto-test"};

  const auto rejects = [&](std::string_view description) {
    std::stop_source source;
    const auto result = punto::SoundManager::run_process_for_test(
        request, self_path, 500ms, source.get_token());
    test.expect(result == punto::SoundLaunchResult::SpawnFailed, description);
  };

  request.player_path = "bin/true";
  rejects("relative player paths fail closed before fork");
  request.player_path = "/bin/true";

  request.sound_path = std::string{"/tmp/sound\0ignored", 18};
  rejects("embedded NUL arguments fail closed before fork");
  request.sound_path = "/tmp/punto-safe-sound";

  request.environment = {"HOME=/tmp\nLD_PRELOAD=/tmp/attack.so"};
  rejects("environment control-character injection fails closed before fork");
  request.environment = {"LD_PRELOAD=/tmp/attack.so"};
  rejects("non-allowlisted loader environment fails closed before fork");
  request.environment = {"=value"};
  rejects("empty environment variable names fail closed before fork");
  request.environment = {"lowercase=value"};
  rejects("unknown environment variable names fail closed before fork");
  request.environment = {"HOME=/tmp"};

  request.groups.assign(1025, static_cast<gid_t>(2345));
  rejects("oversized supplementary group lists fail closed before fork");
}

void test_child_runner_stops_hung_child(TestRunner &test,
                                        const std::string &self_path) {
  punto::SoundLaunchRequest request;
  request.uid = static_cast<uid_t>(1234);
  request.gid = static_cast<gid_t>(2345);
  request.groups.clear();
  request.drop_privileges = true;
  request.player_path = "/bin/true";
  request.sound_path = "/tmp/__PUNTO_SOUND_HANG__";
  request.environment = {"HOME=/tmp", "USER=punto-test", "LOGNAME=punto-test"};

  std::stop_source source;
  punto::SoundLaunchResult result = punto::SoundLaunchResult::Completed;
  std::jthread runner([&] {
    result = punto::SoundManager::run_process_for_test(request, self_path, 5s,
                                                       source.get_token());
  });

  std::this_thread::sleep_for(100ms);
  const auto started = std::chrono::steady_clock::now();
  source.request_stop();
  runner.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  test.expect(result == punto::SoundLaunchResult::Stopped,
              "stop requests terminate the active player");
  test.expect(elapsed < 1s, "hung child termination and reap are bounded");

  std::stop_source timeout_source;
  const auto timeout_started = std::chrono::steady_clock::now();
  const auto timeout_result = punto::SoundManager::run_process_for_test(
      request, self_path, 100ms, timeout_source.get_token());
  const auto timeout_elapsed =
      std::chrono::steady_clock::now() - timeout_started;
  test.expect(timeout_result == punto::SoundLaunchResult::TimedOut,
              "maximum player runtime terminates a hung child");
  test.expect(timeout_elapsed < 1s, "player timeout remains bounded");
}

void test_player_dies_with_owner_process(TestRunner &test,
                                         const std::string &self_path) {
  const std::string pid_file = make_temp_path();
  test.expect(!pid_file.empty(), "parent-death test pid path is available");
  if (pid_file.empty()) {
    return;
  }

  const pid_t owner = ::fork();
  test.expect(owner >= 0, "parent-death test owner process starts");
  if (owner < 0) {
    return;
  }
  if (owner == 0) {
    punto::SoundLaunchRequest request;
    request.uid = static_cast<uid_t>(1234);
    request.gid = static_cast<gid_t>(2345);
    request.drop_privileges = true;
    request.player_path = "/bin/true";
    request.sound_path = "/tmp/__PUNTO_SOUND_PDEATH_HANG__";
    request.environment = {
        "HOME=/tmp",
        "USER=punto-test",
        "LOGNAME=punto-test",
        "PUNTO_SOUND_PID_FILE=" + pid_file,
    };
    std::stop_source source;
    (void)punto::SoundManager::run_process_for_test(request, self_path, 20s,
                                                    source.get_token());
    ::_exit(90);
  }

  pid_t player = -1;
  const bool player_started = wait_until(
      [&] {
        std::ifstream input(pid_file);
        long long value = -1;
        input >> value;
        if (input && value > 1 &&
            value <=
                static_cast<long long>(std::numeric_limits<pid_t>::max())) {
          player = static_cast<pid_t>(value);
          return true;
        }
        return false;
      },
      2s);
  test.expect(player_started,
              "spawned player reports its pid before owner termination");

  (void)::kill(owner, SIGKILL);
  int owner_status = 0;
  while (::waitpid(owner, &owner_status, 0) < 0 && errno == EINTR) {
  }

  const bool player_gone =
      player_started &&
      wait_until(
          [&] {
            if (::kill(player, 0) != 0 && errno == ESRCH) {
              return true;
            }
            std::ifstream status{"/proc/" + std::to_string(player) + "/stat"};
            std::string line;
            std::getline(status, line);
            const std::size_t end_name = line.rfind(')');
            return !status ||
                   (end_name != std::string::npos &&
                    end_name + 2 < line.size() && line[end_name + 2] == 'Z');
          },
          2s);
  test.expect(player_gone,
              "PDEATHSIG terminates the player when its daemon owner dies");
  if (player_started && !player_gone) {
    (void)::kill(player, SIGKILL);
  }
  (void)::unlink(pid_file.c_str());
}

int sound_helper_mode(int argc, char **argv) {
  const std::string_view final_argument = argv[argc - 1];
  if (final_argument.ends_with("__PUNTO_SOUND_PDEATH_HANG__")) {
    if (const char *pid_file = std::getenv("PUNTO_SOUND_PID_FILE");
        pid_file != nullptr) {
      std::ofstream output{pid_file};
      output << ::getpid() << '\n';
      output.flush();
    }
    while (true) {
      ::pause();
    }
  }
  if (final_argument.ends_with("__PUNTO_SOUND_HANG__")) {
    while (true) {
      ::pause();
    }
  }
  if (final_argument.ends_with("__PUNTO_SOUND_FAIL__")) {
    return 93;
  }

  std::ofstream output{std::string{final_argument}};
  if (!output) {
    return 91;
  }
  for (int i = 1; i < argc; ++i) {
    output << argv[i] << '\n';
  }
  const char *sentinel = std::getenv("PUNTO_SOUND_SENTINEL");
  output << "sentinel=" << (sentinel == nullptr ? "" : sentinel) << '\n';
  return output ? 0 : 92;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 2 && std::string_view{argv[1]}.starts_with("--reuid=")) {
    return sound_helper_mode(argc, argv);
  }

  TestRunner test;
  test_identity_resolution_is_bounded_and_fail_closed(test);
  test_identity_resolution_never_blocks_owner(test);
  test_no_launch_when_disabled_or_invalid(test);
  test_burst_is_latest_wins_and_bounded(test);
  test_disabled_manager_can_be_enabled(test);
  test_launch_rate_is_capped(test);
  test_disable_drops_pending_work(test);
  test_shutdown_interrupts_active_work(test);
  test_child_runner_contract(test, "/proc/self/exe");
  test_child_runner_rejects_unsafe_inputs(test, "/proc/self/exe");
  test_child_runner_stops_hung_child(test, "/proc/self/exe");
  test_player_dies_with_owner_process(test, "/proc/self/exe");

  if (test.failures() != 0) {
    std::cerr << test.failures() << " sound contract test(s) failed\n";
    return 1;
  }
  std::cout << "sound manager contract tests passed\n";
  return 0;
}
