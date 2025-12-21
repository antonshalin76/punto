/**
 * @file config.hpp
 * @brief Конфигурация Punto Switcher
 *
 * Типобезопасная конфигурация с YAML парсингом.
 * Все значения имеют разумные дефолты.
 */

#pragma once

#include <linux/input.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "punto/types.hpp"

namespace punto {

// ===========================================================================
// Структура конфигурации
// ===========================================================================

/// Настройки горячей клавиши переключения раскладки
struct HotkeyConfig {
  std::uint16_t modifier = KEY_LEFTCTRL;
  std::uint16_t key = KEY_GRAVE;
};

/// Настройки задержек (в микросекундах для совместимости с usleep)
struct DelayConfig {
  std::chrono::microseconds key_press{20000};      // 20ms
  std::chrono::microseconds layout_switch{100000}; // 100ms
  std::chrono::microseconds retype{3000};          // 3ms между символами
};

/// Полная конфигурация приложения
struct Config {
  HotkeyConfig hotkey;
  DelayConfig delays;
  std::filesystem::path config_path{"/etc/punto/config.yaml"};

  // Дополнительные параметры (для будущего расширения)
  bool debug_mode = false;
  bool log_to_syslog = true;
};

// ===========================================================================
// Загрузчик конфигурации
// ===========================================================================

/**
 * @brief Загружает конфигурацию из YAML файла
 *
 * @param path Путь к конфигурационному файлу
 * @return Config с загруженными или дефолтными значениями
 *
 * В случае ошибки парсинга возвращает конфигурацию с дефолтными значениями
 * и логирует предупреждение в stderr.
 */
[[nodiscard]] Config load_config(std::string_view path = kConfigPath);

/**
 * @brief Парсит значение задержки из строки
 *
 * @param value Строка с числом (миллисекунды)
 * @return Значение в микросекундах или std::nullopt
 */
[[nodiscard]] std::optional<std::chrono::microseconds>
parse_delay_ms(std::string_view value);

/**
 * @brief Валидирует конфигурацию
 *
 * @param config Конфигурация для проверки
 * @return true если все значения в допустимых пределах
 */
[[nodiscard]] bool validate_config(const Config &config);

} // namespace punto
