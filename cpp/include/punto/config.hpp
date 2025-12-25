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
#include <optional>
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

  // Турбо-задержки для автокоррекции.
  // ВАЖНО: слишком малые значения приводят к гонке символов в медленных
  // приложениях (Electron, Java). Минимальные безопасные значения: 15ms/35ms.
  std::chrono::microseconds turbo_key_press{
      15000}; // 15ms для автостирания/печати
  std::chrono::microseconds turbo_retype{
      35000}; // 35ms пауза между символами в turbo

  // Детальные настройки для tap_key (эмуляция нажатия клавиши)
  // Эти параметры влияют на надёжность ввода в разных приложениях
  std::chrono::microseconds key_hold{
      20000}; // 20ms удержание клавиши между press и release
  std::chrono::microseconds modifier_hold{
      15000}; // 15ms удержание модификатора (Shift) перед клавишей
  std::chrono::microseconds modifier_release{
      8000}; // 8ms задержка после отпускания модификатора
  std::chrono::microseconds backspace_hold{18000}; // 18ms удержание Backspace
};

/// Настройки автоматического переключения раскладки
struct AutoSwitchConfig {
  /// Включено ли автопереключение
  bool enabled = true;

  /// Порог срабатывания: отношение Score(Other) / Score(Current)
  /// Например, 2.0 означает, что другая раскладка должна быть в 2 раза
  /// вероятнее
  double threshold = 2.5;

  /// Минимальная длина слова для анализа (чтобы не переключать "a", "i", "y")
  std::size_t min_word_len = 3;

  /// Минимальный скор для принятия решения (защита от случайных данных)
  double min_score = 10.0;

  /// Максимальная глубина отката (сколько последних слов можно перепечатать
  /// при позднем срабатывании анализа).
  ///
  /// Важно: чем больше значение, тем больше будет макрос коррекции (backspace +
  /// replay) при очень быстром наборе.
  std::size_t max_rollback_words = 5;

  // =========================================================================
  // Исправление опечаток (v2.7+)
  // =========================================================================

  /// Включено ли исправление опечаток (перестановки, замены, пропуски, дубли)
  bool typo_correction_enabled = true;

  /// Максимальное расстояние редактирования (для пропусков/дублей букв).
  /// 1 = исправляем только одну пропущенную/лишнюю букву в слове.
  /// 2 = до двух ошибок на слово (более агрессивно).
  std::size_t max_typo_diff = 1;

  /// Включено ли исправление залипшего Shift (ПРивет -> Привет, кОЛБАСА ->
  /// Колбаса)
  bool sticky_shift_correction_enabled = true;
};

/// Настройки звуковой индикации переключения раскладки
struct SoundConfig {
  bool enabled = true;
};

/// Полная конфигурация приложения
struct Config {
  HotkeyConfig hotkey;
  DelayConfig delays;
  AutoSwitchConfig auto_switch;
  SoundConfig sound;
  std::filesystem::path config_path{"/etc/punto/config.yaml"};
};

// ===========================================================================
// Загрузчик конфигурации
// ===========================================================================

/// Результат загрузки конфигурации из файла.
///
/// В отличие от `load_config()`, это API НЕ делает скрытых фолбэков и
/// позволяет вызывающему коду принять решение (fail-fast / fallback /
/// UI-ошибка).
struct ConfigLoadOutcome {
  Config config;
  ConfigResult result = ConfigResult::Ok;
  std::filesystem::path used_path;
  std::string error;
};

/**
 * @brief Загружает конфигурацию из конкретного файла
 *
 * @param path Абсолютный или относительный путь к конфигу
 * @return ConfigLoadOutcome с кодом результата и сообщением ошибки
 */
[[nodiscard]] ConfigLoadOutcome load_config_checked(std::filesystem::path path);

/**
 * @brief Загружает конфигурацию из YAML файла (best-effort)
 *
 * @param path Путь к конфигурационному файлу
 * @return Config с загруженными или дефолтными значениями
 *
 * Best-effort поведение: при ошибках чтения/валидации возвращает дефолты.
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
