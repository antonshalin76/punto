/**
 * @file event_loop.cpp
 * @brief Реализация главного цикла обработки событий
 */

#include "punto/event_loop.hpp"
#include "punto/logger.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace punto {

namespace {

constexpr std::uint64_t kTaskIdFenceStride = 1024;
constexpr auto kControlPlanePollInterval = std::chrono::seconds{2};
constexpr auto kRuntimeShutdownDeadline = std::chrono::seconds{3};
constexpr auto kOutputWriteTimeout = std::chrono::seconds{2};

class ShutdownDeadlineGuard {
public:
  ShutdownDeadlineGuard(std::chrono::milliseconds timeout, const char *phase)
      : state_{std::make_shared<State>()}, watcher_{[state = state_, timeout] {
          std::unique_lock<std::mutex> lock{state->mutex};
          if (state->cv.wait_for(lock, timeout,
                                 [&state] { return state->complete; })) {
            return;
          }
          // The timed-out phase may own every runtime lock, including an
          // iostream or a full stderr pipe. _Exit is the only bounded action.
          std::_Exit(3);
        }} {}

  ShutdownDeadlineGuard(const ShutdownDeadlineGuard &) = delete;
  ShutdownDeadlineGuard &operator=(const ShutdownDeadlineGuard &) = delete;

  ~ShutdownDeadlineGuard() { complete(); }

  void complete() noexcept {
    if (!state_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock{state_->mutex};
      state_->complete = true;
    }
    state_->cv.notify_all();
    if (watcher_.joinable()) {
      watcher_.join();
    }
    state_.reset();
  }

private:
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool complete = false;
  };

  std::shared_ptr<State> state_;
  std::thread watcher_;
};

template <typename Function>
bool run_shutdown_phase(const char *name, Function &&function) noexcept {
  try {
    ShutdownDeadlineGuard deadline{kRuntimeShutdownDeadline, name};
    try {
      std::forward<Function>(function)();
      deadline.complete();
      return true;
    } catch (...) {
      deadline.complete();
      return false;
    }
  } catch (...) {
    // Diagnostics are deliberately omitted here: even write(2) can block on
    // a full pipe, defeating the shutdown guarantee.
    std::_Exit(3);
  }
}

enum class ReadEventStatus {
  Ok,
  Eof,
  Again,
  Truncated,
  Error,
};

ReadEventStatus
read_input_event(int fd, input_event &ev,
                 std::span<std::uint8_t, sizeof(input_event)> frame,
                 std::size_t &frame_size) {
  if (frame_size >= frame.size()) {
    frame_size = 0;
    errno = EOVERFLOW;
    return ReadEventStatus::Error;
  }

  const ssize_t n =
      ::read(fd, frame.data() + frame_size, frame.size() - frame_size);
  if (n > 0) {
    frame_size += static_cast<std::size_t>(n);
    if (frame_size != frame.size()) {
      return ReadEventStatus::Again;
    }
    std::memcpy(&ev, frame.data(), sizeof(ev));
    frame_size = 0;
    return ReadEventStatus::Ok;
  }
  if (n == 0) {
    if (frame_size == 0) {
      return ReadEventStatus::Eof;
    }
    frame_size = 0;
    return ReadEventStatus::Truncated;
  }
  if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    return ReadEventStatus::Again;
  }
  return ReadEventStatus::Error;
}

void drain_fd(int fd) {
  if (fd < 0) {
    return;
  }

  std::array<char, 32> buffer{};
  while (true) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n > 0) {
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

std::string trim_newline(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == '\0')) {
    value.pop_back();
  }
  return value;
}

bool is_numeric_component(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string read_proc_comm_bounded(const std::filesystem::path &path) {
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    return {};
  }
  std::array<char, 64> bytes{};
  const ssize_t count = ::read(fd, bytes.data(), bytes.size());
  const int saved_errno = errno;
  ::close(fd);
  errno = saved_errno;
  if (count <= 0 || static_cast<std::size_t>(count) == bytes.size()) {
    return {};
  }
  return trim_newline(
      std::string{bytes.data(), static_cast<std::size_t>(count)});
}

ConfigLoadOutcome config_load_failure(std::filesystem::path path,
                                      ConfigResult result,
                                      std::string message) {
  ConfigLoadOutcome outcome;
  outcome.used_path = std::move(path);
  outcome.result = result;
  outcome.error = std::move(message);
  return outcome;
}

ConfigLoadOutcome load_config_from_authorized_roots(
    const std::filesystem::path &system_root,
    const std::optional<std::filesystem::path> &user_root,
    const std::string &requested_path) {
  const std::filesystem::path system_path{std::string{kConfigPath}};
  if (!requested_path.empty()) {
    const std::filesystem::path requested{requested_path};
    const std::array<std::optional<std::filesystem::path>, 2> roots{system_root,
                                                                    user_root};
    for (const auto &root : roots) {
      if (!root) {
        continue;
      }
      RestrictedConfigLoadOutcome candidate =
          load_config_beneath_checked(*root, requested);
      if (!candidate.path_allowed) {
        continue;
      }
      if (candidate.load.result == ConfigResult::FileNotFound) {
        return config_load_failure(requested, ConfigResult::InvalidValue,
                                   "Invalid path");
      }
      return std::move(candidate.load);
    }
    return config_load_failure(requested, ConfigResult::InvalidValue,
                               "Invalid path");
  }

  if (user_root) {
    RestrictedConfigLoadOutcome user =
        load_config_beneath_checked(*user_root, *user_root / "config.yaml");
    if (!user.path_allowed) {
      return config_load_failure(*user_root / "config.yaml",
                                 ConfigResult::InvalidValue, "Invalid path");
    }
    if (user.load.result != ConfigResult::FileNotFound) {
      return std::move(user.load);
    }
  }

  RestrictedConfigLoadOutcome system =
      load_config_beneath_checked(system_root, system_path);
  if (!system.path_allowed) {
    return config_load_failure(system_path, ConfigResult::InvalidValue,
                               "Invalid path");
  }
  return std::move(system.load);
}

bool path_is_beneath(const std::filesystem::path &path,
                     const std::filesystem::path &root) {
  const std::filesystem::path relative =
      path.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != "..";
}

bool same_config_authority(const X11SessionInfo &left,
                           const X11SessionInfo &right) noexcept {
  return left.session_id == right.session_id &&
         left.username == right.username && left.uid == right.uid &&
         left.gid == right.gid && left.home_dir == right.home_dir &&
         left.xdg_config_home == right.xdg_config_home;
}

DictionaryLoadOutcome load_system_dictionary() {
  DictionaryLoadOutcome outcome;
  auto dictionary = std::make_unique<Dictionary>();
  outcome.result =
      dictionary->initialize_bounded(DictionaryLoadSpec::system_default());
  if (outcome.result == DictionaryLoadResult::Ok) {
    outcome.dictionary = std::move(dictionary);
  }
  return outcome;
}

const char *dictionary_load_result_name(DictionaryLoadResult result) noexcept {
  switch (result) {
  case DictionaryLoadResult::Ok:
    return "ok";
  case DictionaryLoadResult::NoUsableSource:
    return "no-source";
  case DictionaryLoadResult::Oversize:
    return "oversize";
  case DictionaryLoadResult::Malformed:
    return "malformed";
  case DictionaryLoadResult::IoError:
    return "io-error";
  }
  return "unknown";
}

} // namespace

std::size_t event_loop_detail::count_running_punto_daemons(
    const std::filesystem::path &proc_root, std::size_t max_numeric_candidates,
    std::chrono::milliseconds time_budget) {
  const std::string self_comm = read_proc_comm_bounded(proc_root / "self/comm");
  const auto deadline = std::chrono::steady_clock::now() + time_budget;
  const std::size_t conservative_fallback = std::max<std::size_t>(
      std::thread::hardware_concurrency(), static_cast<unsigned int>(1));
  std::size_t candidates = 0;
  std::size_t count = 0;

  std::error_code error;
  for (const auto &entry :
       std::filesystem::directory_iterator(proc_root, error)) {
    if (error) {
      return std::max(count, conservative_fallback);
    }
    const std::string pid_name = entry.path().filename().string();
    if (!is_numeric_component(pid_name)) {
      continue;
    }
    if (candidates >= max_numeric_candidates ||
        std::chrono::steady_clock::now() >= deadline) {
      return std::max(count, conservative_fallback);
    }
    ++candidates;

    const std::string comm = read_proc_comm_bounded(entry.path() / "comm");
    if (comm == "punto-daemon" || (!self_comm.empty() && comm == self_comm)) {
      ++count;
    }
  }
  if (error) {
    return std::max(count, conservative_fallback);
  }
  return std::max<std::size_t>(count, 1);
}

EventLoop::EventLoop(Config config, X11Session::ProbeFunction x11_probe,
                     ConfigLoaderFunction config_loader,
                     DictionaryLoaderFunction dictionary_loader)
    : config_{std::make_shared<Config>(std::move(config))},
      x11_session_{std::make_unique<X11Session>(std::move(x11_probe))},
      config_loader_state_{std::make_shared<ConfigLoaderState>()},
      dictionary_loader_state_{std::make_shared<DictionaryLoaderState>()} {
  config_loader_state_->loader =
      config_loader ? std::move(config_loader)
                    : ConfigLoaderFunction{load_config_from_authorized_roots};
  dictionary_loader_state_->loader =
      dictionary_loader ? std::move(dictionary_loader)
                        : DictionaryLoaderFunction{load_system_dictionary};
}

EventLoop::~EventLoop() { shutdown_runtime(); }

void EventLoop::set_stop_signal_fd(int fd) noexcept { stop_signal_fd_ = fd; }

bool EventLoop::start_config_loader() noexcept {
  if (config_loader_thread_.joinable()) {
    return true;
  }
  try {
    const auto state = config_loader_state_;
    config_loader_thread_ = std::thread{[state] {
      while (true) {
        ConfigLoadTask task;
        {
          std::unique_lock<std::mutex> lock{state->mutex};
          state->condition.wait(lock, [&] {
            return state->stop_requested || state->request.has_value();
          });
          if (state->stop_requested) {
            state->exited = true;
            state->condition.notify_all();
            return;
          }
          task = std::move(*state->request);
          state->request.reset();
        }

        ConfigLoadOutcome outcome;
        bool used_promotion_fallback = false;
        try {
          outcome = state->loader(task.system_root, task.user_root,
                                  task.requested_path);
          if (task.promotion_reconciliation && !task.requested_path.empty() &&
              outcome.result != ConfigResult::Ok) {
            used_promotion_fallback = true;
            outcome = state->loader(task.system_root, task.user_root, {});
          }
        } catch (...) {
          outcome =
              config_load_failure(task.requested_path, ConfigResult::IoError,
                                  "Config loader failure");
        }

        {
          std::lock_guard<std::mutex> lock{state->mutex};
          if (state->stop_requested) {
            state->exited = true;
            state->condition.notify_all();
            return;
          }
          state->completion = ConfigLoadCompletion{
              std::move(task), std::move(outcome), used_promotion_fallback};
        }
        state->condition.notify_all();
      }
    }};
    return true;
  } catch (...) {
    return false;
  }
}

bool EventLoop::stop_config_loader(std::chrono::milliseconds timeout) noexcept {
  const auto state = config_loader_state_;
  if (!state || !config_loader_thread_.joinable()) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock{state->mutex};
    state->stop_requested = true;
    state->request.reset();
  }
  state->condition.notify_all();

  bool exited = false;
  {
    std::unique_lock<std::mutex> lock{state->mutex};
    exited =
        state->condition.wait_for(lock, timeout, [&] { return state->exited; });
  }
  if (exited) {
    config_loader_thread_.join();
  } else {
    config_loader_thread_.detach();
  }
  return exited;
}

bool EventLoop::start_dictionary_loader() noexcept {
  if (dictionary_loader_thread_.joinable()) {
    return true;
  }
  try {
    const auto state = dictionary_loader_state_;
    dictionary_load_pending_ = true;
    dictionary_loader_thread_ = std::thread{[state] {
      DictionaryLoadOutcome outcome;
      try {
        outcome = state->loader();
      } catch (...) {
        outcome.result = DictionaryLoadResult::IoError;
      }

      {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (!state->stop_requested) {
          state->completion = std::move(outcome);
        }
        state->exited = true;
      }
      state->condition.notify_all();
    }};
    return true;
  } catch (...) {
    dictionary_load_pending_ = false;
    return false;
  }
}

bool EventLoop::stop_dictionary_loader(
    std::chrono::milliseconds timeout) noexcept {
  const auto state = dictionary_loader_state_;
  if (!state || !dictionary_loader_thread_.joinable()) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock{state->mutex};
    state->stop_requested = true;
  }
  state->condition.notify_all();

  bool exited = false;
  {
    std::unique_lock<std::mutex> lock{state->mutex};
    exited =
        state->condition.wait_for(lock, timeout, [&] { return state->exited; });
  }
  if (exited) {
    dictionary_loader_thread_.join();
  } else {
    dictionary_loader_thread_.detach();
  }
  return exited;
}

void EventLoop::poll_dictionary_load_completion() {
  std::optional<DictionaryLoadOutcome> completion;
  {
    std::lock_guard<std::mutex> lock{dictionary_loader_state_->mutex};
    if (dictionary_loader_state_->completion) {
      completion = std::move(dictionary_loader_state_->completion);
      dictionary_loader_state_->completion.reset();
    }
  }
  if (!completion) {
    return;
  }

  dictionary_load_pending_ = false;
  if (completion->result != DictionaryLoadResult::Ok ||
      !completion->dictionary || !completion->dictionary->is_ready()) {
    const DictionaryLoadResult failure =
        completion->result == DictionaryLoadResult::Ok
            ? DictionaryLoadResult::IoError
            : completion->result;
    analysis_health_.fail();
    exit_code_ = exit_code_ == 0 ? 2 : exit_code_;
    std::cerr << "[punto] FATAL: dictionary initialization failed: "
              << dictionary_load_result_name(failure) << "\n";
    request_stop();
    return;
  }

  dictionary_ =
      std::shared_ptr<const Dictionary>{std::move(completion->dictionary)};
  analysis_pool_ = std::make_unique<AnalysisWorkerPool>(*dictionary_);
  analysis_pool_->start(analysis_thread_budget_.worker_threads);
  analysis_health_.mark_progress();
  std::cerr << "[punto] Analysis pool ready with dictionary\n";
}

bool EventLoop::initialize() {
  if (initialized_) {
    return true;
  }

  if (!start_config_loader()) {
    exit_code_ = 2;
    return false;
  }

  if (!start_dictionary_loader()) {
    exit_code_ = 2;
    analysis_health_.fail();
    return false;
  }

  const int output_flags = ::fcntl(STDOUT_FILENO, F_GETFL);
  if (output_flags < 0 ||
      (!(output_flags & O_NONBLOCK) &&
       ::fcntl(STDOUT_FILENO, F_SETFL, output_flags | O_NONBLOCK) != 0)) {
    exit_code_ = 3;
    return false;
  }

  // Session/account discovery may enter NSS. It always starts on the bounded
  // background refresh lane; keyboard passthrough and shutdown stay live.
  x11_session_->start_background_refresh();
  x11_refresh_pending_ = true;
  x11_health_.degrade();

  const bool control_plane_lease_acquired = control_plane_lease_.try_acquire();
  const bool control_plane_ready = control_plane_lease_acquired &&
                                   reconcile_control_plane_before_promotion();
  control_plane_primary_.store(control_plane_ready, std::memory_order_release);
  if (control_plane_ready) {
    std::cerr << "[punto] Control plane role: primary\n";
  } else if (control_plane_lease_acquired) {
    std::cerr << "[punto] Control plane role: promotion pending authoritative "
                 "state\n";
  } else {
    std::cerr << "[punto] Control plane role: secondary\n";
    sync_control_plane_from_shared_state(/*force=*/true);
  }

  // Compute the worker budget now; the pool starts only after the immutable
  // dictionary snapshot reaches the event-loop thread.
  {
    auto cfg = std::atomic_load(&config_);
    analysis_thread_budget_ = compute_analysis_thread_budget(
        std::thread::hardware_concurrency(),
        event_loop_detail::count_running_punto_daemons(),
        cfg->runtime.analysis_threads,
        cfg->runtime.max_analysis_threads_per_daemon);

    std::cerr << "[punto] Analysis pool budget: "
              << analysis_thread_budget_.worker_threads << " threads ("
              << (analysis_thread_budget_.manual_override ? "fixed" : "auto")
              << ", daemons=" << analysis_thread_budget_.daemon_count
              << ", max_per_daemon="
              << cfg->runtime.max_analysis_threads_per_daemon << ")\n";
  }

  std::cerr << "[punto] Предупреждение: X11-наблюдение пока недоступно "
               "(нет активной user-сессии или недоступен "
               "DISPLAY/XAUTHORITY).\n"
            << "[punto] Наблюдение автоматически перепривяжется после "
               "появления user-сессии.\n";
  const X11SessionInfo info = x11_session_->info();
  if (!wayland_warning_emitted_ && !info.wayland_display.empty()) {
    wayland_warning_emitted_ = true;
    std::cerr << "[punto] WARN: Wayland session detected, X11 layout "
                 "observation unavailable until X11 appears.\n";
  }

  if (control_plane_primary_.load(std::memory_order_acquire)) {
    if (!start_primary_ipc_server()) {
      std::cerr << "[punto] Warning: primary IPC server failed to start. "
                   "Tray control will be unavailable.\n";
    } else {
      publish_control_plane_state(/*bump_config_generation=*/true,
                                  /*bump_status_generation=*/true);
    }
  } else if (!start_primary_ipc_server()) {
    std::cerr << "[punto] Warning: secondary diagnostic IPC server failed "
                 "to start\n";
  } else {
    std::cerr << "[punto] Secondary daemon: primary control plus "
                 "instance-local diagnostic IPC available\n";
  }

  initialized_ = true;
  return true;
}

void EventLoop::request_stop() noexcept {
  stop_requested_.store(true, std::memory_order_relaxed);
}

int EventLoop::run() {
  if (!initialize()) {
    std::cerr << "[punto] Failed to initialize event loop\n";
    return exit_code_ != 0 ? exit_code_ : 2;
  }

  std::cerr << "[punto] Startup layout group: " << current_layout_ << "\n";

  input_event ev{};

  pollfd pfd{STDIN_FILENO, POLLIN, 0};

  // Время последней проверки/обновления X11-сессии.
  // Нужен для фикса "прилипание к GDM" на буте и корректной работы после
  // logout/login.
  auto last_x11_check_time = std::chrono::steady_clock::now();
  constexpr auto kX11CheckInterval = std::chrono::seconds{3};

  bool x11_wait_log_emitted = false;

  auto rebuild_x11_deps = [&]() {
    // A GUI-session change may alter the authorized ~/.config source. Keep a
    // generation-owned intent when another load is still in flight so the
    // latest session is re-read after that obsolete work completes.
    request_x11_config_reload();

    {
      const X11SessionInfo info = x11_session_->info();
      if (info.observed_keyboard_layout == 0 ||
          info.observed_keyboard_layout == 1) {
        current_layout_ = info.observed_keyboard_layout;
      }
      std::cerr << "[punto] X11 session: id=" << info.session_id
                << " user=" << info.username << " display=" << info.display
                << "\n";
    }

    std::cerr << "[punto] X11 observation refreshed, layout: "
              << (current_layout_ == 0 ? "EN" : "RU") << "\n";
    x11_dependencies_ready_ = true;
    wayland_warning_emitted_ = false;
  };

  auto teardown_x11_deps = [&]() {
    x11_dependencies_ready_ = false;
    ++x11_config_generation_;
    pending_x11_config_generation_.reset();
    // Keep the last observed layout for diagnostic analysis. The input path
    // performs no X11 operations and never changes the desktop layout.
  };

  // Главный цикл: проверяем флаг остановки на каждой итерации
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    poll_dictionary_load_completion();
    poll_config_load_completion();
    service_ipc_commands();
    poll_config_load_completion();
    observe_ipc_fatal();
    if (stop_requested_.load(std::memory_order_relaxed)) {
      break;
    }

    // Периодически проверяем, не сменилась ли активная user-сессия.
    // Используем фоновый refresh чтобы не блокировать обработку клавиатуры.
    {
      const auto now = std::chrono::steady_clock::now();

      if (last_control_plane_poll_.time_since_epoch().count() == 0 ||
          now - last_control_plane_poll_ >= kControlPlanePollInterval) {
        last_control_plane_poll_ = now;
        if (!control_plane_primary_.load(std::memory_order_acquire)) {
          maybe_promote_to_control_plane_primary();
          if (!control_plane_primary_.load(std::memory_order_acquire)) {
            sync_control_plane_from_shared_state(/*force=*/false);
          }
        } else if ((!ipc_server_ || !ipc_server_->is_running()) &&
                   (!ipc_server_ || !ipc_server_->fatal_reason().has_value())) {
          // Primary без IPC (например, при промоушене сокет ещё держал
          // умирающий процесс) — иначе управление из трея потеряно навсегда.
          if (start_primary_ipc_server()) {
            std::cerr << "[punto] Primary IPC server recovered\n";
            publish_control_plane_state(/*bump_config_generation=*/true,
                                        /*bump_status_generation=*/true);
          }
        }
      }

      // Запускаем фоновый refresh если пришло время и нет активного
      if (!x11_refresh_pending_ &&
          now - last_x11_check_time >= kX11CheckInterval) {
        x11_session_->start_background_refresh();
        x11_refresh_pending_ = true;
      }

      // Проверяем результат фонового refresh (неблокирующий poll)
      if (x11_refresh_pending_) {
        // A failed probe invalidates the session snapshot before the bounded
        // retry sequence finishes. A later healthy commit publishes a fresh
        // immutable observation snapshot.
        if (!x11_session_->is_valid() && x11_dependencies_ready_) {
          x11_health_.degrade();
          teardown_x11_deps();
        }
        auto result = x11_session_->poll_refresh_result();
        if (result.has_value()) {
          x11_refresh_pending_ = false;
          last_x11_check_time = now;

          if (*result == X11Session::RefreshResult::HealthyUpdated) {
            x11_health_.ready();
            x11_health_.mark_progress();
            rebuild_x11_deps();
          } else if (*result == X11Session::RefreshResult::HealthyUnchanged) {
            x11_health_.ready();
            x11_health_.mark_progress();
            if (!x11_dependencies_ready_) {
              rebuild_x11_deps();
            }
          } else if (*result == X11Session::RefreshResult::SessionAbsent) {
            x11_health_.degrade();
            teardown_x11_deps();
            std::cerr
                << "[punto] X11 session invalidated (no active user session)\n";
          } else {
            x11_health_.degrade();
            teardown_x11_deps();
            std::cerr << "[punto] X11 session probe failed; observation "
                         "remains unavailable until a healthy refresh\n";
          }
        }
      }
    }

    // Одноразовый лог "ждём user-сессию" (чтобы не спамить в journalctl).
    // На экране логина активна greeter-сессия (Class=greeter) и X11 контекст
    // пользователя ещё недоступен.
    if (!x11_session_->is_valid()) {
      if (!x11_wait_log_emitted) {
        x11_wait_log_emitted = true;
        std::cerr
            << "[punto] X11: активная пользовательская сессия не обнаружена "
               "(возможно экран логина). Ожидаю входа пользователя...\n";
      }
      const X11SessionInfo info = x11_session_->info();
      if (!wayland_warning_emitted_ && !info.wayland_display.empty()) {
        wayland_warning_emitted_ = true;
        std::cerr << "[punto] WARN: Wayland session detected, X11 layout "
                     "observation unavailable; analysis remains read-only.\n";
      }
    } else {
      x11_wait_log_emitted = false;
      wayland_warning_emitted_ = false;
    }

    // Drain completed analysis even while input is idle so health and
    // diagnostic counters stay current.
    process_ready_results();
    if (stop_requested_.load(std::memory_order_relaxed)) {
      break;
    }

    std::array<pollfd, 2> pfds{};
    nfds_t nfds = 1;
    pfds[0] = pfd;
    if (stop_signal_fd_ >= 0) {
      pfds[1] = pollfd{stop_signal_fd_, POLLIN, 0};
      nfds = 2;
    }

    int ret = poll(pfds.data(), nfds, 1); // 1ms тик

    if (ret > 0) {
      if (stop_signal_fd_ >= 0 && (pfds[1].revents & POLLIN)) {
        drain_fd(stop_signal_fd_);
        request_stop();
        continue;
      }

      pfd = pfds[0];

      // Проверяем флаги закрытия канала или ошибки.
      // POLLHUP возникает когда udevmon закрывает пайп (остановка сервиса).
      // Если установлен POLLIN вместе с POLLHUP, сначала читаем оставшиеся
      // данные.
      if ((pfd.revents & (POLLERR | POLLNVAL)) && !(pfd.revents & POLLIN)) {
        fail_input_pipeline();
        exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
        std::cerr << "[punto] stdin poll failure (revents=0x" << std::hex
                  << pfd.revents << std::dec << ")\n";
        break;
      }
      if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) {
        if (input_frame_size_ != 0 || !input_frame_accepts_.empty()) {
          fail_input_pipeline();
          exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
          std::cerr << "[punto] stdin closed with an incomplete input frame\n";
        } else {
          std::cerr << "[punto] stdin closed, exiting gracefully\n";
        }
        break;
      }

      if (pfd.revents & POLLIN) {
        switch (read_input_event(STDIN_FILENO, ev, input_frame_bytes_,
                                 input_frame_size_)) {
        case ReadEventStatus::Ok:
          note_input_event_accepted(ev);
          handle_event(ev);
          note_input_event_committed(ev);
          process_ready_results();
          continue;
        case ReadEventStatus::Again:
          continue;
        case ReadEventStatus::Eof:
          if (!input_frame_accepts_.empty()) {
            fail_input_pipeline();
            exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
            std::cerr
                << "[punto] stdin closed with an incomplete input frame\n";
          } else {
            std::cerr << "[punto] stdin closed, exiting gracefully\n";
          }
          stop_requested_.store(true, std::memory_order_relaxed);
          continue;
        case ReadEventStatus::Truncated:
          fail_input_pipeline();
          exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
          std::cerr << "[punto] stdin closed with an incomplete input_event\n";
          stop_requested_.store(true, std::memory_order_relaxed);
          continue;
        case ReadEventStatus::Error:
        default:
          fail_input_pipeline();
          exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
          std::cerr << "[punto] stdin read failed: " << std::strerror(errno)
                    << "\n";
          stop_requested_.store(true, std::memory_order_relaxed);
          continue;
        }
      }
    }

    if (ret == 0) {
      continue; // timeout
    }

    if (ret < 0) {
      if (errno == EINTR) {
        // Сигнал прервал poll — проверяем флаг остановки на следующей итерации
        continue;
      }
      exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
      fail_input_pipeline();
      std::cerr << "[punto] poll failed: " << std::strerror(errno) << "\n";
      break;
    }
  }

  shutdown_runtime();
  std::cerr << "[punto] Event loop terminated gracefully\n";
  return exit_code_;
}

void EventLoop::emit_passthrough_event(const input_event &ev) {
  const auto *next = reinterpret_cast<const std::uint8_t *>(&ev);
  std::size_t remaining = sizeof(ev);
  const auto deadline = std::chrono::steady_clock::now() + kOutputWriteTimeout;

  while (remaining > 0) {
    const ssize_t written = ::write(STDOUT_FILENO, next, remaining);
    if (written > 0) {
      next += static_cast<std::size_t>(written);
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const auto now = std::chrono::steady_clock::now();
      if (now < deadline) {
        auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        if (timeout.count() == 0) {
          timeout = std::chrono::milliseconds{1};
        }
        pollfd descriptor{STDOUT_FILENO, POLLOUT, 0};
        const int ready =
            ::poll(&descriptor, 1,
                   static_cast<int>(std::min<std::int64_t>(
                       timeout.count(), std::numeric_limits<int>::max())));
        if (ready > 0 && (descriptor.revents & POLLOUT)) {
          continue;
        }
        if (ready < 0 && errno == EINTR) {
          continue;
        }
      }
    }

    fail_input_pipeline();
    exit_code_ = exit_code_ == 0 ? 3 : exit_code_;
    request_stop();
    return;
  }
}

void EventLoop::note_input_event_accepted(const input_event &event) {
  if (!input_read_frame_started_) {
    const auto accepted_at = std::chrono::steady_clock::now();
    input_frame_accepts_.push_back(accepted_at);
    if (input_frame_accepts_.size() == 1) {
      input_health_.begin(accepted_at);
    }
    input_read_frame_started_ = true;
  }

  if (event.type == EV_SYN && event.code == SYN_REPORT) {
    input_read_frame_started_ = false;
  }
}

void EventLoop::note_input_event_committed(const input_event &event) {
  if (event.type != EV_SYN || event.code != SYN_REPORT) {
    return;
  }
  if (input_frame_accepts_.empty()) {
    fail_input_pipeline();
    return;
  }

  input_frame_accepts_.pop_front();
  input_health_.mark_progress();
  if (input_frame_accepts_.empty()) {
    input_health_.clear_in_flight();
  } else {
    input_health_.begin(input_frame_accepts_.front());
  }
}

void EventLoop::fail_input_pipeline() noexcept { input_health_.fail(); }

void EventLoop::handle_event(const input_event &ev) {
  if (ev.type != EV_KEY) {
    emit_passthrough_event(ev);
    return;
  }

  const ScanCode code = ev.code;
  const bool is_release = ev.value == 0;
  const bool is_repeat = ev.value == 2;

  // Text mutation is intentionally unavailable until an application-bound
  // acknowledgement exists. Swallow every Pause event independently so a
  // release-only or repeat-only frame after restart cannot leak downstream.
  if (code == KEY_PAUSE) {
    return;
  }

  if (is_modifier(code)) {
    update_modifier_state(code, !is_release);
    emit_passthrough_event(ev);
    return;
  }

  if (is_release) {
    emit_passthrough_event(ev);
    return;
  }

  if (is_repeat && (code == KEY_SPACE || code == KEY_TAB)) {
    emit_passthrough_event(ev);
    return;
  }

  auto cfg = std::atomic_load(&config_);

  if (modifiers_.any_ctrl() || modifiers_.any_alt() || modifiers_.any_meta()) {
    reset_async_state();
    buffer_.reset_current();
    emit_passthrough_event(ev);
    return;
  }

  if (code == KEY_BACKSPACE) {
    buffer_.pop_char();
    emit_passthrough_event(ev);
    return;
  }

  if (code == KEY_SPACE || code == KEY_TAB) {
    const auto full_word = buffer_.current_word();
    std::span<const KeyEntry> analysis_word = full_word;
    while (!analysis_word.empty()) {
      const ScanCode last = analysis_word.back().code;
      if (last == KEY_DOT || last == KEY_COMMA || last == KEY_SEMICOLON ||
          last == KEY_APOSTROPHE || last == KEY_SLASH || last == KEY_MINUS) {
        analysis_word = analysis_word.first(analysis_word.size() - 1);
      } else {
        break;
      }
    }

    // Physical input is committed before all analysis bookkeeping and never
    // waits on X11 or configuration I/O.
    emit_passthrough_event(ev);
    buffer_.commit_word();
    if (!buffer_.push_trailing(code)) {
      reset_async_state();
    }
    if (full_word.empty()) {
      return;
    }

    const std::uint64_t task_id = next_task_id_;
    if (!analysis_pool_ || !cfg->auto_switch.enabled ||
        analysis_word.size() < cfg->auto_switch.min_word_len) {
      WordResult result;
      result.task_id = task_id;
      result.word_len = full_word.size();
      result.analysis_len = analysis_word.size();
      result.layout_at_boundary = current_layout_;
      ready_results_[task_id] = result;
      analysis_accepted_at_[task_id] = std::chrono::steady_clock::now();
      lifetime_telemetry_.ready_results.store(ready_results_.size(),
                                              std::memory_order_relaxed);
      ++next_task_id_;
      refresh_analysis_health_head();
      return;
    }

    WordTask task;
    task.task_id = task_id;
    task.word.assign(full_word.begin(), full_word.end());
    task.analysis_len = analysis_word.size();
    task.layout_at_boundary = current_layout_;
    task.cfg = cfg->auto_switch;
    const AnalysisAdmission admission = analysis_pool_->submit(std::move(task));
    if (!admission.accepted) {
      return;
    }
    analysis_accepted_at_[task_id] = admission.accepted_at;
    ++next_task_id_;
    refresh_analysis_health_head();
    return;
  }

  if (code == KEY_DOT || code == KEY_COMMA || code == KEY_SEMICOLON ||
      code == KEY_APOSTROPHE || code == KEY_SLASH || code == KEY_MINUS) {
    emit_passthrough_event(ev);
    const bool was_overflowed = buffer_.current_overflowed();
    if (!buffer_.push_char(code, modifiers_.any_shift()) && !was_overflowed) {
      reset_async_state();
    }
    return;
  }

  if (code == KEY_ENTER || code == KEY_KPENTER) {
    buffer_.reset_all();
    reset_async_state();
    emit_passthrough_event(ev);
    return;
  }

  if (is_letter_key(code)) {
    const bool was_overflowed = buffer_.current_overflowed();
    if (!buffer_.push_char(code, modifiers_.any_shift()) && !was_overflowed) {
      reset_async_state();
    }
    emit_passthrough_event(ev);
    return;
  }

  if (is_navigation_key(code)) {
    buffer_.reset_all();
    reset_async_state();
    emit_passthrough_event(ev);
    return;
  }

  if (is_function_key(code)) {
    emit_passthrough_event(ev);
    return;
  }

  buffer_.reset_current();
  reset_async_state();
  emit_passthrough_event(ev);
}
void EventLoop::update_modifier_state(ScanCode code, bool pressed) {
  switch (code) {
  case KEY_LEFTSHIFT:
    modifiers_.left_shift = pressed;
    break;
  case KEY_RIGHTSHIFT:
    modifiers_.right_shift = pressed;
    break;
  case KEY_LEFTCTRL:
    modifiers_.left_ctrl = pressed;
    break;
  case KEY_RIGHTCTRL:
    modifiers_.right_ctrl = pressed;
    break;
  case KEY_LEFTALT:
    modifiers_.left_alt = pressed;
    break;
  case KEY_RIGHTALT:
    modifiers_.right_alt = pressed;
    break;
  case KEY_LEFTMETA:
    modifiers_.left_meta = pressed;
    break;
  case KEY_RIGHTMETA:
    modifiers_.right_meta = pressed;
    break;
  default:
    break;
  }
}

void EventLoop::reset_async_state(bool bump_task_barrier) {
  if (analysis_pool_) {
    analysis_pool_->begin_new_epoch();
  }
  ready_results_.clear();
  analysis_accepted_at_.clear();
  analysis_health_.clear_in_flight();
  analysis_health_.mark_progress();

  if (bump_task_barrier) {
    const std::uint64_t fence =
        std::max(next_task_id_, next_apply_task_id_) + kTaskIdFenceStride;
    next_task_id_ = fence;
    next_apply_task_id_ = fence;
  } else {
    next_apply_task_id_ = next_task_id_;
  }

  lifetime_telemetry_.ready_results.store(0, std::memory_order_relaxed);
}

void EventLoop::refresh_analysis_health_head() {
  if (analysis_pool_failed_) {
    analysis_health_.fail();
    return;
  }
  if (!analysis_pool_) {
    analysis_health_.clear_in_flight();
    return;
  }

  const auto head = analysis_accepted_at_.find(next_apply_task_id_);
  if (head == analysis_accepted_at_.end()) {
    analysis_health_.clear_in_flight();
    return;
  }
  analysis_health_.begin(head->second);
}

void EventLoop::commit_analysis_terminal(std::uint64_t task_id) {
  analysis_accepted_at_.erase(task_id);
  ++next_apply_task_id_;
  analysis_health_.mark_progress();
  refresh_analysis_health_head();
}

bool EventLoop::start_primary_ipc_server() {
  const bool primary = control_plane_primary_.load(std::memory_order_acquire);
  if (ipc_server_) {
    if (ipc_server_->is_running()) {
      if (!primary || !ipc_server_is_fallback_) {
        return true;
      }
      ipc_server_->stop();
      ipc_server_.reset();
      ipc_server_is_fallback_ = false;
    }
    if (ipc_server_ && ipc_server_->fatal_reason().has_value()) {
      return false;
    }
    if (ipc_server_) {
      return ipc_server_->start();
    }
  }

  IpcServerOptions options;
  if (primary) {
    options.primary_socket_path = kIpcSocketPath;
  } else {
    const std::filesystem::path primary_path{kIpcSocketPath};
    options.primary_socket_path =
        (primary_path.parent_path() /
         (primary_path.stem().string() + "-" + std::to_string(::getpid()) +
          primary_path.extension().string()))
            .string();
    ipc_server_is_fallback_ = true;
    options.endpoint_mode = IpcEndpointMode::DiagnosticReadOnly;
  }
  options.allow_fallback_sockets = false;
  ipc_server_ = std::make_unique<IpcServer>(ipc_mailbox_, std::move(options));

  return ipc_server_->start();
}

IpcResult EventLoop::execute_ipc_command(const IpcRequest &request) {
  switch (request.verb) {
  case IpcVerb::GetStatus:
    return {true, "DISABLED"};
  case IpcVerb::SetStatus: {
    if (request.argument != "0" && request.argument != "1") {
      return {false, "Invalid status"};
    }
    if (request.argument == "1") {
      return {false, "Text mutation disabled"};
    }
    return {true, "DISABLED"};
  }
  case IpcVerb::Reload:
    return reload_config(request.argument);
  case IpcVerb::Stats:
    return stats_report();
  case IpcVerb::Shutdown:
    return {false, "Shutdown not allowed via IPC"};
  }
  return {false, "Unknown command"};
}

void EventLoop::service_ipc_commands() noexcept {
  if (!ipc_mailbox_) {
    return;
  }

  while (auto pending = ipc_mailbox_->try_dequeue()) {
    IpcResult response;
    try {
      response = execute_ipc_command(pending->request);
    } catch (...) {
      response = {false, "Internal failure"};
      exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
      request_stop();
    }

    try {
      pending->complete(std::move(response));
    } catch (...) {
      exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
      request_stop();
    }
  }
}

void EventLoop::cancel_ipc_commands_for_shutdown() noexcept {
  if (!ipc_mailbox_) {
    return;
  }
  while (auto pending = ipc_mailbox_->try_dequeue()) {
    try {
      pending->complete({false, "Shutting down"});
    } catch (...) {
      exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
    }
  }
}

void EventLoop::observe_ipc_fatal() noexcept {
  if (!ipc_server_) {
    return;
  }
  const auto fatal = ipc_server_->fatal_reason();
  if (!fatal.has_value()) {
    return;
  }
  if (!ipc_fatal_reported_) {
    ipc_fatal_reported_ = true;
    std::cerr << "[punto] Fatal IPC server failure: reason="
              << static_cast<int>(*fatal) << "\n";
  }
  exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
  request_stop();
}

void EventLoop::shutdown_runtime() noexcept {
  if (runtime_shutdown_started_) {
    return;
  }
  runtime_shutdown_started_ = true;
  try {
    if (analysis_pool_) {
      analysis_pool_->close_admission();
    }
  } catch (...) {
    exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
  }

  if (ipc_mailbox_ && !run_shutdown_phase("ipc-admission", [this] {
        if (!ipc_mailbox_->close(std::chrono::milliseconds{2800})) {
          std::_Exit(3);
        }
        // Do not execute state-changing commands after shutdown owns
        // admission. Complete the bounded mailbox with an explicit terminal
        // response so the poller can finish its response/descriptor barrier.
        cancel_ipc_commands_for_shutdown();
      })) {
    std::_Exit(3);
  }

  if (ipc_server_ &&
      !run_shutdown_phase("ipc-poller", [this] { ipc_server_->stop(); })) {
    std::_Exit(3);
  }
  observe_ipc_fatal();

  if (!run_shutdown_phase("config-loader", [this] {
        if (!stop_config_loader(std::chrono::milliseconds{2800})) {
          // The loader may be blocked in a filesystem implementation and can
          // still execute yaml-cpp/libc code. Avoid concurrent static teardown.
          std::_Exit(3);
        }
      })) {
    std::_Exit(3);
  }

  if (!run_shutdown_phase("dictionary-loader", [this] {
        if (!stop_dictionary_loader(std::chrono::milliseconds{2800})) {
          std::_Exit(3);
        }
      })) {
    std::_Exit(3);
  }

  if (x11_session_ && !run_shutdown_phase("x11-refresh", [this] {
        if (!x11_session_->shutdown_background_refresh(
                std::chrono::milliseconds{2800})) {
          // A timed-out probe was detached and may still run process-static
          // library code. Do not execute destructors concurrently with it.
          std::_Exit(3);
        }
      })) {
    std::_Exit(3);
  }

  if (analysis_pool_ && !run_shutdown_phase("analysis-workers", [this] {
        analysis_pool_->stop();
      })) {
    std::_Exit(3);
  }
  try {
    WordResult terminal;
    if (analysis_pool_) {
      while (analysis_pool_->try_pop_result(terminal)) {
        analysis_accepted_at_.erase(terminal.task_id);
      }
    }
    analysis_accepted_at_.clear();
    analysis_health_.clear_in_flight();
  } catch (...) {
    exit_code_ = (exit_code_ == 0) ? 3 : exit_code_;
  }
  ipc_server_.reset();
}

void EventLoop::publish_control_plane_state(bool bump_config_generation,
                                            bool bump_status_generation) {
  if (!control_plane_primary_.load(std::memory_order_acquire)) {
    return;
  }
  auto cfg = std::atomic_load(&config_);

  // Мьютекс обязателен: publish вызывается и из main-потока (initialize,
  // failover, X11 refresh -> reload_config), и из IPC-потока
  // (RELOAD/SET_STATUS callbacks).
  std::lock_guard<std::mutex> lock(control_plane_mutex_);

  SharedControlPlaneState next = shared_control_plane_state_;
  next.enabled = false;
  next.config_path = cfg ? cfg->config_path.string() : std::string{};

  if ((bump_config_generation &&
       next.config_generation == std::numeric_limits<std::uint64_t>::max()) ||
      (bump_status_generation &&
       next.status_generation == std::numeric_limits<std::uint64_t>::max())) {
    std::cerr << "[punto] Warning: control-plane generation exhausted\n";
    return;
  }
  if (bump_config_generation) {
    next.config_generation += 1;
  }
  if (bump_status_generation) {
    next.status_generation += 1;
  }

  if (!write_shared_control_plane_state(next)) {
    std::cerr << "[punto] Warning: failed to publish control-plane state\n";
    return;
  }

  shared_control_plane_state_ = next;
  applied_config_generation_ = next.config_generation;
  applied_status_generation_ = next.status_generation;
}

bool EventLoop::reconcile_control_plane_before_promotion() {
  // The lease is already held here, so a previous primary can no longer
  // publish. Keep the externally visible role secondary until its last safe
  // snapshot has actually been applied; otherwise a stale daemon can reuse the
  // same generation and permanently split its peers.
  if (config_load_pending_) {
    return false;
  }

  SharedControlPlaneState authoritative;
  if (!read_shared_control_plane_state(authoritative)) {
    return true;
  }

  std::uint64_t applied_config = 0;
  {
    std::lock_guard<std::mutex> lock(control_plane_mutex_);
    shared_control_plane_state_ = authoritative;
    applied_config = applied_config_generation_;
    applied_status_generation_ = authoritative.status_generation;
  }

  const auto cfg = std::atomic_load(&config_);
  const std::string current_config_path =
      cfg ? cfg->config_path.string() : std::string{};
  const std::filesystem::path system_root{"/etc/punto"};
  std::optional<std::filesystem::path> user_root;
  if (x11_session_) {
    auto session_lease = x11_session_->acquire_write_lease();
    if (session_lease) {
      const X11SessionInfo &info = session_lease->info();
      if (!info.xdg_config_home.empty()) {
        user_root = std::filesystem::path{info.xdg_config_home} / "punto";
      } else if (!info.home_dir.empty()) {
        user_root = std::filesystem::path{info.home_dir} / ".config" / "punto";
      }
    }
  }
  const auto path_allowed = [&system_root,
                             &user_root](const std::string &raw_path) {
    if (raw_path.empty()) {
      return true;
    }
    const std::filesystem::path path{raw_path};
    if (!path.is_absolute()) {
      return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    return path_is_beneath(normalized, system_root) ||
           (user_root && path_is_beneath(normalized, *user_root));
  };
  const bool authoritative_path_allowed =
      path_allowed(authoritative.config_path);
  const bool current_path_allowed =
      !current_config_path.empty() && path_allowed(current_config_path);
  const bool authority_fallback_applied =
      promotion_fallback_applied_generation_ &&
      *promotion_fallback_applied_generation_ ==
          authoritative.config_generation;
  const ControlPlanePromotionAction action = plan_control_plane_promotion(
      authoritative, applied_config, current_config_path,
      authoritative_path_allowed, current_path_allowed,
      authority_fallback_applied);
  if (action == ControlPlanePromotionAction::Ready) {
    return true;
  }

  const std::string reload_path =
      action == ControlPlanePromotionAction::ReloadAuthoritativePath
          ? authoritative.config_path
          : std::string{};
  const IpcResult reload =
      reload_config(reload_path, authoritative.config_generation, std::nullopt,
                    /*promotion_reconciliation=*/true);
  if (!reload.success) {
    std::cerr << "[punto] Warning: control-plane promotion reconciliation "
                 "deferred: "
              << reload.message << "\n";
  }
  return false;
}

void EventLoop::sync_control_plane_from_shared_state(bool force) {
  if (control_plane_primary_.load(std::memory_order_acquire)) {
    return;
  }

  SharedControlPlaneState next;
  if (!read_shared_control_plane_state(next)) {
    return;
  }

  std::uint64_t applied_config = 0;
  std::uint64_t applied_status = 0;
  {
    std::lock_guard<std::mutex> lock(control_plane_mutex_);
    applied_config = applied_config_generation_;
    applied_status = applied_status_generation_;
  }

  // Сравнение через != (а не >): после рестарта primary нумерация может
  // начаться заново, и «меньшее» поколение — это тоже изменение, которое
  // нужно применить, иначе демоны разных клавиатур разойдутся по настройкам.
  const bool config_changed = force || next.config_generation != applied_config;
  const bool status_changed = force || next.status_generation != applied_status;

  if (config_changed) {
    IpcResult res = reload_config(next.config_path, next.config_generation);
    if (!res.success) {
      std::cerr << "[punto] Warning: shared control-plane config sync failed: "
                << res.message << "\n";
      return;
    }
  }

  std::lock_guard<std::mutex> lock(control_plane_mutex_);
  if (status_changed) {
    applied_status_generation_ = next.status_generation;
  }
  shared_control_plane_state_ = next;
}

void EventLoop::maybe_promote_to_control_plane_primary() {
  if (control_plane_primary_.load(std::memory_order_acquire)) {
    return;
  }

  if (!control_plane_lease_.try_acquire()) {
    return;
  }

  if (!reconcile_control_plane_before_promotion()) {
    return;
  }

  control_plane_primary_.store(true, std::memory_order_release);
  std::cerr << "[punto] Control plane failover: promoted to primary\n";
  if (!start_primary_ipc_server()) {
    // Не фатально: main loop будет ретраить запуск IPC в control-plane poll.
    std::cerr
        << "[punto] Warning: promoted primary failed to start IPC server\n";
    return;
  }
  publish_control_plane_state(/*bump_config_generation=*/true,
                              /*bump_status_generation=*/true);
}

void EventLoop::process_ready_results() {
  const auto now = std::chrono::steady_clock::now();
  if (telemetry_.last_report_at.time_since_epoch().count() == 0) {
    telemetry_.last_report_at = now;
  }

  WordResult result;
  while (analysis_pool_ && analysis_pool_->try_pop_result(result)) {
    if (result.task_id < next_apply_task_id_) {
      continue;
    }

    if (result.terminal_status == WordTerminalStatus::Completed) {
      ++telemetry_.analyzed_words;
      lifetime_telemetry_.analyzed_words.fetch_add(1,
                                                   std::memory_order_relaxed);
      if (result.need_switch) {
        ++telemetry_.need_switch_words;
        lifetime_telemetry_.need_switch_words.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
    telemetry_.analysis_us_sum += result.analysis_us;
    telemetry_.queue_us_sum += result.queue_us;
    telemetry_.analysis_us_max =
        std::max(telemetry_.analysis_us_max, result.analysis_us);
    telemetry_.queue_us_max =
        std::max(telemetry_.queue_us_max, result.queue_us);
    lifetime_telemetry_.analysis_us_sum.fetch_add(result.analysis_us,
                                                  std::memory_order_relaxed);
    lifetime_telemetry_.queue_us_sum.fetch_add(result.queue_us,
                                               std::memory_order_relaxed);
    ready_results_[result.task_id] = std::move(result);
  }

  if (analysis_pool_ && !analysis_pool_failed_ &&
      analysis_pool_->has_fatal_error()) {
    analysis_pool_failed_ = true;
    analysis_health_.fail();
  }

  while (ready_results_.erase(next_apply_task_id_) != 0) {
    commit_analysis_terminal(next_apply_task_id_);
  }
  lifetime_telemetry_.ready_results.store(ready_results_.size(),
                                          std::memory_order_relaxed);

  if (now - telemetry_.last_report_at < std::chrono::seconds{1}) {
    return;
  }

  const std::uint64_t words = telemetry_.analyzed_words;
  const std::uint64_t avg_queue =
      words > 0 ? telemetry_.queue_us_sum / words : 0;
  const std::uint64_t avg_analysis =
      words > 0 ? telemetry_.analysis_us_sum / words : 0;
  std::cerr << "[punto] Telemetry: words=" << words
            << " need_switch=" << telemetry_.need_switch_words
            << " avg_queue_us=" << avg_queue
            << " max_queue_us=" << telemetry_.queue_us_max
            << " avg_analysis_us=" << avg_analysis
            << " max_analysis_us=" << telemetry_.analysis_us_max
            << " corrections=0 avg_macro_us=0 max_macro_us=0"
               " avg_tail_len=0 max_tail_len=0 xkb_set=off\n";

  telemetry_.last_report_at = now;
  telemetry_.analyzed_words = 0;
  telemetry_.need_switch_words = 0;
  telemetry_.analysis_us_sum = 0;
  telemetry_.analysis_us_max = 0;
  telemetry_.queue_us_sum = 0;
  telemetry_.queue_us_max = 0;
}
IpcResult EventLoop::stats_report() const {
  const StallHealthSnapshot x11_health = x11_health_.snapshot();
  StallHealthSnapshot analysis_health = analysis_health_.snapshot();
  if (!analysis_pool_ && analysis_health.health != ComponentHealth::Failed) {
    analysis_health.health = ComponentHealth::Degraded;
    analysis_health.in_flight = dictionary_load_pending_;
  }
  const StallHealthSnapshot input_health = input_health_.snapshot();
  const std::uint64_t analyzed =
      lifetime_telemetry_.analyzed_words.load(std::memory_order_relaxed);
  const std::uint64_t queue_sum =
      lifetime_telemetry_.queue_us_sum.load(std::memory_order_relaxed);
  const std::uint64_t analysis_sum =
      lifetime_telemetry_.analysis_us_sum.load(std::memory_order_relaxed);

  std::string stats;
  stats.reserve(512);
  stats += "x11_health=";
  stats += component_health_name(x11_health.health);
  stats += " analysis_health=";
  stats += component_health_name(analysis_health.health);
  stats += " input_health=";
  stats += component_health_name(input_health.health);
  stats +=
      " x11_last_progress_ms=" + std::to_string(x11_health.last_progress_ms);
  stats += " analysis_last_progress_ms=" +
           std::to_string(analysis_health.last_progress_ms);
  stats += " input_last_progress_ms=" +
           std::to_string(input_health.last_progress_ms);
  stats +=
      " analysis_outstanding=" + std::to_string(analysis_accepted_at_.size());
  stats += " input_in_flight=";
  stats += input_health.in_flight ? "1" : "0";
  stats += " log_dropped=" + std::to_string(dropped_log_records());
  stats += " text_mutation=disabled";
  stats += " enabled=0";
  stats += " configured_enabled=";
  const auto configured = std::atomic_load(&config_);
  stats += configured && configured->auto_switch.enabled ? "1" : "0";
  stats += " config_pending=";
  stats += config_load_pending_ ? "1" : "0";
  stats += " config_generation=" + std::to_string(config_load_generation_);
  stats += " config_result=";
  switch (config_load_status_) {
  case ConfigLoadStatus::None:
    stats += "none";
    break;
  case ConfigLoadStatus::Ok:
    stats += "ok";
    break;
  case ConfigLoadStatus::Error:
    stats += "error";
    break;
  }
  stats += " analyzed=" + std::to_string(analyzed);
  stats += " need_switch=" +
           std::to_string(lifetime_telemetry_.need_switch_words.load(
               std::memory_order_relaxed));
  stats += " corrections=0";
  stats += " pending_words=0";
  stats +=
      " ready_results=" + std::to_string(lifetime_telemetry_.ready_results.load(
                              std::memory_order_relaxed));
  stats += " worker_threads=" +
           std::to_string(analysis_pool_ ? analysis_pool_->worker_count() : 0);
  stats +=
      " daemon_peers=" + std::to_string(analysis_thread_budget_.daemon_count);
  stats += " analysis_mode=";
  stats += analysis_thread_budget_.manual_override ? "fixed" : "auto";
  stats += " control_plane=";
  stats += control_plane_primary_.load(std::memory_order_acquire) ? "primary"
                                                                  : "secondary";
  stats +=
      " queued_tasks=" +
      std::to_string(analysis_pool_ ? analysis_pool_->pending_task_count() : 0);
  stats += " avg_queue_us=" +
           std::to_string(analyzed > 0 ? queue_sum / analyzed : 0);
  stats += " avg_analysis_us=" +
           std::to_string(analyzed > 0 ? analysis_sum / analyzed : 0);
  stats += " avg_macro_us=0";
  stats += " avg_tail_len=0";

  return {true, std::move(stats)};
}

IpcResult
EventLoop::reload_config(const std::string &config_path,
                         std::optional<std::uint64_t> control_plane_generation,
                         std::optional<std::uint64_t> x11_config_generation,
                         bool promotion_reconciliation) {
  if (config_load_pending_) {
    return {false, "Config reload in progress"};
  }

  const std::filesystem::path system_root{"/etc/punto"};
  std::optional<std::filesystem::path> user_root;
  std::optional<X11SessionInfo> session_authority;
  auto session_lease = [this]() -> std::optional<X11Session::WriteLease> {
    if (!x11_session_) {
      return std::nullopt;
    }
    return x11_session_->acquire_write_lease();
  }();
  if (session_lease) {
    const X11SessionInfo &info = session_lease->info();
    session_authority = info;
    if (!info.xdg_config_home.empty()) {
      user_root = std::filesystem::path{info.xdg_config_home} / "punto";
    } else if (!info.home_dir.empty()) {
      user_root = std::filesystem::path{info.home_dir} / ".config" / "punto";
    }
  }

  std::string normalized_requested_path;
  if (!config_path.empty()) {
    const std::filesystem::path requested{config_path};
    if (!requested.is_absolute()) {
      return {false, "Invalid path"};
    }
    const std::filesystem::path normalized = requested.lexically_normal();
    const bool allowed = path_is_beneath(normalized, system_root) ||
                         (user_root && path_is_beneath(normalized, *user_root));
    if (!allowed) {
      return {false, "Invalid path"};
    }
    normalized_requested_path = normalized.string();
  }

  ConfigLoadTask task;
  task.generation = ++config_load_generation_;
  task.system_root = system_root;
  task.user_root = std::move(user_root);
  task.session_authority = std::move(session_authority);
  task.control_plane_generation = control_plane_generation;
  task.x11_config_generation = x11_config_generation;
  task.requested_path = std::move(normalized_requested_path);
  task.promotion_reconciliation = promotion_reconciliation;
  {
    std::lock_guard<std::mutex> lock{config_loader_state_->mutex};
    if (config_loader_state_->stop_requested ||
        config_loader_state_->request.has_value() ||
        config_loader_state_->completion.has_value()) {
      return {false, "Config loader unavailable"};
    }
    config_loader_state_->request = std::move(task);
  }
  config_load_pending_ = true;
  config_load_status_ = ConfigLoadStatus::None;
  config_loader_state_->condition.notify_all();
  return {true, "Scheduled"};
}

void EventLoop::request_x11_config_reload() {
  const std::uint64_t generation = ++x11_config_generation_;
  if (config_load_pending_) {
    pending_x11_config_generation_ = generation;
    return;
  }

  IpcResult result = reload_config({}, std::nullopt, generation);
  if (!result.success) {
    pending_x11_config_generation_ = generation;
    std::cerr << "[punto] Warning: config reload after X11 refresh deferred: "
              << result.message << "\n";
  }
}

void EventLoop::retry_pending_x11_config_reload() {
  if (!pending_x11_config_generation_ || config_load_pending_ ||
      !x11_session_ || !x11_session_->is_valid()) {
    return;
  }

  const std::uint64_t generation = *pending_x11_config_generation_;
  pending_x11_config_generation_.reset();
  if (generation != x11_config_generation_) {
    return;
  }

  IpcResult result = reload_config({}, std::nullopt, generation);
  if (!result.success) {
    if (generation == x11_config_generation_) {
      pending_x11_config_generation_ = generation;
    }
    std::cerr << "[punto] Warning: deferred X11 config reload failed: "
              << result.message << "\n";
  }
}

void EventLoop::poll_config_load_completion() {
  std::optional<ConfigLoadCompletion> completion;
  {
    std::lock_guard<std::mutex> lock{config_loader_state_->mutex};
    if (config_loader_state_->completion) {
      completion = std::move(config_loader_state_->completion);
      config_loader_state_->completion.reset();
    }
  }
  if (!completion) {
    return;
  }

  config_load_pending_ = false;
  const auto finish = [this]() { retry_pending_x11_config_reload(); };
  if (completion->task.generation != config_load_generation_ ||
      (completion->task.x11_config_generation &&
       *completion->task.x11_config_generation != x11_config_generation_)) {
    config_load_status_ = pending_x11_config_generation_
                              ? ConfigLoadStatus::None
                              : ConfigLoadStatus::Error;
    std::cerr << "[punto] Config reload superseded by newer session\n";
    finish();
    return;
  }

  ConfigLoadOutcome &loaded = completion->outcome;
  if (loaded.result != ConfigResult::Ok) {
    config_load_status_ = ConfigLoadStatus::Error;
    std::cerr << "[punto] Config reload failed: " << loaded.error << "\n";
    finish();
    return;
  }

  auto authority_lease =
      [this, &completion]() -> std::optional<X11Session::WriteLease> {
    const auto &task = completion->task;
    if (!task.user_root ||
        !path_is_beneath(completion->outcome.used_path, *task.user_root)) {
      return std::nullopt;
    }
    if (!task.session_authority || !x11_session_) {
      return std::nullopt;
    }
    auto lease = x11_session_->acquire_write_lease();
    if (!lease ||
        !same_config_authority(lease->info(), *task.session_authority)) {
      return std::nullopt;
    }
    return lease;
  }();
  const bool user_config =
      completion->task.user_root &&
      path_is_beneath(loaded.used_path, *completion->task.user_root);
  if (user_config && !authority_lease) {
    config_load_status_ = ConfigLoadStatus::Error;
    std::cerr << "[punto] Config reload discarded after session change\n";
    finish();
    return;
  }

  try {
    auto old_cfg = std::atomic_load(&config_);
    auto new_cfg = std::make_shared<Config>(std::move(loaded.config));

    std::shared_ptr<const Config> cfg_const = new_cfg;

    std::atomic_store(&config_, std::move(cfg_const));
    update_log_level(new_cfg->logging.level);

    if (old_cfg && (old_cfg->runtime.analysis_threads !=
                        new_cfg->runtime.analysis_threads ||
                    old_cfg->runtime.max_analysis_threads_per_daemon !=
                        new_cfg->runtime.max_analysis_threads_per_daemon)) {
      std::cerr << "[punto] runtime thread settings changed; restart "
                   "punto/udevmon to apply\n";
    }

    std::cerr << "[punto] Configuration reloaded: " << loaded.used_path << "\n";
    std::cerr << "[punto] auto_switch: enabled=" << new_cfg->auto_switch.enabled
              << ", threshold=" << new_cfg->auto_switch.threshold
              << ", min_word_len=" << new_cfg->auto_switch.min_word_len
              << ", min_score=" << new_cfg->auto_switch.min_score
              << ", max_rollback_words="
              << new_cfg->auto_switch.max_rollback_words << '\n';

    if (completion->task.control_plane_generation) {
      std::lock_guard<std::mutex> lock(control_plane_mutex_);
      applied_config_generation_ = *completion->task.control_plane_generation;
      if (completion->task.promotion_reconciliation &&
          completion->used_promotion_fallback) {
        promotion_fallback_applied_generation_ =
            completion->task.control_plane_generation;
      }
    }

    if (control_plane_primary_.load(std::memory_order_acquire)) {
      publish_control_plane_state(/*bump_config_generation=*/true,
                                  /*bump_status_generation=*/true);
    }
    config_load_status_ = ConfigLoadStatus::Ok;
  } catch (...) {
    config_load_status_ = ConfigLoadStatus::Error;
    std::cerr << "[punto] Config commit failed\n";
  }
  finish();
}

} // namespace punto
