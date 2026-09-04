/**
 * @file config.cpp
 * @brief Реализация загрузчика конфигурации
 */

#include "punto/config.hpp"
#include "punto/scancode_map.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace punto {

namespace {

constexpr std::array<std::string_view, 5> kRootKeys = {
    "hotkey", "auto_switch", "sound", "logging", "runtime"};
constexpr std::array<std::string_view, 2> kHotkeyKeys = {"modifier", "key"};
constexpr std::array<std::string_view, 8> kAutoSwitchKeys = {
    "enabled",
    "threshold",
    "min_word_len",
    "min_score",
    "max_rollback_words",
    "typo_correction_enabled",
    "max_typo_diff",
    "sticky_shift_correction_enabled",
};
constexpr std::array<std::string_view, 1> kSoundKeys = {"enabled"};
constexpr std::array<std::string_view, 1> kLoggingKeys = {"level"};
constexpr std::array<std::string_view, 2> kRuntimeKeys = {
    "analysis_threads", "max_analysis_threads_per_daemon"};
constexpr std::array<std::pair<std::string_view, LogLevel>, 5> kLogLevels = {{
    {"error", LogLevel::Error},
    {"warning", LogLevel::Warning},
    {"info", LogLevel::Info},
    {"debug", LogLevel::Debug},
    {"warn", LogLevel::Warning},
}};

constexpr std::string_view kFileError = "Configuration file not found";
constexpr std::string_view kIoError = "Configuration file unreadable";
constexpr std::string_view kParseError = "Configuration parse error";
constexpr std::string_view kValueError = "Invalid configuration";

struct InvalidConfig final {};

[[noreturn]] void invalid_config() { throw InvalidConfig{}; }

class FileDescriptor final {
public:
  explicit FileDescriptor(int value) noexcept : value_{value} {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      (void)::close(value_);
    }
  }

  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;

  FileDescriptor(FileDescriptor &&other) noexcept
      : value_{std::exchange(other.value_, -1)} {}

  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        (void)::close(value_);
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }

private:
  int value_;
};

enum class SourceReadResult { Ok, Missing, IoError, TooLarge };

struct SourceReadOutcome {
  SourceReadResult result = SourceReadResult::IoError;
  std::string contents;
};

[[nodiscard]] int
open_absolute_directory_no_symlinks(const std::filesystem::path &path,
                                    int &open_error) {
  open_error = 0;
  int raw_fd = -1;
  do {
    raw_fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } while (raw_fd < 0 && errno == EINTR);
  if (raw_fd < 0) {
    open_error = errno;
    return -1;
  }

  FileDescriptor current{raw_fd};
  for (const auto &component : path.relative_path()) {
    const std::string name = component.native();
    if (name.empty() || name == "." || name == "..") {
      open_error = EINVAL;
      return -1;
    }

    int opened_fd = -1;
    do {
      opened_fd = ::openat(current.get(), name.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (opened_fd < 0 && errno == EINTR);
    if (opened_fd < 0) {
      open_error = errno;
      return -1;
    }
    current = FileDescriptor{opened_fd};
  }
  return current.release();
}

[[nodiscard]] SourceReadOutcome read_config_source_fd(int fd) {
  struct stat status {};
  int stat_result = -1;
  do {
    stat_result = ::fstat(fd, &status);
  } while (stat_result < 0 && errno == EINTR);
  if (stat_result < 0 || !S_ISREG(status.st_mode)) {
    return {};
  }
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) >
                                config_limits::kConfigFileMaxBytes) {
    return {SourceReadResult::TooLarge, {}};
  }

  SourceReadOutcome outcome{SourceReadResult::Ok, {}};
  outcome.contents.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096> buffer{};
  for (;;) {
    const std::size_t remaining =
        config_limits::kConfigFileMaxBytes + 1U - outcome.contents.size();
    const std::size_t request = std::min(buffer.size(), remaining);
    ssize_t bytes_read = -1;
    do {
      bytes_read = ::read(fd, buffer.data(), request);
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read < 0) {
      return {};
    }
    if (bytes_read == 0) {
      return outcome;
    }
    outcome.contents.append(buffer.data(),
                            static_cast<std::size_t>(bytes_read));
    if (outcome.contents.size() > config_limits::kConfigFileMaxBytes) {
      return {SourceReadResult::TooLarge, {}};
    }
  }
}

[[nodiscard]] SourceReadOutcome
read_config_source(const std::filesystem::path &path) {
  const std::string &native_path = path.native();
  if (native_path.find('\0') != std::string::npos) {
    return {};
  }

  int raw_fd = -1;
  do {
    raw_fd = ::open(native_path.c_str(),
                    O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  } while (raw_fd < 0 && errno == EINTR);
  if (raw_fd < 0) {
    return {errno == ENOENT ? SourceReadResult::Missing
                            : SourceReadResult::IoError,
            {}};
  }
  const FileDescriptor fd{raw_fd};
  return read_config_source_fd(fd.get());
}

template <std::size_t Size>
void require_schema(const YAML::Node &node,
                    const std::array<std::string_view, Size> &allowed_keys) {
  if (!node.IsMap()) {
    invalid_config();
  }

  std::array<bool, Size> seen{};
  for (const auto &entry : node) {
    if (!entry.first.IsScalar()) {
      invalid_config();
    }
    const auto found = std::find(allowed_keys.begin(), allowed_keys.end(),
                                 entry.first.Scalar());
    if (found == allowed_keys.end()) {
      invalid_config();
    }
    const auto index = static_cast<std::size_t>(found - allowed_keys.begin());
    if (seen[index]) {
      invalid_config();
    }
    seen[index] = true;
  }
}

template <std::size_t Size, typename Parser>
void parse_section(const YAML::Node &root, const char *name,
                   const std::array<std::string_view, Size> &allowed_keys,
                   Parser parser) {
  const YAML::Node section = root[name];
  if (section) {
    require_schema(section, allowed_keys);
    parser(section);
  }
}

template <typename Value>
void read_scalar(const YAML::Node &mapping, const char *key, Value &value) {
  const YAML::Node node = mapping[key];
  if (node) {
    if (!node.IsScalar()) {
      invalid_config();
    }
    value = node.as<Value>();
  }
}

void read_size(const YAML::Node &mapping, const char *key, std::size_t &value) {
  const YAML::Node node = mapping[key];
  if (!node) {
    return;
  }
  if (!node.IsScalar()) {
    invalid_config();
  }
  const long long parsed = node.as<long long>();
  if (!std::in_range<std::size_t>(parsed)) {
    invalid_config();
  }
  value = static_cast<std::size_t>(parsed);
}

void read_key(const YAML::Node &mapping, const char *key, bool require_modifier,
              std::uint16_t &value) {
  const YAML::Node node = mapping[key];
  if (!node) {
    return;
  }
  if (!node.IsScalar()) {
    invalid_config();
  }
  const auto code = key_name_to_code(node.Scalar());
  if (!code || (require_modifier && !is_modifier(*code))) {
    invalid_config();
  }
  value = *code;
}

void read_log_level(const YAML::Node &mapping, LogLevel &value) {
  const YAML::Node node = mapping["level"];
  if (!node) {
    return;
  }
  if (!node.IsScalar()) {
    invalid_config();
  }
  const auto found = std::find_if(
      kLogLevels.begin(), kLogLevels.end(),
      [&node](const auto &entry) { return entry.first == node.Scalar(); });
  if (found == kLogLevels.end()) {
    invalid_config();
  }
  value = found->second;
}

void parse_document(const YAML::Node &root, Config &config) {
  require_schema(root, kRootKeys);
  parse_section(root, "hotkey", kHotkeyKeys,
                [&config](const YAML::Node &section) {
                  read_key(section, "modifier", true, config.hotkey.modifier);
                  read_key(section, "key", false, config.hotkey.key);
                });
  parse_section(root, "auto_switch", kAutoSwitchKeys,
                [&config](const YAML::Node &section) {
                  auto &value = config.auto_switch;
                  read_scalar(section, "enabled", value.enabled);
                  read_scalar(section, "threshold", value.threshold);
                  read_size(section, "min_word_len", value.min_word_len);
                  read_scalar(section, "min_score", value.min_score);
                  read_size(section, "max_rollback_words",
                            value.max_rollback_words);
                  read_scalar(section, "typo_correction_enabled",
                              value.typo_correction_enabled);
                  read_size(section, "max_typo_diff", value.max_typo_diff);
                  read_scalar(section, "sticky_shift_correction_enabled",
                              value.sticky_shift_correction_enabled);
                });
  parse_section(root, "sound", kSoundKeys,
                [&config](const YAML::Node &section) {
                  read_scalar(section, "enabled", config.sound.enabled);
                });
  parse_section(root, "logging", kLoggingKeys,
                [&config](const YAML::Node &section) {
                  read_log_level(section, config.logging.level);
                });
  parse_section(
      root, "runtime", kRuntimeKeys, [&config](const YAML::Node &section) {
        read_size(section, "analysis_threads", config.runtime.analysis_threads);
        read_size(section, "max_analysis_threads_per_daemon",
                  config.runtime.max_analysis_threads_per_daemon);
      });
  if (!validate_config(config)) {
    invalid_config();
  }
}

[[nodiscard]] std::string get_user_config_path() {
  const char *home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return {};
  }
  return std::string{home} + "/" + std::string{kUserConfigRelPath};
}

[[nodiscard]] std::optional<std::string_view> log_level_name(LogLevel level) {
  const auto found = std::find_if(
      kLogLevels.begin(), kLogLevels.end(),
      [level](const auto &entry) { return entry.second == level; });
  return found == kLogLevels.end()
             ? std::nullopt
             : std::optional<std::string_view>{found->first};
}

template <typename Value>
[[nodiscard]] constexpr bool in_range(Value value, Value minimum,
                                      Value maximum) {
  return value >= minimum && value <= maximum;
}

void set_failure(ConfigLoadOutcome &outcome, ConfigResult result,
                 std::string_view error) {
  outcome.config = Config{};
  outcome.result = result;
  outcome.error.assign(error);
}

[[nodiscard]] ConfigLoadOutcome parse_config_source(std::filesystem::path path,
                                                    SourceReadOutcome source) {
  ConfigLoadOutcome outcome;
  outcome.used_path = std::move(path);

  if (source.result != SourceReadResult::Ok) {
    if (source.result == SourceReadResult::Missing) {
      set_failure(outcome, ConfigResult::FileNotFound, kFileError);
    } else if (source.result == SourceReadResult::TooLarge) {
      set_failure(outcome, ConfigResult::InvalidValue, kValueError);
    } else {
      set_failure(outcome, ConfigResult::IoError, kIoError);
    }
    return outcome;
  }

  try {
    const std::vector<YAML::Node> documents = YAML::LoadAll(source.contents);

    Config parsed;
    if (documents.size() > 1 ||
        (!documents.empty() && documents.front().IsNull())) {
      invalid_config();
    }
    if (!documents.empty()) {
      parse_document(documents.front(), parsed);
    }

    parsed.config_path = outcome.used_path;
    outcome.config = std::move(parsed);
    outcome.result = ConfigResult::Ok;
    outcome.error.clear();
  } catch (const YAML::ParserException &) {
    set_failure(outcome, ConfigResult::ParseError, kParseError);
  } catch (const InvalidConfig &) {
    set_failure(outcome, ConfigResult::InvalidValue, kValueError);
  } catch (const YAML::Exception &) {
    set_failure(outcome, ConfigResult::InvalidValue, kValueError);
  }
  return outcome;
}

[[nodiscard]] ConfigLoadOutcome
config_open_failure(std::filesystem::path requested_path, int error) {
  ConfigLoadOutcome outcome;
  outcome.used_path = std::move(requested_path);
  set_failure(outcome,
              error == ENOENT ? ConfigResult::FileNotFound
                              : ConfigResult::IoError,
              error == ENOENT ? kFileError : kIoError);
  return outcome;
}

[[nodiscard]] bool safe_relative_path(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute() || path == ".") {
    return false;
  }
  return std::none_of(path.begin(), path.end(), [](const auto &component) {
    return component.empty() || component == "." || component == "..";
  });
}

} // namespace

bool validate_config(const Config &config) {
  using namespace config_limits;

  const auto modifier = key_code_to_name(config.hotkey.modifier);
  const auto key = key_code_to_name(config.hotkey.key);
  if (!modifier || !is_modifier(config.hotkey.modifier) || !key ||
      config.hotkey.modifier == config.hotkey.key) {
    return false;
  }

  return std::isfinite(config.auto_switch.threshold) &&
         in_range(config.auto_switch.threshold, kThresholdMin, kThresholdMax) &&
         in_range(config.auto_switch.min_word_len, kMinWordLengthMin,
                  kMinWordLengthMax) &&
         std::isfinite(config.auto_switch.min_score) &&
         in_range(config.auto_switch.min_score, kMinScoreMin, kMinScoreMax) &&
         in_range(config.auto_switch.max_rollback_words, kRollbackWordsMin,
                  kRollbackWordsMax) &&
         in_range(config.auto_switch.max_typo_diff, kTypoDiffMin,
                  kTypoDiffMax) &&
         config.runtime.analysis_threads <= kAnalysisThreadsMax &&
         in_range(config.runtime.max_analysis_threads_per_daemon,
                  kMaxAnalysisThreadsPerDaemonMin,
                  kMaxAnalysisThreadsPerDaemonMax) &&
         log_level_name(config.logging.level).has_value();
}

ConfigLoadOutcome load_config_checked(std::filesystem::path path) {
  if (path.empty()) {
    ConfigLoadOutcome outcome;
    outcome.used_path = std::move(path);
    set_failure(outcome, ConfigResult::FileNotFound, kFileError);
    return outcome;
  }
  SourceReadOutcome source = read_config_source(path);
  return parse_config_source(std::move(path), std::move(source));
}

RestrictedConfigLoadOutcome
load_config_beneath_checked(std::filesystem::path allowed_root,
                            std::filesystem::path requested_path) {
  RestrictedConfigLoadOutcome outcome;
  outcome.load.used_path = requested_path;

  const std::string &root_native = allowed_root.native();
  const std::string &requested_native = requested_path.native();
  if (root_native.find('\0') != std::string::npos ||
      requested_native.find('\0') != std::string::npos ||
      !allowed_root.is_absolute() || !requested_path.is_absolute()) {
    return outcome;
  }

  allowed_root = allowed_root.lexically_normal();
  requested_path = requested_path.lexically_normal();
  const std::filesystem::path relative =
      requested_path.lexically_relative(allowed_root);
  if (!safe_relative_path(relative)) {
    return outcome;
  }

  int open_error = 0;
  const int root_fd =
      open_absolute_directory_no_symlinks(allowed_root, open_error);
  if (root_fd < 0) {
    outcome.path_allowed = open_error != ELOOP && open_error != ENOTDIR;
    outcome.load = config_open_failure(std::move(requested_path), open_error);
    return outcome;
  }

  FileDescriptor current{root_fd};
  for (auto component = relative.begin(); component != relative.end();
       ++component) {
    const bool final_component = std::next(component) == relative.end();
    const int flags =
        final_component
            ? O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW
            : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW;
    int opened_fd = -1;
    do {
      opened_fd = ::openat(current.get(), component->c_str(), flags);
    } while (opened_fd < 0 && errno == EINTR);
    if (opened_fd < 0) {
      const int open_error = errno;
      outcome.path_allowed = open_error != ELOOP && open_error != ENOTDIR;
      outcome.load = config_open_failure(std::move(requested_path), open_error);
      return outcome;
    }
    current = FileDescriptor{opened_fd};
  }

  outcome.path_allowed = true;
  outcome.load = parse_config_source(std::move(requested_path),
                                     read_config_source_fd(current.get()));
  return outcome;
}

ConfigLoadOutcome
load_effective_config_checked(std::filesystem::path system_path) {
  const std::string user_path = get_user_config_path();
  if (!user_path.empty()) {
    ConfigLoadOutcome user = load_config_checked(user_path);
    if (user.result != ConfigResult::FileNotFound) {
      return user;
    }
  }
  return load_config_checked(std::move(system_path));
}

Config load_config(std::string_view path) {
  const bool use_effective_path = path == kConfigPath;
  ConfigLoadOutcome outcome =
      use_effective_path
          ? load_effective_config_checked(std::filesystem::path{path})
          : load_config_checked(std::filesystem::path{path});
  if (outcome.result != ConfigResult::Ok) {
    std::cerr << "[punto] Warning: " << outcome.error << '\n';
    return Config{};
  }
  if (use_effective_path && outcome.used_path != std::filesystem::path{path}) {
    std::cerr << "[punto] Using user configuration\n";
  }
  return outcome.config;
}

std::optional<std::string> serialize_config(const Config &config) {
  if (!validate_config(config)) {
    return std::nullopt;
  }

  const auto modifier = key_code_to_name(config.hotkey.modifier);
  const auto key = key_code_to_name(config.hotkey.key);
  const auto level = log_level_name(config.logging.level);
  if (!modifier || !key || !level) {
    return std::nullopt;
  }

  YAML::Node root;
  root["hotkey"]["modifier"] = std::string{*modifier};
  root["hotkey"]["key"] = std::string{*key};
  root["auto_switch"]["enabled"] = config.auto_switch.enabled;
  root["auto_switch"]["threshold"] = config.auto_switch.threshold;
  root["auto_switch"]["min_word_len"] = config.auto_switch.min_word_len;
  root["auto_switch"]["min_score"] = config.auto_switch.min_score;
  root["auto_switch"]["max_rollback_words"] =
      config.auto_switch.max_rollback_words;
  root["auto_switch"]["typo_correction_enabled"] =
      config.auto_switch.typo_correction_enabled;
  root["auto_switch"]["max_typo_diff"] = config.auto_switch.max_typo_diff;
  root["auto_switch"]["sticky_shift_correction_enabled"] =
      config.auto_switch.sticky_shift_correction_enabled;
  root["sound"]["enabled"] = config.sound.enabled;
  root["logging"]["level"] = std::string{*level};
  root["runtime"]["analysis_threads"] = config.runtime.analysis_threads;
  root["runtime"]["max_analysis_threads_per_daemon"] =
      config.runtime.max_analysis_threads_per_daemon;

  YAML::Emitter output;
  output.SetDoublePrecision(std::numeric_limits<double>::max_digits10);
  output << root;

  if (!output.good()) {
    return std::nullopt;
  }
  return std::string{output.c_str()};
}

} // namespace punto
