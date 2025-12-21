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
      config.delays.retype.count() <= 0) {
    return false;
  }

  // Проверка кодов клавиш
  if (config.hotkey.modifier == 0 || config.hotkey.key == 0) {
    return false;
  }

  return true;
}

Config load_config(std::string_view path) {
  Config config;

  std::ifstream file{std::string{path}};
  if (!file.is_open()) {
    // Файл не найден — используем дефолты
    return config;
  }

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
