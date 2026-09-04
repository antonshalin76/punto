/**
 * @file config.hpp
 * @brief Типобезопасная конфигурация Punto Switcher
 */

#pragma once

#include <linux/input.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "punto/types.hpp"

namespace punto {

namespace config_limits {

inline constexpr double kThresholdMin = 0.5;
inline constexpr double kThresholdMax = 10.0;
inline constexpr std::size_t kMinWordLengthMin = 1;
inline constexpr std::size_t kMinWordLengthMax = 10;
inline constexpr double kMinScoreMin = 0.0;
inline constexpr double kMinScoreMax = 20.0;
inline constexpr std::size_t kRollbackWordsMin = 1;
inline constexpr std::size_t kRollbackWordsMax = 50;
inline constexpr std::size_t kTypoDiffMin = 1;
inline constexpr std::size_t kTypoDiffMax = 2;
inline constexpr std::size_t kAnalysisThreadsMax = 128;
inline constexpr std::size_t kMaxAnalysisThreadsPerDaemonMin = 1;
inline constexpr std::size_t kMaxAnalysisThreadsPerDaemonMax = 128;
inline constexpr std::size_t kConfigFileMaxBytes = 64U * 1024U;

} // namespace config_limits

struct HotkeyConfig {
  std::uint16_t modifier = KEY_LEFTCTRL;
  std::uint16_t key = KEY_GRAVE;
};

struct AutoSwitchConfig {
  bool enabled = true;
  double threshold = 3.5;
  std::size_t min_word_len = 2;
  double min_score = 5.0;

  // Maximum number of recent words that a late correction may replay.
  std::size_t max_rollback_words = 5;
  bool typo_correction_enabled = false;
  std::size_t max_typo_diff = 2;
  bool sticky_shift_correction_enabled = true;
};

struct SoundConfig {
  bool enabled = true;
};

enum class LogLevel {
  Error = 0,
  Warning = 1,
  Info = 2,
  Debug = 3,
};

struct LoggingConfig {
  LogLevel level = LogLevel::Info;
};

struct RuntimeConfig {
  // Zero selects the automatic per-daemon CPU budget.
  std::size_t analysis_threads = 0;
  std::size_t max_analysis_threads_per_daemon = 4;
};

struct Config {
  HotkeyConfig hotkey;
  AutoSwitchConfig auto_switch;
  SoundConfig sound;
  LoggingConfig logging;
  RuntimeConfig runtime;
  std::filesystem::path config_path{"/etc/punto/config.yaml"};
};

/** Strict load: never falls back and returns a complete safe snapshot. */
struct ConfigLoadOutcome {
  Config config;
  ConfigResult result = ConfigResult::Ok;
  std::filesystem::path used_path;
  std::string error;
};

/**
 * Result of opening a config relative to a pinned allowed directory.
 * `path_allowed` is false when the requested path escapes the root or any
 * component is a symlink. The file is parsed from the verified descriptor.
 */
struct RestrictedConfigLoadOutcome {
  bool path_allowed = false;
  ConfigLoadOutcome load;
};

[[nodiscard]] ConfigLoadOutcome load_config_checked(std::filesystem::path path);

[[nodiscard]] RestrictedConfigLoadOutcome
load_config_beneath_checked(std::filesystem::path allowed_root,
                            std::filesystem::path requested_path);

/** Strict effective load: user config wins; only absence falls back to system.
 */
[[nodiscard]] ConfigLoadOutcome
load_effective_config_checked(std::filesystem::path system_path = kConfigPath);

/** Best-effort load; invalid or unreadable input produces documented defaults.
 */
[[nodiscard]] Config load_config(std::string_view path = kConfigPath);

/** Serializes all settings; config_path remains load metadata. */
[[nodiscard]] std::optional<std::string> serialize_config(const Config &config);

[[nodiscard]] bool validate_config(const Config &config);

} // namespace punto
