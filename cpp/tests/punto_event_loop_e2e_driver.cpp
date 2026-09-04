#include "punto/event_loop.hpp"

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop_pipe_write_fd = -1;

void stop_signal_handler(int signal) {
  if (signal != SIGINT && signal != SIGTERM) {
    return;
  }
  const int saved_errno = errno;
  const int fd = static_cast<int>(g_stop_pipe_write_fd);
  if (fd >= 0) {
    constexpr char kStopByte = 'x';
    const ssize_t ignored = ::write(fd, &kStopByte, sizeof(kStopByte));
    (void)ignored;
  }
  errno = saved_errno;
}

const char *required_environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    std::cerr << "missing required environment: " << name << '\n';
    std::exit(2);
  }
  return value;
}

punto::x11_detail::ProbeResult probe_test_session() {
  if (std::getenv("PUNTO_E2E_STUCK_PROBE") != nullptr) {
    const int marker =
        ::open("/run/punto-e2e-stuck-probe-ready",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
    for (;;) {
      std::this_thread::sleep_for(std::chrono::hours{1});
    }
  }

  const bool session_race =
      std::getenv("PUNTO_E2E_CONFIG_SESSION_RACE") != nullptr;
  const bool new_session =
      session_race && std::filesystem::exists("/run/punto-e2e-switch-session");

  punto::X11SessionInfo info;
  info.session_id =
      new_session ? "punto-event-loop-e2e-new" : "punto-event-loop-e2e";
  info.username = "punto-e2e";
  info.uid = static_cast<std::uint32_t>(::getuid());
  info.gid = static_cast<std::uint32_t>(::getgid());
  const char *probe_display = std::getenv("PUNTO_E2E_PROBE_DISPLAY");
  info.display = probe_display != nullptr && probe_display[0] != '\0'
                     ? probe_display
                     : required_environment("DISPLAY");
  info.xauthority_path = required_environment("XAUTHORITY");
  info.home_dir = session_race ? (new_session ? "/tmp/punto-new-home"
                                              : "/tmp/punto-old-home")
                               : required_environment("HOME");
  info.xdg_runtime_dir = required_environment("XDG_RUNTIME_DIR");
  info.xdg_config_home = session_race ? (new_session ? "/tmp/punto-new-config"
                                                     : "/tmp/punto-old-config")
                                      : required_environment("XDG_CONFIG_HOME");
  info.observed_keyboard_layout = 0;
  if (const char *layout = std::getenv("PUNTO_E2E_OBSERVED_LAYOUT");
      layout != nullptr && (layout[0] == '0' || layout[0] == '1') &&
      layout[1] == '\0') {
    info.observed_keyboard_layout = layout[0] - '0';
  }

  if (new_session) {
    const int marker =
        ::open("/run/punto-e2e-new-session-observed",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
  }

  const int group_count = ::getgroups(0, nullptr);
  if (group_count > 0) {
    std::vector<gid_t> groups(static_cast<std::size_t>(group_count));
    if (::getgroups(group_count, groups.data()) == group_count) {
      info.supplementary_groups.reserve(groups.size());
      for (const gid_t group : groups) {
        info.supplementary_groups.push_back(static_cast<std::uint32_t>(group));
      }
    }
  }

  return {punto::x11_detail::ProbeStatus::Healthy, std::move(info)};
}

punto::ConfigLoadOutcome
blocking_config_loader(const std::filesystem::path &,
                       const std::optional<std::filesystem::path> &,
                       const std::string &) {
  const int marker = ::open("/run/punto-e2e-stuck-config-ready",
                            O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (marker >= 0) {
    ::close(marker);
  }
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours{1});
  }
}

punto::ConfigLoadOutcome session_switch_config_loader(
    const std::filesystem::path &,
    const std::optional<std::filesystem::path> &user_root,
    const std::string &) {
  punto::ConfigLoadOutcome outcome;
  if (!user_root) {
    outcome.result = punto::ConfigResult::IoError;
    outcome.error = "missing session config root";
    return outcome;
  }

  const bool old_session =
      user_root->string().find("punto-old-config") != std::string::npos;
  if (old_session) {
    const int marker =
        ::open("/run/punto-e2e-old-config-ready",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
    while (!std::filesystem::exists("/run/punto-e2e-release-old-config")) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  outcome.config.auto_switch.enabled = old_session;
  outcome.config.sound.enabled = false;
  outcome.config.logging.level = punto::LogLevel::Error;
  outcome.config.runtime.analysis_threads = 1;
  outcome.config.runtime.max_analysis_threads_per_daemon = 1;
  outcome.used_path = *user_root / "config.yaml";
  outcome.config.config_path = outcome.used_path;
  outcome.result = punto::ConfigResult::Ok;

  if (!old_session) {
    const int marker =
        ::open("/run/punto-e2e-new-config-loaded",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
  }
  return outcome;
}

bool write_fixture(const std::filesystem::path &path,
                   const std::string &contents) {
  const int fd =
      ::open(path.c_str(),
             O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        ::write(fd, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return ::close(fd) == 0;
}

struct ScopedFixturePath {
  std::filesystem::path path;
  ~ScopedFixturePath() { (void)::unlink(path.c_str()); }
};

punto::DictionaryLoadOutcome deterministic_dictionary_loader() {
  const std::string suffix = "-" + std::to_string(::getpid()) + ".dic";
  const ScopedFixturePath english{"/tmp/punto-e2e-en" + suffix};
  const ScopedFixturePath russian{"/tmp/punto-e2e-ru" + suffix};
  punto::DictionaryLoadOutcome outcome;
  if (!write_fixture(english.path, "2\nhello\nworld\n") ||
      !write_fixture(russian.path, "1\nпривет\n")) {
    return outcome;
  }

  punto::DictionaryLoadSpec spec;
  spec.english_paths = {english.path};
  spec.russian_paths = {russian.path};
  auto dictionary = std::make_unique<punto::Dictionary>();
  outcome.result = dictionary->initialize_bounded(spec);
  if (outcome.result == punto::DictionaryLoadResult::Ok) {
    outcome.dictionary = std::move(dictionary);
  }
  return outcome;
}

punto::DictionaryLoadOutcome oversize_dictionary_loader() {
  const ScopedFixturePath path{"/tmp/punto-e2e-oversize-" +
                               std::to_string(::getpid()) + ".dic"};
  punto::DictionaryLoadOutcome outcome;
  if (!write_fixture(path.path, "1\n" + std::string(128, 'a') + "\n")) {
    return outcome;
  }
  punto::DictionaryLoadSpec spec;
  spec.english_paths = {path.path};
  spec.limits.max_file_bytes = 64;
  auto dictionary = std::make_unique<punto::Dictionary>();
  outcome.result = dictionary->initialize_bounded(spec);
  if (outcome.result == punto::DictionaryLoadResult::Ok) {
    outcome.dictionary = std::move(dictionary);
  }
  return outcome;
}

punto::DictionaryLoadOutcome blocking_dictionary_loader() {
  const int marker = ::open("/run/punto-e2e-stuck-dictionary-ready",
                            O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (marker >= 0) {
    ::close(marker);
  }
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours{1});
  }
}

} // namespace

int main() {
  int stop_pipe[2] = {-1, -1};
  if (::pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    std::cerr << "failed to create stop pipe\n";
    return 2;
  }

  struct sigaction stop_action {};
  stop_action.sa_handler = stop_signal_handler;
  ::sigemptyset(&stop_action.sa_mask);

  struct sigaction ignore_sigpipe {};
  ignore_sigpipe.sa_handler = SIG_IGN;
  ::sigemptyset(&ignore_sigpipe.sa_mask);

  g_stop_pipe_write_fd = stop_pipe[1];
  if (::sigaction(SIGPIPE, &ignore_sigpipe, nullptr) != 0 ||
      ::sigaction(SIGINT, &stop_action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &stop_action, nullptr) != 0) {
    g_stop_pipe_write_fd = -1;
    (void)::close(stop_pipe[0]);
    (void)::close(stop_pipe[1]);
    std::cerr << "failed to install signal handlers\n";
    return 2;
  }

  if (const char *proc_root = std::getenv("PUNTO_E2E_PROC_SCAN_ROOT");
      proc_root != nullptr && proc_root[0] != '\0') {
    const std::size_t observed =
        punto::event_loop_detail::count_running_punto_daemons(
            proc_root, 1, std::chrono::milliseconds{100});
    const std::size_t conservative = std::max<std::size_t>(
        std::thread::hardware_concurrency(), static_cast<unsigned int>(1));
    return observed >= conservative ? 0 : 1;
  }

  punto::Config config;
  config.auto_switch.enabled = true;
  config.sound.enabled = false;
  config.logging.level = punto::LogLevel::Error;
  config.runtime.analysis_threads = 1;
  config.runtime.max_analysis_threads_per_daemon = 1;

  punto::EventLoop::ConfigLoaderFunction config_loader;
  if (std::getenv("PUNTO_E2E_STUCK_CONFIG") != nullptr) {
    config_loader = blocking_config_loader;
  } else if (std::getenv("PUNTO_E2E_CONFIG_SESSION_RACE") != nullptr) {
    config_loader = session_switch_config_loader;
  }

  punto::EventLoop::DictionaryLoaderFunction dictionary_loader =
      deterministic_dictionary_loader;
  if (std::getenv("PUNTO_E2E_STUCK_DICTIONARY") != nullptr) {
    dictionary_loader = blocking_dictionary_loader;
  } else if (std::getenv("PUNTO_E2E_OVERSIZE_DICTIONARY") != nullptr) {
    dictionary_loader = oversize_dictionary_loader;
  }

  punto::EventLoop loop{std::move(config), probe_test_session,
                        std::move(config_loader), std::move(dictionary_loader)};
  loop.set_stop_signal_fd(stop_pipe[0]);
  const int result = loop.run();
  g_stop_pipe_write_fd = -1;
  (void)::close(stop_pipe[0]);
  (void)::close(stop_pipe[1]);
  return result;
}
