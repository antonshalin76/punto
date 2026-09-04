/**
 * @file sound_manager.cpp
 * @brief Non-blocking, privilege-separated layout sound playback.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "punto/sound_manager.hpp"

#include "punto/config.hpp"
#include "punto/x11_session.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace punto {

namespace {

using namespace std::chrono_literals;

inline constexpr std::string_view kSoundEnToRu =
    "/usr/share/punto-switcher/sounds/en_ru.wav";
inline constexpr std::string_view kSoundRuToEn =
    "/usr/share/punto-switcher/sounds/ru_en.wav";
#ifndef PUNTO_TESTING
inline constexpr std::string_view kPaplayPath = "/usr/bin/paplay";
inline constexpr std::string_view kAplayPath = "/usr/bin/aplay";
#endif
inline constexpr std::string_view kSetprivPath = "/usr/bin/setpriv";

inline constexpr std::size_t kMaximumUsernameBytes = 256;
inline constexpr std::size_t kMaximumSupplementaryGroups = 1024;
inline constexpr std::size_t kMaximumEnvironmentValueBytes = 4096;
#ifndef PUNTO_TESTING
inline constexpr auto kMinimumLaunchInterval = 100ms;
#endif
inline constexpr auto kMaximumPlayerRuntime = 2s;
inline constexpr auto kTerminationGrace = 150ms;
inline constexpr auto kKillReapGrace = 500ms;
inline constexpr auto kWaitPollInterval = 10ms;
inline constexpr auto kShutdownWait = 2500ms;

#ifdef PUNTO_TESTING
using LaunchResult = SoundLaunchResult;
using LaunchRequest = SoundLaunchRequest;
using ResolvedUser = SoundManagerResolvedUser;
#else
enum class LaunchResult {
  Completed,
  SpawnFailed,
  ExitedFailure,
  TimedOut,
  Stopped,
};

struct ResolvedUser {
  std::string username;
  std::string home_dir;
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> groups;
};

struct LaunchRequest {
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> groups;
  bool drop_privileges = true;
  std::string player_path;
  std::string sound_path;
  std::vector<std::string> environment;
};
#endif

[[nodiscard]] bool has_embedded_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool is_safe_environment_value(std::string_view value) {
  if (value.size() > kMaximumEnvironmentValueBytes || has_embedded_nul(value)) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20U || character == 0x7fU;
  });
}

[[nodiscard]] bool is_absolute_safe_path(std::string_view value) {
  return !value.empty() && value.front() == '/' &&
         is_safe_environment_value(value);
}

#ifndef PUNTO_TESTING
[[nodiscard]] bool is_executable(std::string_view path) {
  const std::string value{path};
  return ::access(value.c_str(), X_OK) == 0;
}
#endif

[[nodiscard]] std::optional<ResolvedUser>
resolve_user(const X11SessionInfo &session) {
  if (session.username.empty() ||
      session.username.size() > kMaximumUsernameBytes ||
      has_embedded_nul(session.username)) {
    return std::nullopt;
  }

  const uid_t expected_uid = static_cast<uid_t>(session.uid);
  const gid_t expected_gid = static_cast<gid_t>(session.gid);
  if (static_cast<std::uintmax_t>(expected_uid) != session.uid ||
      static_cast<std::uintmax_t>(expected_gid) != session.gid ||
      expected_uid == 0 || !is_absolute_safe_path(session.home_dir) ||
      session.supplementary_groups.empty() ||
      session.supplementary_groups.size() > kMaximumSupplementaryGroups) {
    return std::nullopt;
  }
  std::vector<gid_t> groups;
  groups.reserve(session.supplementary_groups.size());
  for (const std::uint32_t group : session.supplementary_groups) {
    groups.push_back(static_cast<gid_t>(group));
  }
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  if (std::find(groups.begin(), groups.end(), expected_gid) == groups.end()) {
    return std::nullopt;
  }

  return ResolvedUser{
      .username = session.username,
      .home_dir = session.home_dir,
      .uid = expected_uid,
      .gid = expected_gid,
      .groups = std::move(groups),
  };
}

[[nodiscard]] std::optional<std::vector<std::string>>
make_environment(const X11SessionInfo &session, const ResolvedUser &user) {
  if (!is_safe_environment_value(user.username) ||
      !is_absolute_safe_path(user.home_dir)) {
    return std::nullopt;
  }

  std::vector<std::string> environment;
  environment.reserve(6);
  environment.push_back("HOME=" + user.home_dir);
  environment.push_back("USER=" + user.username);
  environment.push_back("LOGNAME=" + user.username);

  const auto append = [&environment](std::string_view name,
                                     const std::string &value,
                                     bool require_absolute_path) {
    if (value.empty()) {
      return true;
    }
    if (!is_safe_environment_value(value) ||
        (require_absolute_path && !is_absolute_safe_path(value))) {
      return false;
    }
    environment.push_back(std::string{name} + value);
    return true;
  };

  if (!append("XDG_RUNTIME_DIR=", session.xdg_runtime_dir, true) ||
      !append("DISPLAY=", session.display, false) ||
      !append("XAUTHORITY=", session.xauthority_path, true)) {
    return std::nullopt;
  }
  return environment;
}

[[nodiscard]] std::string group_argument(const std::vector<gid_t> &groups) {
  std::string value{"--groups="};
  for (std::size_t index = 0; index < groups.size(); ++index) {
    if (index != 0) {
      value.push_back(',');
    }
    value += std::to_string(static_cast<std::uintmax_t>(groups[index]));
  }
  return value;
}

[[nodiscard]] bool is_safe_environment_entry(std::string_view entry) {
  if (!is_safe_environment_value(entry)) {
    return false;
  }
  const std::size_t separator = entry.find('=');
  if (separator == 0 || separator == std::string_view::npos) {
    return false;
  }
  const std::string_view name = entry.substr(0, separator);
  const std::array<std::string_view, 6> allowed_names = {
      "HOME", "USER", "LOGNAME", "XDG_RUNTIME_DIR", "DISPLAY", "XAUTHORITY",
  };
  if (std::find(allowed_names.begin(), allowed_names.end(), name) !=
      allowed_names.end()) {
    return true;
  }
#ifdef PUNTO_TESTING
  return name == "PUNTO_SOUND_SENTINEL" || name == "PUNTO_SOUND_PID_FILE";
#else
  return false;
#endif
}

[[nodiscard]] bool poll_reaped(pid_t pid, int &status,
                               std::chrono::steady_clock::time_point deadline) {
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      return true;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
    std::this_thread::sleep_for(kWaitPollInterval);
  }
  return ::waitpid(pid, &status, WNOHANG) == pid;
}

void signal_process_group(pid_t pid, int signal_number) noexcept {
  if (::kill(-pid, signal_number) != 0 && errno == ESRCH) {
    (void)::kill(pid, signal_number);
  }
}

[[nodiscard]] LaunchResult stop_and_reap(pid_t pid, LaunchResult result) {
  int status = 0;
  signal_process_group(pid, SIGTERM);
  if (poll_reaped(pid, status,
                  std::chrono::steady_clock::now() + kTerminationGrace)) {
    return result;
  }

  signal_process_group(pid, SIGKILL);
  if (poll_reaped(pid, status,
                  std::chrono::steady_clock::now() + kKillReapGrace)) {
    return result;
  }

  std::cerr << "[punto] Sound: child could not be reaped after SIGKILL\n";
  return LaunchResult::ExitedFailure;
}

[[noreturn]] void child_exec(pid_t expected_parent,
                             const std::string &executable,
                             std::vector<char *> &argv,
                             std::vector<char *> &envp) noexcept {
  // This post-fork path uses only async-signal-safe operations until execve.
  // setpriv keeps the death signal across its credential transition.
  if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 ||
      ::getppid() != expected_parent || ::setsid() < 0) {
    ::_exit(127);
  }

  const int null_descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null_descriptor < 0) {
    ::_exit(127);
  }
  for (int descriptor : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    if (::dup2(null_descriptor, descriptor) < 0) {
      ::_exit(127);
    }
  }
  if (null_descriptor > STDERR_FILENO) {
    (void)::close(null_descriptor);
  }

  sigset_t empty_mask{};
  if (::sigemptyset(&empty_mask) != 0 ||
      ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0) {
    ::_exit(127);
  }
  struct sigaction default_action {};
  default_action.sa_handler = SIG_DFL;
  if (::sigemptyset(&default_action.sa_mask) != 0) {
    ::_exit(127);
  }
  for (int signal_number : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) {
    if (::sigaction(signal_number, &default_action, nullptr) != 0) {
      ::_exit(127);
    }
  }

  // A player must not inherit input devices, IPC sockets, or log descriptors.
  if (::close_range(3U, std::numeric_limits<unsigned int>::max(), 0) != 0) {
    ::_exit(127);
  }
  ::execve(executable.c_str(), argv.data(), envp.data());
  ::_exit(127);
}

[[nodiscard]] LaunchResult
run_process(const LaunchRequest &request, std::string_view helper_path,
            std::chrono::milliseconds maximum_runtime, std::stop_token stop) {
  const std::string executable =
      request.drop_privileges ? std::string{helper_path} : request.player_path;
  if (!is_absolute_safe_path(executable) ||
      !is_absolute_safe_path(request.player_path) ||
      !is_absolute_safe_path(request.sound_path) ||
      request.groups.size() > kMaximumSupplementaryGroups ||
      maximum_runtime <= 0ms ||
      !std::all_of(request.environment.begin(), request.environment.end(),
                   is_safe_environment_entry)) {
    return LaunchResult::SpawnFailed;
  }

  std::vector<std::string> arguments;
  if (request.drop_privileges) {
    arguments.reserve(12);
    arguments.emplace_back(helper_path);
    arguments.push_back(
        "--reuid=" + std::to_string(static_cast<std::uintmax_t>(request.uid)));
    arguments.push_back(
        "--regid=" + std::to_string(static_cast<std::uintmax_t>(request.gid)));
    if (request.groups.empty()) {
      arguments.emplace_back("--clear-groups");
    } else {
      arguments.push_back(group_argument(request.groups));
    }
    arguments.emplace_back("--inh-caps=-all");
    arguments.emplace_back("--ambient-caps=-all");
    arguments.emplace_back("--bounding-set=-all");
    arguments.emplace_back("--no-new-privs");
    arguments.emplace_back("--pdeathsig=keep");
    arguments.emplace_back("--");
    arguments.push_back(request.player_path);
    arguments.push_back(request.sound_path);
  } else {
    arguments.reserve(2);
    arguments.push_back(request.player_path);
    arguments.push_back(request.sound_path);
  }

  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1U);
  for (std::string &argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::vector<char *> envp;
  envp.reserve(request.environment.size() + 1U);
  std::vector<std::string> environment = request.environment;
  for (std::string &entry : environment) {
    envp.push_back(entry.data());
  }
  envp.push_back(nullptr);

  const pid_t expected_parent = ::getpid();
  const pid_t pid = ::fork();
  if (pid < 0) {
    return LaunchResult::SpawnFailed;
  }
  if (pid == 0) {
    child_exec(expected_parent, executable, argv, envp);
  }

  const auto deadline = std::chrono::steady_clock::now() + maximum_runtime;
  int status = 0;
  while (true) {
    const pid_t wait_result = ::waitpid(pid, &status, WNOHANG);
    if (wait_result == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return LaunchResult::Completed;
      }
      return WIFEXITED(status) && WEXITSTATUS(status) == 127
                 ? LaunchResult::SpawnFailed
                 : LaunchResult::ExitedFailure;
    }
    if (wait_result < 0 && errno != EINTR) {
      return LaunchResult::ExitedFailure;
    }
    if (stop.stop_requested()) {
      return stop_and_reap(pid, LaunchResult::Stopped);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return stop_and_reap(pid, LaunchResult::TimedOut);
    }
    std::this_thread::sleep_for(kWaitPollInterval);
  }
}

#ifndef PUNTO_TESTING
[[nodiscard]] std::string select_player() {
  if (is_executable(kPaplayPath)) {
    return std::string{kPaplayPath};
  }
  if (is_executable(kAplayPath)) {
    return std::string{kAplayPath};
  }
  return {};
}
#endif

} // namespace

struct SoundManager::Impl {
  using Resolver = std::function<std::optional<ResolvedUser>(
      const X11SessionInfo &, std::stop_token)>;
#ifdef PUNTO_TESTING
  using Launcher = std::function<SoundLaunchResult(const SoundLaunchRequest &,
                                                   std::stop_token)>;
#endif

  struct SharedState {
    std::mutex mutex;
    std::condition_variable condition;
    bool enabled = true;
    bool accepting = false;
    bool finished = true;
    std::optional<int> pending_layout;
    X11SessionInfo session;
    Resolver resolver;
    std::array<LaunchRequest, 2> launch_requests;
    std::chrono::milliseconds minimum_launch_interval{100};
#ifdef PUNTO_TESTING
    Launcher test_launch;
#endif
  };

  Impl(const X11SessionInfo &session, bool session_valid,
       const SoundConfig &config, Resolver resolver, std::string player_path,
       bool drop_privileges, std::chrono::milliseconds minimum_launch_interval,
       std::chrono::milliseconds shutdown_wait)
      : state_{std::make_shared<SharedState>()},
        shutdown_wait_{std::clamp(shutdown_wait, 0ms, kShutdownWait)} {
    initialize(session, session_valid, config.enabled, std::move(resolver),
               std::move(player_path), drop_privileges,
               minimum_launch_interval);
  }

#ifdef PUNTO_TESTING
  Impl(const X11SessionInfo &session, const SoundConfig &config,
       SoundManagerTestOptions options)
      : state_{std::make_shared<SharedState>()},
        shutdown_wait_{std::clamp(options.shutdown_wait, 0ms, kShutdownWait)} {
    Resolver resolver = std::move(options.resolve_user);
    if (!resolver) {
      resolver = [resolved = std::move(options.resolved_user)](
                     const X11SessionInfo &,
                     std::stop_token) mutable -> std::optional<ResolvedUser> {
        return resolved;
      };
    }
    state_->test_launch = std::move(options.launch);
    initialize(session, options.session_valid, config.enabled,
               std::move(resolver), std::move(options.player_path),
               options.drop_privileges, options.minimum_launch_interval);
  }
#endif

  ~Impl() { shutdown(); }

  void initialize(const X11SessionInfo &session, bool session_valid,
                  bool enabled, Resolver resolver, std::string player_path,
                  bool drop_privileges,
                  std::chrono::milliseconds minimum_launch_interval) {
    state_->enabled = enabled;
    if (!session_valid) {
      std::cerr
          << "[punto] Sound: X11 session is unavailable; sound disabled\n";
      return;
    }
    if (!resolver || player_path.empty() ||
        !is_absolute_safe_path(player_path)) {
      std::cerr << "[punto] Sound: no trusted audio player; sound disabled\n";
      return;
    }
    state_->session = session;
    state_->resolver = std::move(resolver);
    state_->minimum_launch_interval = std::max(minimum_launch_interval, 0ms);
    state_->launch_requests[0] = LaunchRequest{
        .uid = 0,
        .gid = 0,
        .groups = {},
        .drop_privileges = drop_privileges,
        .player_path = std::move(player_path),
        .sound_path = std::string{kSoundRuToEn},
        .environment = {},
    };
    state_->launch_requests[1] = state_->launch_requests[0];
    state_->launch_requests[1].sound_path = std::string{kSoundEnToRu};
    state_->accepting = true;
    state_->finished = false;
    try {
      worker_.emplace(
          [state = state_](std::stop_token stop) { worker_loop(state, stop); });
    } catch (const std::exception &error) {
      state_->accepting = false;
      state_->finished = true;
      std::cerr << "[punto] Sound: failed to start worker: " << error.what()
                << '\n';
    }
  }

  void set_enabled(bool enabled) noexcept {
    {
      std::lock_guard lock(state_->mutex);
      state_->enabled = enabled;
      if (!state_->enabled) {
        state_->pending_layout.reset();
      }
    }
    state_->condition.notify_all();
  }

  void play_for_layout(int new_layout) noexcept {
    if (new_layout != 0 && new_layout != 1) {
      return;
    }
    {
      std::lock_guard lock(state_->mutex);
      if (!state_->accepting || !state_->enabled) {
        return;
      }
      state_->pending_layout = new_layout;
    }
    state_->condition.notify_one();
  }

  void shutdown() noexcept {
    std::optional<std::jthread> worker;
    const std::shared_ptr<SharedState> state = state_;
    {
      std::lock_guard lock(state->mutex);
      if (!worker_) {
        state->accepting = false;
        state->pending_layout.reset();
        return;
      }
      state->accepting = false;
      state->pending_layout.reset();
      worker_->request_stop();
      worker.swap(worker_);
    }
    state->condition.notify_all();

    std::unique_lock lock(state->mutex);
    const bool finished = state->condition.wait_for(
        lock, shutdown_wait_, [&] { return state->finished; });
    lock.unlock();
    if (finished && worker->joinable()) {
      worker->join();
    } else if (worker->joinable()) {
      worker->detach();
      std::cerr << "[punto] Sound: identity lookup did not stop; detached "
                   "from retired manager state\n";
    }
  }

  [[nodiscard]] static LaunchResult
  launch(const std::shared_ptr<SharedState> &state,
         const LaunchRequest &request, std::stop_token stop) {
#ifdef PUNTO_TESTING
    if (state->test_launch) {
      return state->test_launch(request, stop);
    }
#endif
    return run_process(request, kSetprivPath, kMaximumPlayerRuntime, stop);
  }

  static void worker_loop(const std::shared_ptr<SharedState> &state,
                          std::stop_token stop) noexcept {
    std::optional<ResolvedUser> resolved_user;
    bool resolver_failed = false;
    try {
      resolved_user = state->resolver(state->session, stop);
    } catch (const std::exception &) {
      resolver_failed = true;
    } catch (...) {
      resolver_failed = true;
    }

    std::optional<std::vector<std::string>> environment;
    if (resolved_user && !stop.stop_requested()) {
      environment = make_environment(state->session, *resolved_user);
    }

    std::unique_lock lock(state->mutex);
    if (stop.stop_requested() || !state->accepting || !resolved_user ||
        !environment) {
      if (!stop.stop_requested() && state->accepting) {
        std::cerr << (resolver_failed
                          ? "[punto] Sound: identity resolver failed; sound "
                            "disabled\n"
                          : "[punto] Sound: session credentials or environment "
                            "failed validation; sound disabled\n");
      }
      state->accepting = false;
      state->pending_layout.reset();
      state->finished = true;
      lock.unlock();
      state->condition.notify_all();
      return;
    }

    for (LaunchRequest &request : state->launch_requests) {
      request.uid = resolved_user->uid;
      request.gid = resolved_user->gid;
      request.groups = resolved_user->groups;
      request.environment = *environment;
    }
    auto next_launch = std::chrono::steady_clock::time_point::min();
    while (!stop.stop_requested()) {
      state->condition.wait(lock, [&] {
        return stop.stop_requested() || state->pending_layout.has_value();
      });
      if (stop.stop_requested()) {
        break;
      }
      if (!state->enabled) {
        state->pending_layout.reset();
        continue;
      }

      const auto now = std::chrono::steady_clock::now();
      if (now < next_launch) {
        state->condition.wait_until(lock, next_launch, [&] {
          return stop.stop_requested() || !state->enabled;
        });
        continue;
      }

      const int layout = state->pending_layout.value_or(-1);
      state->pending_layout.reset();
      if (layout < 0 ||
          static_cast<std::size_t>(layout) >= state->launch_requests.size()) {
        continue;
      }
      next_launch = now + state->minimum_launch_interval;
      const LaunchRequest &request =
          state->launch_requests[static_cast<std::size_t>(layout)];
      lock.unlock();

      LaunchResult result = LaunchResult::ExitedFailure;
      try {
        result = launch(state, request, stop);
      } catch (const std::exception &error) {
        std::cerr << "[punto] Sound: player runner failed: " << error.what()
                  << '\n';
      } catch (...) {
        std::cerr << "[punto] Sound: player runner failed with unknown error\n";
      }
      if (result != LaunchResult::Completed &&
          result != LaunchResult::Stopped) {
        std::cerr << "[punto] Sound: player failed or timed out\n";
      }
      lock.lock();
    }
    state->pending_layout.reset();
    state->accepting = false;
    state->finished = true;
    lock.unlock();
    state->condition.notify_all();
  }

  std::shared_ptr<SharedState> state_;
  std::optional<std::jthread> worker_;
  std::chrono::milliseconds shutdown_wait_;
};

#ifndef PUNTO_TESTING
SoundManager::SoundManager(const X11Session &x11_session,
                           const SoundConfig &config) {
  bool session_valid = x11_session.is_valid();
  const X11SessionInfo session = x11_session.info();
  std::string player;
  bool drop_privileges = true;

  if (session_valid) {
    player = select_player();
    const uid_t daemon_uid = ::geteuid();
    const gid_t daemon_gid = ::getegid();
    if (daemon_uid == 0) {
      if (!is_executable(kSetprivPath)) {
        session_valid = false;
        std::cerr << "[punto] Sound: /usr/bin/setpriv is unavailable; "
                     "sound disabled\n";
      }
    } else if (static_cast<std::uintmax_t>(daemon_uid) == session.uid &&
               static_cast<std::uintmax_t>(daemon_gid) == session.gid) {
      drop_privileges = false;
    } else {
      session_valid = false;
      std::cerr << "[punto] Sound: daemon credentials cannot enter the "
                   "desktop user context; sound disabled\n";
    }
  }

  Impl::Resolver resolver = [](const X11SessionInfo &snapshot,
                               std::stop_token stop) {
    if (stop.stop_requested()) {
      return std::optional<ResolvedUser>{};
    }
    return resolve_user(snapshot);
  };
  impl_ = std::make_unique<Impl>(
      session, session_valid, config, std::move(resolver), std::move(player),
      drop_privileges, kMinimumLaunchInterval, kShutdownWait);
}
#endif

SoundManager::~SoundManager() = default;

void SoundManager::set_enabled(bool enabled) noexcept {
  impl_->set_enabled(enabled);
}

void SoundManager::play_for_layout(int new_layout) noexcept {
  impl_->play_for_layout(new_layout);
}

#ifdef PUNTO_TESTING
SoundManager::SoundManager(const X11SessionInfo &session,
                           const SoundConfig &config,
                           SoundManagerTestOptions options)
    : impl_{std::make_unique<Impl>(session, config, std::move(options))} {}

std::optional<SoundManagerResolvedUser>
SoundManager::resolve_user_for_test(const X11SessionInfo &session) {
  return resolve_user(session);
}

SoundLaunchResult SoundManager::run_process_for_test(
    const SoundLaunchRequest &request, const std::string &helper_path,
    std::chrono::milliseconds maximum_runtime, std::stop_token stop) {
  return run_process(request, helper_path, maximum_runtime, stop);
}
#endif

} // namespace punto
