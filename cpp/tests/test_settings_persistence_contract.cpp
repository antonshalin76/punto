#include "punto/config.hpp"
#include "punto/settings_dialog.hpp"

#include <linux/input-event-codes.h>

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PUNTO_SOURCE_DIR
#define PUNTO_SOURCE_DIR "."
#endif

namespace {

class TestRunner {
public:
  void expect(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << message << '\n';
    }
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

private:
  int failures_ = 0;
};

class TempDirectory {
public:
  TempDirectory() {
    char path[] = "/tmp/punto-settings-contract-XXXXXX";
    char *created = ::mkdtemp(path);
    if (created == nullptr) {
      throw std::runtime_error{"mkdtemp failed"};
    }
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class ScopedHome {
public:
  explicit ScopedHome(const std::string &value) {
    if (const char *current = std::getenv("HOME"); current != nullptr) {
      previous_ = current;
    }
    if (::setenv("HOME", value.c_str(), 1) != 0) {
      throw std::runtime_error{"setenv(HOME) failed"};
    }
  }

  ~ScopedHome() {
    if (previous_) {
      (void)::setenv("HOME", previous_->c_str(), 1);
    } else {
      (void)::unsetenv("HOME");
    }
  }

  ScopedHome(const ScopedHome &) = delete;
  ScopedHome &operator=(const ScopedHome &) = delete;

private:
  std::optional<std::string> previous_;
};

class ScopedEnvironment {
public:
  ScopedEnvironment(std::string name, const std::string &value)
      : name_(std::move(name)) {
    if (const char *current = std::getenv(name_.c_str()); current != nullptr) {
      previous_ = current;
    }
    if (::setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error{"setenv failed"};
    }
  }

  ~ScopedEnvironment() {
    if (previous_) {
      (void)::setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      (void)::unsetenv(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

private:
  std::string name_;
  std::optional<std::string> previous_;
};

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output.is_open()) {
    throw std::runtime_error{"cannot create test file"};
  }
  output << contents;
  if (!output.good()) {
    throw std::runtime_error{"cannot write test file"};
  }
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input.is_open()) {
    throw std::runtime_error{"cannot read test file"};
  }
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

[[nodiscard]] mode_t file_mode(const std::filesystem::path &path) {
  struct stat status {};
  if (::stat(path.c_str(), &status) != 0) {
    throw std::runtime_error{"cannot stat test file"};
  }
  return status.st_mode & 07777;
}

[[nodiscard]] bool same_settings(const punto::SettingsData &left,
                                 const punto::SettingsData &right) {
  return left.auto_enabled == right.auto_enabled &&
         left.threshold == right.threshold &&
         left.min_word_len == right.min_word_len &&
         left.min_score == right.min_score &&
         left.max_rollback_words == right.max_rollback_words &&
         left.typo_correction_enabled == right.typo_correction_enabled &&
         left.max_typo_diff == right.max_typo_diff &&
         left.sticky_shift_correction_enabled ==
             right.sticky_shift_correction_enabled &&
         left.sound_enabled == right.sound_enabled &&
         left.modifier == right.modifier && left.key == right.key;
}

void make_executable_spy(const std::filesystem::path &path) {
  write_file(path, R"(#!/bin/sh
: > "$PUNTO_PROCESS_SPY"
exit 97
)");
  std::error_code error;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    throw std::runtime_error{"cannot make process spy executable"};
  }
}

[[nodiscard]] std::string valid_config() {
  return R"(hotkey:
  modifier: leftctrl
  key: grave
auto_switch:
  enabled: true
  threshold: 3.5
  min_word_len: 2
  min_score: 5.0
  max_rollback_words: 5
  typo_correction_enabled: false
  max_typo_diff: 2
  sticky_shift_correction_enabled: true
sound:
  enabled: true
logging:
  level: debug
runtime:
  analysis_threads: 7
  max_analysis_threads_per_daemon: 11
)";
}

[[nodiscard]] punto::SettingsData edited_settings() {
  punto::SettingsData settings;
  settings.auto_enabled = false;
  settings.threshold = 4.5;
  settings.min_word_len = 4;
  settings.min_score = 7.5;
  settings.max_rollback_words = 9;
  settings.typo_correction_enabled = false;
  settings.max_typo_diff = 1;
  settings.sticky_shift_correction_enabled = false;
  settings.sound_enabled = false;
  settings.modifier = "rightalt";
  settings.key = "space";
  return settings;
}

[[nodiscard]] std::filesystem::path
user_config_path(const std::filesystem::path &home) {
  return home / ".config/punto/config.yaml";
}

void test_canonical_ui_defaults(TestRunner &runner) {
  const punto::SettingsData settings;
  runner.expect(settings.auto_enabled, "auto-switch default is enabled");
  runner.expect(settings.threshold == 3.5, "threshold default is 3.5");
  runner.expect(settings.min_word_len == 2, "min-word default is 2");
  runner.expect(settings.min_score == 5.0, "min-score default is 5.0");
  runner.expect(settings.max_rollback_words == 5, "rollback default is 5");
  runner.expect(!settings.typo_correction_enabled,
                "typo correction default matches shipped disabled mode");
  runner.expect(settings.max_typo_diff == 2,
                "typo distance default matches shipped value 2");
  runner.expect(settings.sticky_shift_correction_enabled,
                "sticky-shift default is enabled");
  runner.expect(settings.sound_enabled, "sound default is enabled");
  runner.expect(settings.modifier == "leftctrl",
                "modifier default matches the shipped config");
  runner.expect(settings.key == "grave",
                "key default matches the shipped config");
}

void test_persistence_owner_is_process_free(TestRunner &runner) {
  const std::filesystem::path tray_source =
      std::filesystem::path{PUNTO_SOURCE_DIR} / "cpp/src/tray";
  const std::vector<std::string> forbidden = {
      "std::system(", "::system(",    "popen(",       "fork(",
      "vfork(",       "execl(",       "execv(",       "execve(",
      "execvp(",      "posix_spawn(", "posix_spawnp("};

  bool inspected_owner = false;
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator{tray_source}) {
    const std::string filename = entry.path().filename().string();
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp" ||
        (filename.find("settings") == std::string::npos &&
         filename.find("persistence") == std::string::npos)) {
      continue;
    }
    inspected_owner = true;
    const std::string source = read_file(entry.path());
    for (const std::string &token : forbidden) {
      runner.expect(source.find(token) == std::string::npos,
                    filename +
                        ": settings persistence does not launch processes");
    }
  }
  runner.expect(inspected_owner,
                "settings persistence source owner is present and inspected");
}

void test_compatibility_hotkey_is_not_exposed_as_active_ui(TestRunner &runner) {
  const std::filesystem::path source_root{PUNTO_SOURCE_DIR};
  const std::string contents =
      read_file(source_root / "cpp/src/tray/settings_dialog.cpp");
  for (const char *promise : {"Горячие клавиши", "Модификатор:", "Клавиша:",
                              "gtk_combo_box_text_new"}) {
    runner.expect(contents.find(promise) == std::string::npos,
                  std::string{"inactive compatibility hotkey is absent from "
                              "settings UI: "} +
                      promise);
  }

  const std::string tray = read_file(source_root / "cpp/src/tray/tray_app.cpp");
  for (const char *mutation_callback :
       {"on_auto_toggle_changed", "on_sound_toggle_changed",
        "apply_settings_change_with_reload"}) {
    runner.expect(tray.find(mutation_callback) == std::string::npos,
                  std::string{"inactive tray mutation callback is absent: "} +
                      mutation_callback);
  }

  const std::string ipc_header =
      read_file(source_root / "cpp/include/punto/ipc_client.hpp");
  runner.expect(ipc_header.find("set_status(") == std::string::npos,
                "inactive typed tray SET_STATUS API is absent");
}

void test_hostile_home_is_data_not_shell(TestRunner &runner,
                                         const TempDirectory &directory) {
  const std::filesystem::path sentinel = directory.path() / "SHELL_EXECUTED";
  const std::filesystem::path process_spy =
      directory.path() / "PROCESS_LAUNCHED";
  const std::filesystem::path spy_bin = directory.path() / "spy-bin";
  make_executable_spy(spy_bin / "mkdir");
  make_executable_spy(spy_bin / "cp");
  make_executable_spy(spy_bin / "touch");
  const std::string hostile_home =
      (directory.path() / "home\"; touch \"").string() + sentinel.string() +
      "\"; #";

  {
    ScopedHome home{hostile_home};
    ScopedEnvironment path{"PATH", spy_bin.string()};
    ScopedEnvironment spy{"PUNTO_PROCESS_SPY", process_spy.string()};
    try {
      (void)punto::SettingsDialog::load_settings();
    } catch (...) {
      runner.expect(false,
                    "hostile HOME must not throw while loading settings");
    }
  }

  runner.expect(!std::filesystem::exists(sentinel),
                "HOME bytes must never be evaluated by a shell");
  runner.expect(!std::filesystem::exists(process_spy),
                "settings persistence performs zero process launches");
}

void test_malformed_config_does_not_crash(TestRunner &runner,
                                          const TempDirectory &directory) {
  const std::filesystem::path home = directory.path() / "malformed-home";
  const std::filesystem::path path = user_config_path(home);
  const std::string malformed = R"(auto_switch:
  min_word_len: definitely-not-an-integer
)";
  write_file(path, malformed);

  bool threw = false;
  std::optional<punto::SettingsData> loaded;
  {
    ScopedHome scoped_home{home.string()};
    try {
      loaded = punto::SettingsDialog::load_settings();
    } catch (...) {
      threw = true;
    }
  }
  runner.expect(!threw, "malformed user config must not crash punto-tray");
  runner.expect(loaded.has_value() &&
                    same_settings(*loaded, punto::SettingsData{}),
                "malformed user config returns canonical safe UI defaults");
  runner.expect(read_file(path) == malformed,
                "malformed user config remains byte-for-byte unchanged");
  runner.expect(
      std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
      "malformed user config remains a regular file");
}

void test_save_preserves_non_ui_fields(TestRunner &runner,
                                       const TempDirectory &directory) {
  const std::filesystem::path home = directory.path() / "roundtrip-home";
  const std::filesystem::path path = user_config_path(home);
  write_file(path, valid_config());

  bool saved = false;
  {
    ScopedHome scoped_home{home.string()};
    saved = punto::SettingsDialog::save_settings(edited_settings());
  }
  runner.expect(saved, "valid settings save succeeds");

  const punto::ConfigLoadOutcome loaded = punto::load_config_checked(path);
  runner.expect(loaded.result == punto::ConfigResult::Ok,
                "saved settings remain a valid daemon config");
  runner.expect(loaded.error.empty(),
                "successful settings round-trip has no diagnostic");
  runner.expect(loaded.used_path == path && loaded.config.config_path == path,
                "successful settings round-trip retains its selected path");
  runner.expect(loaded.config.hotkey.modifier == KEY_RIGHTALT,
                "save persists the edited modifier");
  runner.expect(loaded.config.hotkey.key == KEY_SPACE,
                "save persists the edited hotkey key");
  runner.expect(!loaded.config.auto_switch.enabled,
                "save persists auto-switch enabled state");
  runner.expect(loaded.config.auto_switch.threshold == 4.5,
                "save persists threshold");
  runner.expect(loaded.config.auto_switch.min_word_len == 4,
                "save persists minimum word length");
  runner.expect(loaded.config.auto_switch.min_score == 7.5,
                "save persists minimum score");
  runner.expect(loaded.config.auto_switch.max_rollback_words == 9,
                "save persists rollback depth");
  runner.expect(!loaded.config.auto_switch.typo_correction_enabled,
                "save persists typo correction state");
  runner.expect(loaded.config.auto_switch.max_typo_diff == 1,
                "save persists typo distance");
  runner.expect(!loaded.config.auto_switch.sticky_shift_correction_enabled,
                "save persists sticky-shift state");
  runner.expect(!loaded.config.sound.enabled, "save persists sound state");
  runner.expect(loaded.config.logging.level == punto::LogLevel::Debug,
                "save preserves logging.level not represented in the UI");
  runner.expect(loaded.config.runtime.analysis_threads == 7,
                "save preserves runtime.analysis_threads");
  runner.expect(loaded.config.runtime.max_analysis_threads_per_daemon == 11,
                "save preserves runtime max thread budget");
  runner.expect(!std::filesystem::exists(path.string() + ".tmp"),
                "successful save leaves no predictable temp path");
  runner.expect(file_mode(path) == 0600,
                "successful save installs the active config with mode 0600");
}

void test_modifier_chords_save(TestRunner &runner,
                               const TempDirectory &directory) {
  struct ChordCase {
    std::string name;
    std::string modifier;
    std::string key;
    std::uint16_t expected_modifier;
    std::uint16_t expected_key;
  };
  const std::array<ChordCase, 2> cases = {{
      {"alt-shift", "leftalt", "leftshift", KEY_LEFTALT, KEY_LEFTSHIFT},
      {"ctrl-alt", "rightctrl", "rightalt", KEY_RIGHTCTRL, KEY_RIGHTALT},
  }};

  for (const ChordCase &test_case : cases) {
    const std::filesystem::path home =
        directory.path() / (test_case.name + "-home");
    const std::filesystem::path path = user_config_path(home);
    write_file(path, valid_config());
    punto::SettingsData settings = edited_settings();
    settings.modifier = test_case.modifier;
    settings.key = test_case.key;

    bool saved = false;
    {
      ScopedHome scoped_home{home.string()};
      saved = punto::SettingsDialog::save_settings(settings);
    }
    runner.expect(saved, test_case.name + ": documented chord saves");
    const punto::ConfigLoadOutcome loaded = punto::load_config_checked(path);
    runner.expect(loaded.result == punto::ConfigResult::Ok &&
                      loaded.config.hotkey.modifier ==
                          test_case.expected_modifier &&
                      loaded.config.hotkey.key == test_case.expected_key,
                  test_case.name + ": saved chord round-trips");
    runner.expect(file_mode(path) == 0600,
                  test_case.name + ": saved config mode is 0600");
  }
}

void test_inclusive_ui_boundaries_save(TestRunner &runner,
                                       const TempDirectory &directory) {
  std::vector<std::pair<std::string, punto::SettingsData>> cases;

  punto::SettingsData lower = edited_settings();
  lower.threshold = 0.5;
  lower.min_word_len = 1;
  lower.min_score = 0.0;
  lower.max_rollback_words = 1;
  lower.max_typo_diff = 1;
  cases.emplace_back("lower-boundaries", lower);

  punto::SettingsData upper = edited_settings();
  upper.threshold = 10.0;
  upper.min_word_len = 10;
  upper.min_score = 20.0;
  upper.max_rollback_words = 50;
  upper.max_typo_diff = 2;
  cases.emplace_back("upper-boundaries", upper);

  for (const auto &[name, settings] : cases) {
    const std::filesystem::path home = directory.path() / name;
    const std::filesystem::path path = user_config_path(home);
    write_file(path, valid_config());

    bool saved = false;
    punto::SettingsData loaded_ui;
    {
      ScopedHome scoped_home{home.string()};
      saved = punto::SettingsDialog::save_settings(settings);
      loaded_ui = punto::SettingsDialog::load_settings();
    }
    runner.expect(saved, name + ": inclusive UI boundary save succeeds");
    runner.expect(same_settings(loaded_ui, settings),
                  name + ": inclusive UI boundaries round-trip exactly");

    const punto::ConfigLoadOutcome loaded = punto::load_config_checked(path);
    runner.expect(loaded.result == punto::ConfigResult::Ok,
                  name + ": boundary output is valid daemon YAML");
  }
}

void test_temp_path_race_is_safe(TestRunner &runner,
                                 const TempDirectory &directory) {
  const std::filesystem::path home = directory.path() / "symlink-home";
  const std::filesystem::path path = user_config_path(home);
  const std::filesystem::path victim = directory.path() / "victim";
  const std::filesystem::path attacker_path = path.string() + ".tmp";
  write_file(path, valid_config());
  write_file(victim, "DO NOT MODIFY\n");
  const std::string config_before = read_file(path);
  const std::string victim_before = read_file(victim);

  if (::mkfifo(attacker_path.c_str(), 0600) != 0) {
    throw std::runtime_error{"cannot create attacker FIFO"};
  }
  const int keeper =
      ::open(attacker_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (keeper < 0) {
    throw std::runtime_error{"cannot open attacker FIFO"};
  }
  (void)::fcntl(keeper, F_SETPIPE_SZ, 4096);

  std::array<char, 4096> filler{};
  while (::write(keeper, filler.data(), filler.size()) > 0) {
  }
  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    (void)::close(keeper);
    throw std::runtime_error{"cannot fill attacker FIFO"};
  }

  const int notify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (notify_fd < 0 ||
      ::inotify_add_watch(notify_fd, path.parent_path().c_str(), IN_OPEN) < 0) {
    (void)::close(keeper);
    if (notify_fd >= 0) {
      (void)::close(notify_fd);
    }
    throw std::runtime_error{"cannot watch temporary path"};
  }

  ScopedHome scoped_home{home.string()};
  int result_pipe[2] = {-1, -1};
  if (::pipe2(result_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    (void)::close(notify_fd);
    (void)::close(keeper);
    throw std::runtime_error{"cannot create save result pipe"};
  }
  const pid_t child = ::fork();
  if (child < 0) {
    (void)::close(result_pipe[0]);
    (void)::close(result_pipe[1]);
    (void)::close(notify_fd);
    (void)::close(keeper);
    throw std::runtime_error{"cannot fork settings writer"};
  }
  if (child == 0) {
    (void)::close(result_pipe[0]);
    (void)::close(notify_fd);
    (void)::close(keeper);
    const bool saved = punto::SettingsDialog::save_settings(edited_settings());
    const char result = saved ? '1' : '0';
    ssize_t written = -1;
    do {
      written = ::write(result_pipe[1], &result, 1);
    } while (written < 0 && errno == EINTR);
    ::_exit(written == 1 ? 0 : 126);
  }
  (void)::close(result_pipe[1]);

  bool predictable_temp_opened = false;
  std::array<pollfd, 2> initial_descriptors = {
      pollfd{notify_fd, POLLIN, 0}, pollfd{result_pipe[0], POLLIN, 0}};
  if (::poll(initial_descriptors.data(), initial_descriptors.size(), 500) > 0 &&
      (initial_descriptors[0].revents & POLLIN) != 0) {
    std::array<char, 4096> events{};
    const ssize_t count = ::read(notify_fd, events.data(), events.size());
    std::size_t offset = 0;
    while (count > 0 &&
           offset + sizeof(inotify_event) <= static_cast<std::size_t>(count)) {
      const auto *event =
          reinterpret_cast<const inotify_event *>(events.data() + offset);
      if (event->len > 0 &&
          std::string_view{event->name} == attacker_path.filename().string()) {
        predictable_temp_opened = true;
      }
      offset += sizeof(inotify_event) + event->len;
    }
  }

  if (predictable_temp_opened) {
    std::error_code error;
    std::filesystem::remove(attacker_path, error);
    if (error) {
      (void)::close(notify_fd);
      (void)::close(keeper);
      throw std::runtime_error{"cannot replace attacker path"};
    }
    std::filesystem::create_symlink(victim, attacker_path, error);
    if (error) {
      (void)::close(notify_fd);
      (void)::close(keeper);
      throw std::runtime_error{"cannot install raced symlink"};
    }
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  int child_status = 0;
  pid_t waited = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    while (::read(keeper, filler.data(), filler.size()) > 0) {
    }
    waited = ::waitpid(child, &child_status, WNOHANG);
    if (waited == child || waited < 0) {
      break;
    }
    (void)::poll(nullptr, 0, 5);
  }
  if (waited == 0) {
    (void)::kill(child, SIGKILL);
    do {
      waited = ::waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
  }

  char result = '\0';
  ssize_t result_size = -1;
  do {
    result_size = ::read(result_pipe[0], &result, 1);
  } while (result_size < 0 && errno == EINTR);
  const bool completed = waited == child && WIFEXITED(child_status) &&
                         WEXITSTATUS(child_status) == 0 && result_size == 1;
  const bool saved = completed && result == '1';
  (void)::close(result_pipe[0]);
  (void)::close(notify_fd);
  (void)::close(keeper);

  runner.expect(completed, "adversarial temporary-file save is bounded");
  runner.expect(read_file(victim) == victim_before,
                "save never writes through an attacker-controlled path");
  const bool active_is_regular =
      std::filesystem::is_regular_file(std::filesystem::symlink_status(path));
  runner.expect(active_is_regular,
                "active config remains a regular file after the race");
  if (saved && active_is_regular) {
    const punto::ConfigLoadOutcome loaded = punto::load_config_checked(path);
    runner.expect(loaded.result == punto::ConfigResult::Ok &&
                      loaded.config.hotkey.modifier == KEY_RIGHTALT &&
                      loaded.config.hotkey.key == KEY_SPACE,
                  "safe success atomically installs the requested settings");
  } else if (!saved && active_is_regular) {
    runner.expect(read_file(path) == config_before,
                  "safe failure leaves the active config byte-identical");
  }
}

void test_invalid_ui_values_are_atomic(TestRunner &runner,
                                       const TempDirectory &directory) {
  const std::filesystem::path home = directory.path() / "invalid-home";
  const std::filesystem::path path = user_config_path(home);

  struct InvalidSettingsCase {
    std::string name;
    std::function<void(punto::SettingsData &)> mutate;
  };
  const std::vector<InvalidSettingsCase> cases = {
      {"threshold-nan",
       [](punto::SettingsData &value) {
         value.threshold = std::numeric_limits<double>::quiet_NaN();
       }},
      {"threshold-positive-infinity",
       [](punto::SettingsData &value) {
         value.threshold = std::numeric_limits<double>::infinity();
       }},
      {"threshold-negative-infinity",
       [](punto::SettingsData &value) {
         value.threshold = -std::numeric_limits<double>::infinity();
       }},
      {"threshold-below-range",
       [](punto::SettingsData &value) { value.threshold = 0.49; }},
      {"threshold-above-range",
       [](punto::SettingsData &value) { value.threshold = 10.01; }},
      {"min-word-below-range",
       [](punto::SettingsData &value) { value.min_word_len = 0; }},
      {"min-word-above-range",
       [](punto::SettingsData &value) { value.min_word_len = 11; }},
      {"min-score-nan",
       [](punto::SettingsData &value) {
         value.min_score = std::numeric_limits<double>::quiet_NaN();
       }},
      {"min-score-positive-infinity",
       [](punto::SettingsData &value) {
         value.min_score = std::numeric_limits<double>::infinity();
       }},
      {"min-score-negative-infinity",
       [](punto::SettingsData &value) {
         value.min_score = -std::numeric_limits<double>::infinity();
       }},
      {"min-score-below-range",
       [](punto::SettingsData &value) { value.min_score = -0.01; }},
      {"min-score-above-range",
       [](punto::SettingsData &value) { value.min_score = 20.01; }},
      {"rollback-below-range",
       [](punto::SettingsData &value) { value.max_rollback_words = 0; }},
      {"rollback-above-range",
       [](punto::SettingsData &value) { value.max_rollback_words = 51; }},
      {"typo-distance-below-range",
       [](punto::SettingsData &value) { value.max_typo_diff = 0; }},
      {"typo-distance-above-range",
       [](punto::SettingsData &value) { value.max_typo_diff = 3; }},
      {"empty-modifier",
       [](punto::SettingsData &value) { value.modifier.clear(); }},
      {"unknown-modifier",
       [](punto::SettingsData &value) {
         value.modifier = "private-hyperctrl";
       }},
      {"newline-modifier",
       [](punto::SettingsData &value) {
         value.modifier = "leftctrl\nprivate_injected: true";
       }},
      {"empty-key", [](punto::SettingsData &value) { value.key.clear(); }},
      {"unknown-key",
       [](punto::SettingsData &value) { value.key = "private-mystery-key"; }},
      {"newline-key",
       [](punto::SettingsData &value) {
         value.key = "grave\nprivate_injected: true";
       }},
      {"same-modifier-and-key",
       [](punto::SettingsData &value) {
         value.modifier = "leftctrl";
         value.key = "leftctrl";
       }},
  };

  for (const InvalidSettingsCase &test_case : cases) {
    write_file(path, valid_config());
    const std::string before = read_file(path);
    punto::SettingsData invalid = edited_settings();
    test_case.mutate(invalid);

    bool saved = false;
    {
      ScopedHome scoped_home{home.string()};
      saved = punto::SettingsDialog::save_settings(invalid);
    }

    runner.expect(!saved, test_case.name + ": invalid UI value is rejected");
    runner.expect(read_file(path) == before,
                  test_case.name +
                      ": rejection leaves active config byte-identical");
    runner.expect(
        std::filesystem::is_regular_file(std::filesystem::symlink_status(path)),
        test_case.name + ": active config remains a regular file");
  }
}

} // namespace

int main() {
  try {
    TestRunner runner;
    TempDirectory directory;

    test_canonical_ui_defaults(runner);
    test_persistence_owner_is_process_free(runner);
    test_compatibility_hotkey_is_not_exposed_as_active_ui(runner);
    test_hostile_home_is_data_not_shell(runner, directory);
    test_malformed_config_does_not_crash(runner, directory);
    test_save_preserves_non_ui_fields(runner, directory);
    test_modifier_chords_save(runner, directory);
    test_inclusive_ui_boundaries_save(runner, directory);
    test_temp_path_race_is_safe(runner, directory);
    test_invalid_ui_values_are_atomic(runner, directory);

    if (runner.failures() != 0) {
      std::cerr << runner.failures()
                << " settings persistence contract assertion(s) failed\n";
      return 1;
    }
    std::cout << "settings persistence contract tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FATAL: " << error.what() << '\n';
    return 2;
  }
}
