/**
 * @file config.cpp
 * @brief Реализация загрузчика конфигурации
 */

#include "punto/config.hpp"
#include "punto/scancode_map.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace punto {

namespace {

/// Удаляет пробелы с начала и конца строки
std::string_view trim(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

/// Парсит целое число из строки
std::optional<int> parse_int(std::string_view sv) {
  sv = trim(sv);
  int value = 0;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
  if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
    return value;
  }
  return std::nullopt;
}

/// Парсит число с плавающей точкой из строки
std::optional<double> parse_double(std::string_view sv) {
  sv = trim(sv);
  // std::from_chars для double не везде поддерживается, используем strtod
  std::string str{sv};
  char *end = nullptr;
  double value = std::strtod(str.c_str(), &end);
  if (end == str.c_str() + str.size()) {
    return value;
  }
  return std::nullopt;
}

/// Парсит булево значение из строки
std::optional<bool> parse_bool(std::string_view sv) {
  sv = trim(sv);
  if (sv == "true" || sv == "yes" || sv == "1" || sv == "on") {
    return true;
  }
  if (sv == "false" || sv == "no" || sv == "0" || sv == "off") {
    return false;
  }
  return std::nullopt;
}

} // namespace

std::optional<std::chrono::microseconds>
parse_delay_ms(std::string_view value) {
  auto ms = parse_int(value);
  if (ms && *ms > 0) {
    return std::chrono::microseconds{*ms * 1000};
  }
  return std::nullopt;
}

bool validate_config(const Config &config) {
  // Проверка задержек
  if (config.delays.key_press.count() <= 0 ||
      config.delays.layout_switch.count() <= 0 ||
      config.delays.retype.count() <= 0 ||
      config.delays.turbo_key_press.count() <= 0 ||
      config.delays.turbo_retype.count() <= 0) {
    return false;
  }

  // Проверка кодов клавиш
  if (config.hotkey.modifier == 0 || config.hotkey.key == 0) {
    return false;
  }

  return true;
}

/// Получает путь к user config (~/.config/punto/config.yaml)
std::string get_user_config_path() {
  const char* home = std::getenv("HOME");
  if (home) {
    return std::string(home) + "/" + std::string(kUserConfigRelPath);
  }
  return "";
}

Config load_config(std::string_view path) {
  Config config;

  // Пробуем сначала user config, потом system config
  std::string config_path;
  
  if (path == kConfigPath) {
    // Если запрашивается дефолтный путь, пробуем user config первым
    std::string user_path = get_user_config_path();
    if (!user_path.empty()) {
      std::ifstream user_file{user_path};
      if (user_file.is_open()) {
        config_path = user_path;
        std::cerr << "[punto] Using user config: " << user_path << "\n";
      }
    }
  }
  
  if (config_path.empty()) {
    config_path = std::string{path};
  }

  std::ifstream file{config_path};
  if (!file.is_open()) {
    // Файл не найден — используем дефолты
    return config;
  }
  
  config.config_path = config_path;

  std::string line;
  std::string current_section;

  while (std::getline(file, line)) {
    std::string_view sv = trim(line);

    // Пропуск пустых строк и комментариев
    if (sv.empty() || sv.front() == '#') {
      continue;
    }

    // Определение секции
    if (sv == "hotkey:" || sv.starts_with("hotkey:")) {
      current_section = "hotkey";
      continue;
    }
    if (sv == "delays:" || sv.starts_with("delays:")) {
      current_section = "delays";
      continue;
    }
    if (sv == "auto_switch:" || sv.starts_with("auto_switch:")) {
      current_section = "auto_switch";
      continue;
    }
    if (sv == "sound:" || sv.starts_with("sound:")) {
      current_section = "sound";
      continue;
    }

    // Парсинг key: value
    auto colon_pos = sv.find(':');
    if (colon_pos == std::string_view::npos) {
      continue;
    }

    std::string_view key = trim(sv.substr(0, colon_pos));
    std::string_view value = trim(sv.substr(colon_pos + 1));

    if (current_section == "hotkey") {
      if (key == "modifier") {
        if (auto code = key_name_to_code(value)) {
          config.hotkey.modifier = *code;
        }
      } else if (key == "key") {
        if (auto code = key_name_to_code(value)) {
          config.hotkey.key = *code;
        }
      }
    } else if (current_section == "delays") {
      if (auto delay = parse_delay_ms(value)) {
        if (key == "key_press") {
          config.delays.key_press = *delay;
        } else if (key == "layout_switch") {
          config.delays.layout_switch = *delay;
        } else if (key == "retype") {
          config.delays.retype = *delay;
        } else if (key == "turbo_key_press") {
          config.delays.turbo_key_press = *delay;
        } else if (key == "turbo_retype") {
          config.delays.turbo_retype = *delay;
        }
      }
    } else if (current_section == "auto_switch") {
      if (key == "enabled") {
        if (auto val = parse_bool(value)) {
          config.auto_switch.enabled = *val;
        }
      } else if (key == "threshold") {
        if (auto val = parse_double(value)) {
          config.auto_switch.threshold = *val;
        }
      } else if (key == "min_word_len") {
        if (auto val = parse_int(value)) {
          config.auto_switch.min_word_len = static_cast<std::size_t>(*val);
        }
      } else if (key == "min_score") {
        if (auto val = parse_double(value)) {
          config.auto_switch.min_score = *val;
        }
      }
    } else if (current_section == "sound") {
      if (key == "enabled") {
        if (auto val = parse_bool(value)) {
          config.sound.enabled = *val;
        }
      }
    }
  }

  if (!validate_config(config)) {
    std::cerr << "[punto] Предупреждение: некорректная конфигурация, "
                 "используются значения по умолчанию\n";
    return Config{};
  }

  return config;
}

} // namespace punto
