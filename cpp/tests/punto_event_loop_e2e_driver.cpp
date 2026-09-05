#include "punto/event_loop.hpp"

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <xcb/xcbext.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define explicit explicit_value
#include <xcb/xkb.h>
#undef explicit
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {
bool consume_private_fault_marker(const char *path) {
  const int marker =
      ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (marker < 0) {
    return false;
  }
  struct stat metadata {};
  const bool armed =
      ::fstat(marker, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
      metadata.st_uid == ::geteuid() && metadata.st_nlink == 1 &&
      (metadata.st_mode & 0077) == 0;
  (void)::close(marker);
  return armed && ::unlink(path) == 0;
}

std::mutex keyboard_query_mutex;
std::optional<std::pair<xcb_connection_t *, unsigned int>> keyboard_query;
} // namespace

extern "C" decltype(xcb_xkb_get_state) __real_xcb_xkb_get_state;
extern "C" xcb_xkb_get_state_cookie_t __wrap_xcb_xkb_get_state(
    xcb_connection_t *connection, xcb_xkb_device_spec_t device) {
  const auto cookie = __real_xcb_xkb_get_state(connection, device);
  if (consume_private_fault_marker("/run/punto-e2e-arm-keyboard-observation")) {
    std::lock_guard lock{keyboard_query_mutex};
    keyboard_query = std::pair{connection, cookie.sequence};
  }
  return cookie;
}

extern "C" decltype(xcb_poll_for_reply) __real_xcb_poll_for_reply;
extern "C" int __wrap_xcb_poll_for_reply(
    xcb_connection_t *connection, unsigned int sequence, void **reply,
    xcb_generic_error_t **error) {
  const int result = __real_xcb_poll_for_reply(connection, sequence, reply, error);
  bool observed = false;
  if (result != 0 && reply != nullptr && *reply != nullptr) {
    std::lock_guard lock{keyboard_query_mutex};
    if (keyboard_query == std::optional{std::pair{connection, sequence}}) {
      keyboard_query.reset();
      observed = true;
    }
  }
  if (observed) {
    const bool hold = consume_private_fault_marker("/run/punto-e2e-hold-keyboard-observation");
    const int marker = ::open("/run/punto-e2e-keyboard-observed",
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      (void)::close(marker);
    }
    // Test-only delayed delivery of an already captured reply, not a server
    // delay or a production timeout. Never retain the mutex while waiting.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (hold && std::chrono::steady_clock::now() < deadline &&
           !consume_private_fault_marker("/run/punto-e2e-release-keyboard-observation")) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
  return result;
}

extern "C" decltype(xcb_disconnect) __real_xcb_disconnect;
extern "C" void __wrap_xcb_disconnect(xcb_connection_t *connection) {
  {
    std::lock_guard lock{keyboard_query_mutex};
    if (keyboard_query && keyboard_query->first == connection) {
      keyboard_query.reset();
    }
  }
  __real_xcb_disconnect(connection);
}

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  constexpr const char *arm = "/run/punto-e2e-fail-directory-fsync";
  struct stat target {}, runtime {};
  if (::fstat(fd, &target) == 0 && S_ISDIR(target.st_mode) &&
      ::stat("/run", &runtime) == 0 &&
      target.st_dev == runtime.st_dev && target.st_ino == runtime.st_ino &&
      consume_private_fault_marker(arm)) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

extern "C" decltype(xcb_xkb_get_map) __real_xcb_xkb_get_map;
extern "C" xcb_xkb_get_map_cookie_t __wrap_xcb_xkb_get_map(
    xcb_connection_t *connection, xcb_xkb_device_spec_t device,
    std::uint16_t full, std::uint16_t partial, std::uint8_t first_type,
    std::uint8_t types, xcb_keycode_t first_symbol, std::uint8_t symbols,
    xcb_keycode_t first_action, std::uint8_t actions,
    xcb_keycode_t first_behavior, std::uint8_t behaviors,
    std::uint16_t virtual_modifiers, xcb_keycode_t first_explicit,
    std::uint8_t explicit_count, xcb_keycode_t first_modifier,
    std::uint8_t modifiers, xcb_keycode_t first_virtual_modifier,
    std::uint8_t virtual_modifier_count) {
  if (consume_private_fault_marker("/run/punto-e2e-fail-xkb-map")) {
    return {};
  }
  return __real_xcb_xkb_get_map(
      connection, device, full, partial, first_type, types, first_symbol,
      symbols, first_action, actions, first_behavior, behaviors,
      virtual_modifiers, first_explicit, explicit_count, first_modifier,
      modifiers, first_virtual_modifier, virtual_modifier_count);
}

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
  if (std::getenv("PUNTO_E2E_DYNAMIC_LAYOUT") != nullptr &&
      std::filesystem::exists("/run/punto-e2e-layout-ru")) {
    info.observed_keyboard_layout = 1;
  }
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
  const ScopedFixturePath affix{"/tmp/punto-e2e" + suffix + ".aff"};
  punto::DictionaryLoadOutcome outcome;
  if (!write_fixture(english.path, "2\nhello\nworld\n") ||
      !write_fixture(russian.path, "2\nпривет\nжест\n") ||
      !write_fixture(affix.path, "SET UTF-8\nTRY esiarntolcdugmphbyfvkwzxjq\n")) {
    return outcome;
  }

  punto::DictionaryLoadSpec spec;
  spec.english_paths = {english.path};
  spec.russian_paths = {russian.path};
  spec.english_affix = affix.path;
  spec.english_hunspell_dictionary = english.path;
  spec.russian_affix = affix.path;
  spec.russian_hunspell_dictionary = russian.path;
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
