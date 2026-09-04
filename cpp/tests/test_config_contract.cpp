#include "punto/config.hpp"
#include "punto/scancode_map.hpp"

#include <linux/input-event-codes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PUNTO_SOURCE_DIR
#define PUNTO_SOURCE_DIR "."
#endif

namespace {

using punto::Config;
using punto::ConfigLoadOutcome;
using punto::ConfigResult;
using punto::LogLevel;

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

class TempDirectory {
public:
  TempDirectory() {
    char path[] = "/tmp/punto-config-contract-PRIVATE_ROOT_XXXXXX";
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

class ScopedHome final {
public:
  explicit ScopedHome(const std::filesystem::path &path) {
    const char *current = std::getenv("HOME");
    if (current != nullptr) {
      had_original_ = true;
      original_ = current;
    }
    if (::setenv("HOME", path.c_str(), 1) != 0) {
      throw std::system_error{errno, std::generic_category(), "setenv failed"};
    }
  }

  ~ScopedHome() {
    if (had_original_) {
      (void)::setenv("HOME", original_.c_str(), 1);
    } else {
      (void)::unsetenv("HOME");
    }
  }

  ScopedHome(const ScopedHome &) = delete;
  ScopedHome &operator=(const ScopedHome &) = delete;

private:
  bool had_original_ = false;
  std::string original_;
};

[[nodiscard]] bool same_config(const Config &left, const Config &right) {
  return left.hotkey.modifier == right.hotkey.modifier &&
         left.hotkey.key == right.hotkey.key &&
         left.auto_switch.enabled == right.auto_switch.enabled &&
         left.auto_switch.threshold == right.auto_switch.threshold &&
         left.auto_switch.min_word_len == right.auto_switch.min_word_len &&
         left.auto_switch.min_score == right.auto_switch.min_score &&
         left.auto_switch.max_rollback_words ==
             right.auto_switch.max_rollback_words &&
         left.auto_switch.typo_correction_enabled ==
             right.auto_switch.typo_correction_enabled &&
         left.auto_switch.max_typo_diff == right.auto_switch.max_typo_diff &&
         left.auto_switch.sticky_shift_correction_enabled ==
             right.auto_switch.sticky_shift_correction_enabled &&
         left.sound.enabled == right.sound.enabled &&
         left.logging.level == right.logging.level &&
         left.runtime.analysis_threads == right.runtime.analysis_threads &&
         left.runtime.max_analysis_threads_per_daemon ==
             right.runtime.max_analysis_threads_per_daemon &&
         left.config_path == right.config_path;
}

[[nodiscard]] Config documented_defaults() {
  Config config;
  config.hotkey.modifier = KEY_LEFTCTRL;
  config.hotkey.key = KEY_GRAVE;
  config.auto_switch.enabled = true;
  config.auto_switch.threshold = 3.5;
  config.auto_switch.min_word_len = 2;
  config.auto_switch.min_score = 5.0;
  config.auto_switch.max_rollback_words = 5;
  config.auto_switch.typo_correction_enabled = false;
  config.auto_switch.max_typo_diff = 2;
  config.auto_switch.sticky_shift_correction_enabled = true;
  config.sound.enabled = true;
  config.logging.level = LogLevel::Info;
  config.runtime.analysis_threads = 0;
  config.runtime.max_analysis_threads_per_daemon = 4;
  config.config_path = "/etc/punto/config.yaml";
  return config;
}

[[nodiscard]] std::filesystem::path
write_config(const std::filesystem::path &directory, std::size_t sequence,
             std::string_view contents) {
  const std::string suffix = std::to_string(sequence);
  const std::filesystem::path parent = directory / ("PRIVATE_PARENT_" + suffix);
  std::error_code create_error;
  std::filesystem::create_directories(parent, create_error);
  if (create_error) {
    throw std::runtime_error{"cannot create temporary config directory"};
  }
  const std::filesystem::path path =
      parent / ("PRIVATE_CONFIG_" + suffix + ".yaml");
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output.is_open()) {
    throw std::runtime_error{"cannot create temporary config"};
  }
  output << contents;
  if (!output.good()) {
    throw std::runtime_error{"cannot write temporary config"};
  }
  return path;
}

[[nodiscard]] std::string replace_once(std::string source,
                                       std::string_view needle,
                                       std::string_view replacement) {
  const std::size_t position = source.find(needle);
  if (position == std::string::npos) {
    throw std::runtime_error{"test mutation needle not found"};
  }
  source.replace(position, needle.size(), replacement);
  return source;
}

[[nodiscard]] std::string result_name(ConfigResult result) {
  switch (result) {
  case ConfigResult::Ok:
    return "Ok";
  case ConfigResult::FileNotFound:
    return "FileNotFound";
  case ConfigResult::IoError:
    return "IoError";
  case ConfigResult::ParseError:
    return "ParseError";
  case ConfigResult::InvalidValue:
    return "InvalidValue";
  }
  return "Unknown";
}

[[nodiscard]] bool error_is_categorical(ConfigResult result,
                                        std::string error) {
  std::transform(error.begin(), error.end(), error.begin(), [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });

  switch (result) {
  case ConfigResult::FileNotFound:
    return error.find("not found") != std::string::npos;
  case ConfigResult::IoError:
    return error.find("unreadable") != std::string::npos ||
           error.find("cannot open") != std::string::npos ||
           error.find("i/o") != std::string::npos;
  case ConfigResult::ParseError:
    return error.find("parse") != std::string::npos ||
           error.find("syntax") != std::string::npos ||
           error.find("malformed") != std::string::npos;
  case ConfigResult::InvalidValue:
    return error.find("invalid") != std::string::npos ||
           error.find("unknown") != std::string::npos ||
           error.find("duplicate") != std::string::npos ||
           error.find("range") != std::string::npos ||
           error.find("schema") != std::string::npos;
  case ConfigResult::Ok:
    return false;
  }
  return false;
}

[[nodiscard]] bool contains_control_byte(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](char ch) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    return byte < 0x20U || byte == 0x7fU;
  });
}

[[nodiscard]] std::vector<std::string>
private_tokens(std::string_view yaml_fragment) {
  std::vector<std::string> tokens;
  constexpr std::string_view prefix = "PRIVATE_";
  std::size_t start = yaml_fragment.find(prefix);
  while (start != std::string_view::npos) {
    std::size_t end = start + prefix.size();
    while (end < yaml_fragment.size()) {
      const unsigned char ch = static_cast<unsigned char>(yaml_fragment[end]);
      if (std::isalnum(ch) == 0 && ch != '_' && ch != '-') {
        break;
      }
      ++end;
    }
    tokens.emplace_back(yaml_fragment.substr(start, end - start));
    start = yaml_fragment.find(prefix, end);
  }
  return tokens;
}

void expect_safe_error_snapshot(TestRunner &runner, std::string_view case_name,
                                const ConfigLoadOutcome &outcome,
                                const std::filesystem::path &path) {
  const std::string prefix{case_name};
  runner.expect(same_config(outcome.config, documented_defaults()),
                prefix + ": rejected input returns the complete safe snapshot");
  runner.expect(outcome.used_path == path,
                prefix +
                    ": rejected input reports the attempted path separately");
}

void expect_ok_contract(TestRunner &runner, std::string_view case_name,
                        const ConfigLoadOutcome &outcome,
                        const std::filesystem::path &path,
                        const Config &expected) {
  const std::string prefix{case_name};
  runner.expect(outcome.result == ConfigResult::Ok,
                prefix + ": strict load returns Ok");
  runner.expect(same_config(outcome.config, expected),
                prefix +
                    ": strict load returns the complete expected snapshot");
  runner.expect(outcome.used_path == path,
                prefix + ": successful load reports the selected path");
  runner.expect(outcome.error.empty(),
                prefix + ": successful load has no diagnostic");
}

void expect_error_contract(
    TestRunner &runner, std::string_view case_name,
    const ConfigLoadOutcome &outcome, ConfigResult expected_result,
    const std::filesystem::path &path,
    const std::vector<std::string> &payload_tokens = {}) {
  const std::string prefix{case_name};
  runner.expect(!outcome.error.empty(), prefix + ": diagnostic is nonempty");
  runner.expect(outcome.error.size() <= 160,
                prefix + ": diagnostic is bounded to 160 bytes");
  runner.expect(!contains_control_byte(outcome.error),
                prefix + ": diagnostic contains no control bytes");

  const std::vector<std::string> path_tokens = {
      path.string(),     "PRIVATE_ROOT_",    "PRIVATE_PARENT_",
      "PRIVATE_CONFIG_", "PRIVATE_MISSING_", "PRIVATE_EFFECTIVE_HOME"};
  for (const std::string &token : path_tokens) {
    if (!token.empty()) {
      runner.expect(outcome.error.find(token) == std::string::npos,
                    prefix + ": diagnostic does not disclose a path token");
    }
  }
  for (const std::string &token : payload_tokens) {
    if (!token.empty()) {
      runner.expect(outcome.error.find(token) == std::string::npos,
                    prefix + ": diagnostic does not echo a payload token");
    }
  }
  runner.expect(error_is_categorical(expected_result, outcome.error),
                prefix + ": diagnostic identifies the error category");
  expect_safe_error_snapshot(runner, case_name, outcome, path);
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
  level: info
runtime:
  analysis_threads: 0
  max_analysis_threads_per_daemon: 4
)";
}

void test_compiled_defaults(TestRunner &runner) {
  runner.expect(same_config(Config{}, documented_defaults()),
                "compiled defaults match the documented shipped template");
}

void test_shipped_config(TestRunner &runner) {
  const std::filesystem::path shipped_path =
      std::filesystem::path{PUNTO_SOURCE_DIR} / "config.yaml";
  const ConfigLoadOutcome outcome = punto::load_config_checked(shipped_path);
  Config expected = documented_defaults();
  expected.config_path = shipped_path;
  expect_ok_contract(runner, "shipped-config", outcome, shipped_path, expected);
}

void test_inline_comments(TestRunner &runner,
                          const std::filesystem::path &directory) {
  const std::string contents = R"(hotkey: # layout toggle
  modifier: rightalt # modifier enum
  key: space # key enum
auto_switch: # correction policy
  enabled: false # boolean
  threshold: 4.5 # floating point
  min_word_len: 4 # integer
  min_score: 7.25 # floating point
  max_rollback_words: 9 # integer
  typo_correction_enabled: false # boolean
  max_typo_diff: 1 # integer
  sticky_shift_correction_enabled: false # boolean
sound:
  enabled: false # boolean
logging:
  level: debug # enum
runtime:
  analysis_threads: 3 # integer
  max_analysis_threads_per_daemon: 2 # integer
)";
  const std::filesystem::path path = write_config(directory, 0, contents);
  const ConfigLoadOutcome outcome = punto::load_config_checked(path);

  Config expected;
  expected.hotkey.modifier = KEY_RIGHTALT;
  expected.hotkey.key = KEY_SPACE;
  expected.auto_switch.enabled = false;
  expected.auto_switch.threshold = 4.5;
  expected.auto_switch.min_word_len = 4;
  expected.auto_switch.min_score = 7.25;
  expected.auto_switch.max_rollback_words = 9;
  expected.auto_switch.typo_correction_enabled = false;
  expected.auto_switch.max_typo_diff = 1;
  expected.auto_switch.sticky_shift_correction_enabled = false;
  expected.sound.enabled = false;
  expected.logging.level = LogLevel::Debug;
  expected.runtime.analysis_threads = 3;
  expected.runtime.max_analysis_threads_per_daemon = 2;
  expected.config_path = path;
  expect_ok_contract(runner, "inline-comments", outcome, path, expected);
}

void test_modifier_keys_as_second_chord_member(
    TestRunner &runner, const std::filesystem::path &directory) {
  constexpr std::array<std::string_view, 8> keys = {
      "leftctrl",  "rightctrl",  "leftalt",  "rightalt",
      "leftshift", "rightshift", "leftmeta", "rightmeta"};

  for (std::size_t index = 0; index < keys.size(); ++index) {
    const std::string_view key_name = keys[index];
    const std::string_view modifier_name =
        key_name == "leftctrl" ? "rightalt" : "leftctrl";
    std::string contents =
        replace_once(valid_config(), "  modifier: leftctrl\n",
                     "  modifier: " + std::string{modifier_name} + "\n");
    contents = replace_once(contents, "  key: grave\n",
                            "  key: " + std::string{key_name} + "\n");
    const std::filesystem::path path =
        write_config(directory, 40 + index, contents);
    const ConfigLoadOutcome loaded = punto::load_config_checked(path);
    const auto modifier = punto::key_name_to_code(modifier_name);
    const auto key = punto::key_name_to_code(key_name);

    runner.expect(modifier.has_value() && key.has_value(),
                  "modifier-key chord names resolve through canonical map");
    runner.expect(loaded.result == ConfigResult::Ok,
                  std::string{key_name} +
                      ": documented modifier key is accepted as chord key");
    if (loaded.result != ConfigResult::Ok || !modifier || !key) {
      continue;
    }
    runner.expect(loaded.config.hotkey.modifier == *modifier &&
                      loaded.config.hotkey.key == *key,
                  std::string{key_name} + ": chord codes are preserved");
    const std::optional<std::string> serialized =
        punto::serialize_config(loaded.config);
    runner.expect(serialized.has_value(),
                  std::string{key_name} + ": chord serializes");
    if (!serialized) {
      continue;
    }
    const std::filesystem::path roundtrip_path =
        write_config(directory, 60 + index, *serialized);
    const ConfigLoadOutcome roundtrip =
        punto::load_config_checked(roundtrip_path);
    runner.expect(roundtrip.result == ConfigResult::Ok &&
                      roundtrip.config.hotkey.modifier == *modifier &&
                      roundtrip.config.hotkey.key == *key,
                  std::string{key_name} + ": chord round-trips");
  }
}

void test_serializer_contract(TestRunner &runner,
                              const std::filesystem::path &directory) {
  constexpr std::array<LogLevel, 4> levels = {
      LogLevel::Error, LogLevel::Warning, LogLevel::Info, LogLevel::Debug};
  for (std::size_t index = 0; index < levels.size(); ++index) {
    Config source;
    source.logging.level = levels[index];
    source.auto_switch.threshold = 3.5000000000000004;
    source.auto_switch.min_score = 7.1250000000000009;
    source.config_path = directory / "PRIVATE_SERIALIZER_METADATA";
    const std::optional<std::string> serialized =
        punto::serialize_config(source);
    runner.expect(serialized.has_value(), "valid config serializes");
    if (!serialized) {
      continue;
    }
    runner.expect(serialized->find("PRIVATE_SERIALIZER_METADATA") ==
                      std::string::npos,
                  "serializer excludes load-path metadata");
    const std::filesystem::path path =
        write_config(directory, 80 + index, *serialized);
    const ConfigLoadOutcome loaded = punto::load_config_checked(path);
    runner.expect(loaded.result == ConfigResult::Ok,
                  "serialized config parses strictly");
    runner.expect(loaded.config.logging.level == levels[index] &&
                      loaded.config.auto_switch.threshold ==
                          source.auto_switch.threshold &&
                      loaded.config.auto_switch.min_score ==
                          source.auto_switch.min_score,
                  "serializer preserves enum and double precision");
  }

  Config invalid;
  invalid.auto_switch.threshold = std::numeric_limits<double>::quiet_NaN();
  runner.expect(!punto::serialize_config(invalid).has_value(),
                "serializer rejects an invalid typed snapshot");
}

void test_empty_partial_and_legacy_configs(
    TestRunner &runner, const std::filesystem::path &directory) {
  std::size_t sequence = 100;
  const auto expect_valid = [&](std::string_view name,
                                std::string_view contents, Config expected) {
    const std::filesystem::path path =
        write_config(directory, sequence++, contents);
    expected.config_path = path;
    const ConfigLoadOutcome outcome = punto::load_config_checked(path);
    expect_ok_contract(runner, name, outcome, path, expected);
  };

  expect_valid("empty-config", "", documented_defaults());

  Config partial = documented_defaults();
  partial.sound.enabled = false;
  expect_valid("partial-config", "sound:\n  enabled: false\n", partial);

  Config legacy = documented_defaults();
  legacy.hotkey.modifier = KEY_RIGHTALT;
  legacy.hotkey.key = KEY_SPACE;
  legacy.auto_switch.enabled = false;
  legacy.auto_switch.threshold = 4.5;
  legacy.auto_switch.min_word_len = 4;
  legacy.auto_switch.min_score = 7.25;
  legacy.auto_switch.max_rollback_words = 9;
  legacy.sound.enabled = false;
  expect_valid("legacy-config-without-newer-keys",
               R"(hotkey:
  modifier: rightalt
  key: space
auto_switch:
  enabled: false
  threshold: 4.5
  min_word_len: 4
  min_score: 7.25
  max_rollback_words: 9
sound:
  enabled: false
)",
               legacy);
}

void test_inclusive_valid_boundaries(TestRunner &runner,
                                     const std::filesystem::path &directory) {
  const auto expect_valid = [&](std::size_t sequence, std::string_view name,
                                std::string_view contents, Config expected) {
    const std::filesystem::path path =
        write_config(directory, sequence, contents);
    expected.config_path = path;
    const ConfigLoadOutcome outcome = punto::load_config_checked(path);
    expect_ok_contract(runner, name, outcome, path, expected);
  };

  Config lower = documented_defaults();
  lower.auto_switch.threshold = 0.5;
  lower.auto_switch.min_word_len = 1;
  lower.auto_switch.min_score = 0.0;
  lower.auto_switch.max_rollback_words = 1;
  lower.auto_switch.max_typo_diff = 1;
  lower.runtime.analysis_threads = 0;
  lower.runtime.max_analysis_threads_per_daemon = 1;
  expect_valid(
      200, "lower-boundaries",
      replace_once(
          replace_once(
              replace_once(
                  replace_once(replace_once(replace_once(valid_config(),
                                                         "  threshold: 3.5\n",
                                                         "  threshold: 0.5\n"),
                                            "  min_word_len: 2\n",
                                            "  min_word_len: 1\n"),
                               "  min_score: 5.0\n", "  min_score: 0.0\n"),
                  "  max_rollback_words: 5\n", "  max_rollback_words: 1\n"),
              "  max_typo_diff: 2\n", "  max_typo_diff: 1\n"),
          "  max_analysis_threads_per_daemon: 4\n",
          "  max_analysis_threads_per_daemon: 1\n"),
      lower);

  Config upper = documented_defaults();
  upper.auto_switch.threshold = 10.0;
  upper.auto_switch.min_word_len = 10;
  upper.auto_switch.min_score = 20.0;
  upper.auto_switch.max_rollback_words = 50;
  upper.auto_switch.max_typo_diff = 2;
  upper.runtime.analysis_threads = 128;
  upper.runtime.max_analysis_threads_per_daemon = 128;
  expect_valid(
      201, "upper-boundaries",
      replace_once(
          replace_once(
              replace_once(
                  replace_once(replace_once(replace_once(valid_config(),
                                                         "  threshold: 3.5\n",
                                                         "  threshold: 10.0\n"),
                                            "  min_word_len: 2\n",
                                            "  min_word_len: 10\n"),
                               "  min_score: 5.0\n", "  min_score: 20.0\n"),
                  "  max_rollback_words: 5\n", "  max_rollback_words: 50\n"),
              "  analysis_threads: 0\n", "  analysis_threads: 128\n"),
          "  max_analysis_threads_per_daemon: 4\n",
          "  max_analysis_threads_per_daemon: 128\n"),
      upper);
}

struct InvalidCase {
  std::string name;
  std::string contents;
  ConfigResult expected_result = ConfigResult::InvalidValue;
  std::vector<std::string> payload_tokens;
};

[[nodiscard]] std::vector<InvalidCase> invalid_cases() {
  const std::string base = valid_config();
  const auto mutate = [&base](std::string name, std::string_view needle,
                              std::string_view replacement,
                              ConfigResult expected =
                                  ConfigResult::InvalidValue,
                              std::string hostile_scalar = {}) {
    std::vector<std::string> tokens = private_tokens(replacement);
    if (!hostile_scalar.empty()) {
      tokens.push_back(std::move(hostile_scalar));
    }
    return InvalidCase{std::move(name), replace_once(base, needle, replacement),
                       expected, std::move(tokens)};
  };

  std::vector<InvalidCase> cases;
  cases.push_back(
      {"unknown-section",
       base + "PRIVATE_UNKNOWN_SECTION_001:\n  value: 712345678901\n",
       ConfigResult::InvalidValue,
       {"PRIVATE_UNKNOWN_SECTION_001", "712345678901"}});
  cases.push_back(mutate("unknown-key", "  threshold: 3.5\n",
                         "  threshold: 3.5\n"
                         "  PRIVATE_UNKNOWN_KEY_002: true\n"));
  cases.push_back(mutate("duplicate-key", "  enabled: true\n",
                         "  enabled: &PRIVATE_DUPLICATE_KEY_003 true\n"
                         "  enabled: false\n"));
  cases.push_back(
      {"duplicate-section",
       base + "sound: &PRIVATE_DUPLICATE_SECTION_004\n  enabled: false\n",
       ConfigResult::InvalidValue,
       {"PRIVATE_DUPLICATE_SECTION_004"}});
  cases.push_back(mutate("malformed-yaml-line", "  enabled: true\n",
                         "  PRIVATE_MALFORMED_LINE_005 enabled true\n",
                         ConfigResult::ParseError));
  cases.push_back(mutate("malformed-flow-scalar", "  threshold: 3.5\n",
                         "  threshold: [PRIVATE_MALFORMED_FLOW_006\n",
                         ConfigResult::ParseError));
  cases.push_back(mutate("misplaced-known-key", "sound:\n",
                         "sound:\n  threshold: 8.765432109\n",
                         ConfigResult::InvalidValue, "8.765432109"));
  cases.push_back(mutate("wrong-indentation", "  enabled: true\n",
                         "enabled: &PRIVATE_WRONG_INDENT_008 true\n",
                         ConfigResult::ParseError));
  cases.push_back(
      {"root-sequence",
       "- PRIVATE_ROOT_SEQUENCE_009_A\n- PRIVATE_ROOT_SEQUENCE_009_B\n",
       ConfigResult::InvalidValue,
       {"PRIVATE_ROOT_SEQUENCE_009_A", "PRIVATE_ROOT_SEQUENCE_009_B"}});
  cases.push_back({"scalar-section",
                   "auto_switch: PRIVATE_SCALAR_SECTION_010\n",
                   ConfigResult::InvalidValue,
                   {"PRIVATE_SCALAR_SECTION_010"}});

  cases.push_back(mutate("malformed-bool", "  enabled: true\n",
                         "  enabled: PRIVATE_BOOL_011\n"));
  cases.push_back(mutate("empty-bool", "  enabled: true\n",
                         "  enabled: &PRIVATE_EMPTY_BOOL_012\n"));
  cases.push_back(mutate("malformed-sound-bool", "sound:\n  enabled: true\n",
                         "sound:\n  enabled: PRIVATE_SOUND_BOOL_013\n"));
  cases.push_back(mutate("empty-sticky-bool",
                         "  sticky_shift_correction_enabled: true\n",
                         "  sticky_shift_correction_enabled: "
                         "&PRIVATE_EMPTY_STICKY_BOOL_014\n"));

  cases.push_back(mutate("malformed-double", "  threshold: 3.5\n",
                         "  threshold: PRIVATE_DOUBLE_015\n"));
  constexpr std::string_view hostile_scalar =
      "DO_NOT_ECHO_THIS_HOSTILE_CONFIGURATION_VALUE";
  cases.push_back(
      mutate("hostile-scalar", "  threshold: 3.5\n",
             std::string{"  threshold: "} + std::string{hostile_scalar} + "\n",
             ConfigResult::InvalidValue, std::string{hostile_scalar}));
  cases.push_back(mutate("empty-double", "  min_score: 5.0\n",
                         "  min_score: &PRIVATE_EMPTY_DOUBLE_017\n"));
  cases.push_back(mutate("malformed-integer", "  min_word_len: 2\n",
                         "  min_word_len: PRIVATE_INTEGER_018\n"));
  cases.push_back(mutate("empty-integer", "  max_typo_diff: 2\n",
                         "  max_typo_diff: &PRIVATE_EMPTY_INTEGER_019\n"));

  cases.push_back(mutate("invalid-modifier-enum", "  modifier: leftctrl\n",
                         "  modifier: PRIVATE_MODIFIER_020\n"));
  cases.push_back(mutate("empty-key-enum", "  key: grave\n",
                         "  key: &PRIVATE_EMPTY_KEY_021\n"));
  cases.push_back(mutate("invalid-log-level-enum", "  level: info\n",
                         "  level: PRIVATE_LOG_LEVEL_022\n"));

  cases.push_back(mutate("nan-threshold", "  threshold: 3.5\n",
                         "  threshold: .NaN\n", ConfigResult::InvalidValue,
                         ".NaN"));
  cases.push_back(mutate("nan-min-score", "  min_score: 5.0\n",
                         "  min_score: .nan\n", ConfigResult::InvalidValue,
                         ".nan"));
  cases.push_back(mutate("positive-infinity", "  threshold: 3.5\n",
                         "  threshold: .inf\n", ConfigResult::InvalidValue,
                         ".inf"));
  cases.push_back(mutate("negative-infinity", "  min_score: 5.0\n",
                         "  min_score: -.Inf\n", ConfigResult::InvalidValue,
                         "-.Inf"));

  cases.push_back(mutate("negative-threshold", "  threshold: 3.5\n",
                         "  threshold: -101.500001\n",
                         ConfigResult::InvalidValue, "-101.500001"));
  cases.push_back(mutate("negative-min-word", "  min_word_len: 2\n",
                         "  min_word_len: -102001\n",
                         ConfigResult::InvalidValue, "-102001"));
  cases.push_back(mutate("negative-min-score", "  min_score: 5.0\n",
                         "  min_score: -103.000103\n",
                         ConfigResult::InvalidValue, "-103.000103"));
  cases.push_back(mutate("negative-rollback", "  max_rollback_words: 5\n",
                         "  max_rollback_words: -104004\n",
                         ConfigResult::InvalidValue, "-104004"));
  cases.push_back(mutate("negative-typo-diff", "  max_typo_diff: 2\n",
                         "  max_typo_diff: -105005\n",
                         ConfigResult::InvalidValue, "-105005"));
  cases.push_back(mutate("negative-analysis-threads", "  analysis_threads: 0\n",
                         "  analysis_threads: -106006\n",
                         ConfigResult::InvalidValue, "-106006"));
  cases.push_back(mutate("negative-max-analysis-threads",
                         "  max_analysis_threads_per_daemon: 4\n",
                         "  max_analysis_threads_per_daemon: -107007\n",
                         ConfigResult::InvalidValue, "-107007"));

  cases.push_back(mutate("threshold-below-range", "  threshold: 3.5\n",
                         "  threshold: 0.4123456789\n",
                         ConfigResult::InvalidValue, "0.4123456789"));
  cases.push_back(mutate("threshold-above-range", "  threshold: 3.5\n",
                         "  threshold: 10.123456789\n",
                         ConfigResult::InvalidValue, "10.123456789"));
  cases.push_back(mutate("min-word-below-range", "  min_word_len: 2\n",
                         "  min_word_len: 000000000000\n",
                         ConfigResult::InvalidValue, "000000000000"));
  cases.push_back(mutate("min-word-above-range", "  min_word_len: 2\n",
                         "  min_word_len: 110011\n", ConfigResult::InvalidValue,
                         "110011"));
  cases.push_back(mutate("min-score-above-range", "  min_score: 5.0\n",
                         "  min_score: 20.123456789\n",
                         ConfigResult::InvalidValue, "20.123456789"));
  cases.push_back(mutate("rollback-below-range", "  max_rollback_words: 5\n",
                         "  max_rollback_words: 0000000000000\n",
                         ConfigResult::InvalidValue, "0000000000000"));
  cases.push_back(mutate("rollback-above-range", "  max_rollback_words: 5\n",
                         "  max_rollback_words: 510051\n",
                         ConfigResult::InvalidValue, "510051"));
  cases.push_back(mutate("typo-diff-below-range", "  max_typo_diff: 2\n",
                         "  max_typo_diff: 00000000000000\n",
                         ConfigResult::InvalidValue, "00000000000000"));
  cases.push_back(mutate("typo-diff-above-range", "  max_typo_diff: 2\n",
                         "  max_typo_diff: 300003\n",
                         ConfigResult::InvalidValue, "300003"));
  cases.push_back(mutate(
      "analysis-threads-above-range", "  analysis_threads: 0\n",
      "  analysis_threads: 129129\n", ConfigResult::InvalidValue, "129129"));
  cases.push_back(mutate("max-analysis-threads-below-range",
                         "  max_analysis_threads_per_daemon: 4\n",
                         "  max_analysis_threads_per_daemon: 000000000000\n",
                         ConfigResult::InvalidValue, "000000000000"));
  cases.push_back(mutate("max-analysis-threads-above-range",
                         "  max_analysis_threads_per_daemon: 4\n",
                         "  max_analysis_threads_per_daemon: 129456\n",
                         ConfigResult::InvalidValue, "129456"));

  cases.push_back(mutate("integer-overflow", "  min_word_len: 2\n",
                         "  min_word_len: 214748364812345\n",
                         ConfigResult::InvalidValue, "214748364812345"));
  cases.push_back(mutate("huge-integer-overflow", "  max_typo_diff: 2\n",
                         "  max_typo_diff: 99999999999999999999999912345\n",
                         ConfigResult::InvalidValue,
                         "99999999999999999999999912345"));
  cases.push_back(mutate("floating-overflow", "  threshold: 3.5\n",
                         "  threshold: 1.23456789e309\n",
                         ConfigResult::InvalidValue, "1.23456789e309"));
  return cases;
}

void test_invalid_contract_table(TestRunner &runner,
                                 const std::filesystem::path &directory) {
  const std::vector<InvalidCase> cases = invalid_cases();

  for (std::size_t index = 0; index < cases.size(); ++index) {
    const InvalidCase &test_case = cases[index];
    const std::filesystem::path path =
        write_config(directory, index + 1, test_case.contents);
    const ConfigLoadOutcome outcome = punto::load_config_checked(path);

    runner.expect(outcome.result == test_case.expected_result,
                  test_case.name + ": strict load returns " +
                      result_name(test_case.expected_result));
    expect_error_contract(runner, test_case.name, outcome,
                          test_case.expected_result, path,
                          test_case.payload_tokens);
  }
}

void test_file_errors(TestRunner &runner,
                      const std::filesystem::path &directory) {
  const std::filesystem::path missing_path =
      directory / "PRIVATE_PARENT_MISSING" /
      ("PRIVATE_MISSING_" + std::string(190, 'x') + ".yaml");
  const ConfigLoadOutcome missing = punto::load_config_checked(missing_path);
  runner.expect(missing.result == ConfigResult::FileNotFound,
                "missing-file: strict load returns FileNotFound");
  expect_error_contract(runner, "missing-file", missing,
                        ConfigResult::FileNotFound, missing_path);

  const std::filesystem::path unreadable_path =
      write_config(directory, 300, valid_config());
  std::error_code permission_error;
  std::filesystem::permissions(unreadable_path, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace,
                               permission_error);
  if (permission_error) {
    throw std::runtime_error{"cannot make config fixture unreadable"};
  }

  const auto exercise_unreadable = [&unreadable_path](TestRunner &target) {
    const ConfigLoadOutcome unreadable =
        punto::load_config_checked(unreadable_path);
    target.expect(unreadable.result == ConfigResult::IoError,
                  "unreadable-file: strict load returns IoError");
    expect_error_contract(target, "unreadable-file", unreadable,
                          unreadable.result, unreadable_path);
  };

  if (::geteuid() == 0) {
    const pid_t child = ::fork();
    if (child < 0) {
      throw std::system_error{errno, std::generic_category(), "fork failed"};
    }
    if (child == 0) {
      if (::setgid(65534) != 0 || ::setuid(65534) != 0) {
        ::_exit(125);
      }
      TestRunner child_runner;
      exercise_unreadable(child_runner);
      ::_exit(child_runner.failures() == 0 ? 0 : 1);
    }

    int status = 0;
    pid_t waited = -1;
    do {
      waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    runner.expect(
        waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "unreadable-file: privilege-dropped child proves EACCES contract");
  } else {
    exercise_unreadable(runner);
  }

  std::filesystem::permissions(
      unreadable_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, permission_error);
  if (permission_error) {
    throw std::runtime_error{"cannot restore temporary config permissions"};
  }
}

void test_safe_file_source_contract(TestRunner &runner,
                                    const std::filesystem::path &directory) {
  const std::filesystem::path regular_path =
      write_config(directory, 400, valid_config());

  const std::filesystem::path symlink_path =
      directory / "PRIVATE_CONFIG_SYMLINK.yaml";
  if (::symlink(regular_path.c_str(), symlink_path.c_str()) != 0) {
    throw std::system_error{errno, std::generic_category(), "symlink failed"};
  }
  const ConfigLoadOutcome symlink = punto::load_config_checked(symlink_path);
  runner.expect(symlink.result == ConfigResult::IoError,
                "symlink-source: strict load rejects path indirection");
  expect_error_contract(runner, "symlink-source", symlink,
                        ConfigResult::IoError, symlink_path);

  const std::filesystem::path directory_path =
      directory / "PRIVATE_CONFIG_DIRECTORY";
  std::error_code create_error;
  std::filesystem::create_directory(directory_path, create_error);
  if (create_error) {
    throw std::system_error{create_error, "create_directory failed"};
  }
  const ConfigLoadOutcome directory_source =
      punto::load_config_checked(directory_path);
  runner.expect(directory_source.result == ConfigResult::IoError,
                "directory-source: strict load rejects non-regular input");
  expect_error_contract(runner, "directory-source", directory_source,
                        ConfigResult::IoError, directory_path);

  const std::filesystem::path fifo_path = directory / "PRIVATE_CONFIG_FIFO";
  if (::mkfifo(fifo_path.c_str(), 0600) != 0) {
    throw std::system_error{errno, std::generic_category(), "mkfifo failed"};
  }
  const pid_t fifo_child = ::fork();
  if (fifo_child < 0) {
    throw std::system_error{errno, std::generic_category(), "fork failed"};
  }
  if (fifo_child == 0) {
    TestRunner child_runner;
    const ConfigLoadOutcome fifo = punto::load_config_checked(fifo_path);
    child_runner.expect(fifo.result == ConfigResult::IoError,
                        "fifo-source: strict load rejects FIFO input");
    expect_error_contract(child_runner, "fifo-source", fifo,
                          ConfigResult::IoError, fifo_path);
    ::_exit(child_runner.failures() == 0 ? 0 : 1);
  }

  int fifo_status = 0;
  pid_t fifo_waited = 0;
  for (int attempt = 0; attempt < 100 && fifo_waited == 0; ++attempt) {
    fifo_waited = ::waitpid(fifo_child, &fifo_status, WNOHANG);
    if (fifo_waited == 0) {
      ::usleep(5'000);
    }
  }
  if (fifo_waited == 0) {
    (void)::kill(fifo_child, SIGKILL);
    do {
      fifo_waited = ::waitpid(fifo_child, &fifo_status, 0);
    } while (fifo_waited < 0 && errno == EINTR);
    runner.expect(false, "fifo-source: strict load returns without blocking");
  } else {
    runner.expect(fifo_waited == fifo_child && WIFEXITED(fifo_status) &&
                      WEXITSTATUS(fifo_status) == 0,
                  "fifo-source: strict load rejects FIFO within 500 ms");
  }

  std::string oversized = valid_config();
  oversized.append("\n#");
  oversized.resize(punto::config_limits::kConfigFileMaxBytes + 1U, 'x');
  const std::filesystem::path oversized_path =
      write_config(directory, 401, oversized);
  const ConfigLoadOutcome oversized_result =
      punto::load_config_checked(oversized_path);
  runner.expect(oversized_result.result == ConfigResult::InvalidValue,
                "oversized-source: strict load enforces the byte limit");
  expect_error_contract(runner, "oversized-source", oversized_result,
                        ConfigResult::InvalidValue, oversized_path);

  std::string nul_path = regular_path.native();
  nul_path.push_back('\0');
  nul_path.append("PRIVATE_NUL_SUFFIX");
  const std::filesystem::path embedded_nul{nul_path};
  const ConfigLoadOutcome nul_result = punto::load_config_checked(embedded_nul);
  runner.expect(nul_result.result == ConfigResult::IoError,
                "nul-path: strict load rejects ambiguous native paths");
  expect_error_contract(runner, "nul-path", nul_result, ConfigResult::IoError,
                        embedded_nul);
}

void test_effective_config_precedence(TestRunner &runner,
                                      const std::filesystem::path &directory) {
  const std::filesystem::path home = directory / "PRIVATE_EFFECTIVE_HOME";
  const std::filesystem::path user_path =
      home / std::filesystem::path{punto::kUserConfigRelPath};
  const std::filesystem::path system_path =
      write_config(directory, 500, valid_config());
  ScopedHome scoped_home{home};

  const ConfigLoadOutcome system =
      punto::load_effective_config_checked(system_path);
  runner.expect(system.result == ConfigResult::Ok &&
                    system.used_path == system_path,
                "effective-config: missing user file falls back to system");

  std::error_code create_error;
  std::filesystem::create_directories(user_path.parent_path(), create_error);
  if (create_error) {
    throw std::system_error{create_error, "create_directories failed"};
  }
  {
    std::ofstream output{user_path, std::ios::binary | std::ios::trunc};
    output << replace_once(valid_config(), "  modifier: leftctrl\n",
                           "  modifier: rightalt\n");
    if (!output.good()) {
      throw std::runtime_error{"cannot write effective user config"};
    }
  }

  const ConfigLoadOutcome user =
      punto::load_effective_config_checked(system_path);
  runner.expect(user.result == ConfigResult::Ok &&
                    user.used_path == user_path &&
                    user.config.hotkey.modifier == KEY_RIGHTALT,
                "effective-config: valid user file has precedence");

  {
    std::ofstream output{user_path, std::ios::binary | std::ios::trunc};
    output << "PRIVATE_UNKNOWN_ROOT: true\n";
    if (!output.good()) {
      throw std::runtime_error{"cannot write invalid effective user config"};
    }
  }
  const ConfigLoadOutcome invalid_user =
      punto::load_effective_config_checked(system_path);
  runner.expect(
      invalid_user.result == ConfigResult::InvalidValue &&
          invalid_user.used_path == user_path,
      "effective-config: invalid user file never falls back to system");
  expect_error_contract(runner, "effective-invalid-user", invalid_user,
                        ConfigResult::InvalidValue, user_path,
                        {"PRIVATE_UNKNOWN_ROOT"});
}

void test_restricted_config_path_contract(
    TestRunner &runner, const std::filesystem::path &directory) {
  const std::filesystem::path allowed = directory / "PRIVATE_ALLOWED_ROOT";
  const std::filesystem::path outside = directory / "PRIVATE_OUTSIDE_ROOT";
  std::filesystem::create_directories(allowed / "slot");
  std::filesystem::create_directories(outside);

  const std::string safe_contents = valid_config();
  const std::string outside_contents = replace_once(
      valid_config(), "  modifier: leftctrl\n", "  modifier: rightalt\n");
  const auto write_exact = [](const std::filesystem::path &path,
                              std::string_view contents) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
      throw std::runtime_error{"cannot create restricted config fixture"};
    }
    output << contents;
    if (!output.good()) {
      throw std::runtime_error{"cannot write restricted config fixture"};
    }
  };

  const std::filesystem::path safe_path = allowed / "slot" / "config.yaml";
  const std::filesystem::path outside_path = outside / "config.yaml";
  write_exact(safe_path, safe_contents);
  write_exact(outside_path, outside_contents);

  const auto safe = punto::load_config_beneath_checked(allowed, safe_path);
  runner.expect(safe.path_allowed && safe.load.result == ConfigResult::Ok &&
                    safe.load.config.hotkey.modifier == KEY_LEFTCTRL,
                "restricted-load: regular descendant is read from pinned fd");

  const std::filesystem::path intermediate_alias =
      directory / "PRIVATE_ALLOWED_PARENT_LINK";
  if (::symlink(directory.c_str(), intermediate_alias.c_str()) != 0) {
    throw std::system_error{errno, std::generic_category(), "symlink failed"};
  }
  const std::filesystem::path aliased_root =
      intermediate_alias / "PRIVATE_ALLOWED_ROOT";
  const auto aliased_root_result = punto::load_config_beneath_checked(
      aliased_root, aliased_root / "slot" / "config.yaml");
  runner.expect(
      !aliased_root_result.path_allowed,
      "restricted-load: symlink component inside allowed root is rejected");

  const auto escaped =
      punto::load_config_beneath_checked(allowed, outside_path);
  runner.expect(!escaped.path_allowed,
                "restricted-load: lexical escape is rejected");

  const std::filesystem::path symlinked_parent = allowed / "linked";
  if (::symlink(outside.c_str(), symlinked_parent.c_str()) != 0) {
    throw std::system_error{errno, std::generic_category(), "symlink failed"};
  }
  const auto symlink_escape = punto::load_config_beneath_checked(
      allowed, symlinked_parent / "config.yaml");
  runner.expect(!symlink_escape.path_allowed,
                "restricted-load: symlinked parent cannot escape root");

  const std::filesystem::path backup = allowed / "slot-backup";
  std::atomic<bool> stop{false};
  std::thread swapper([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      if (::rename((allowed / "slot").c_str(), backup.c_str()) == 0) {
        const int symlink_result =
            ::symlink(outside.c_str(), (allowed / "slot").c_str());
        const int unlink_result = ::unlink((allowed / "slot").c_str());
        const int restore_result =
            ::rename(backup.c_str(), (allowed / "slot").c_str());
        (void)symlink_result;
        (void)unlink_result;
        (void)restore_result;
      }
    }
  });

  for (int attempt = 0; attempt < 2000; ++attempt) {
    const auto raced = punto::load_config_beneath_checked(allowed, safe_path);
    if (raced.path_allowed && raced.load.result == ConfigResult::Ok) {
      runner.expect(raced.load.config.hotkey.modifier == KEY_LEFTCTRL,
                    "restricted-load: parent swap never reads outside root");
    }
  }
  stop.store(true, std::memory_order_relaxed);
  swapper.join();

  const int unlink_result = ::unlink((allowed / "slot").c_str());
  (void)unlink_result;
  if (!std::filesystem::exists(allowed / "slot") &&
      std::filesystem::exists(backup)) {
    std::filesystem::rename(backup, allowed / "slot");
  }
}

} // namespace

int main() {
  try {
    TestRunner runner;
    TempDirectory directory;

    test_compiled_defaults(runner);
    test_shipped_config(runner);
    test_inline_comments(runner, directory.path());
    test_modifier_keys_as_second_chord_member(runner, directory.path());
    test_serializer_contract(runner, directory.path());
    test_empty_partial_and_legacy_configs(runner, directory.path());
    test_inclusive_valid_boundaries(runner, directory.path());
    test_invalid_contract_table(runner, directory.path());
    test_file_errors(runner, directory.path());
    test_safe_file_source_contract(runner, directory.path());
    test_effective_config_precedence(runner, directory.path());
    test_restricted_config_path_contract(runner, directory.path());

    if (runner.failures() != 0) {
      std::cerr << runner.failures()
                << " config contract assertion(s) failed\n";
      return 1;
    }
    std::cout << "config contract tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FATAL: " << error.what() << '\n';
    return 2;
  }
}
