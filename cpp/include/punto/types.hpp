/**
 * @file types.hpp
 * @brief Базовые типы и структуры данных для Punto Switcher
 *
 * Этот файл содержит все фундаментальные типы, используемые во всём приложении.
 * Zero-overhead абстракции с RAII гарантиями.
 */

#pragma once

#include <linux/input.h>
#include <sys/time.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace punto {

// ===========================================================================
// Константы
// ===========================================================================

/// Максимальная длина слова (в скан-кодах)
inline constexpr std::size_t kMaxWordLen = 256;

/// Путь к системному конфигурационному файлу
inline constexpr std::string_view kConfigPath = "/etc/punto/config.yaml";

/// Путь к пользовательскому конфигу (относительно $HOME)
inline constexpr std::string_view kUserConfigRelPath = ".config/punto/config.yaml";

// ===========================================================================
// Типы для работы с событиями ввода
// ===========================================================================

/// Значение события клавиши
enum class KeyState : std::int32_t { Release = 0, Press = 1, Repeat = 2 };

/// Структура для хранения биграммы и её веса (выровнена для SSE)
struct alignas(4) BigramEntry {
  char first;
  char second;
  std::uint8_t weight;
};

/// Скан-код клавиши (обёртка над linux/input.h константами)
using ScanCode = std::uint16_t;

/// Состояние модификаторов
struct ModifierState {
  bool left_shift : 1 = false;
  bool right_shift : 1 = false;
  bool left_ctrl : 1 = false;
  bool right_ctrl : 1 = false;
  bool left_alt : 1 = false;
  bool right_alt : 1 = false;
  bool left_meta : 1 = false;
  bool right_meta : 1 = false;

  [[nodiscard]] constexpr bool any_shift() const noexcept {
    return left_shift || right_shift;
  }

  [[nodiscard]] constexpr bool any_ctrl() const noexcept {
    return left_ctrl || right_ctrl;
  }

  [[nodiscard]] constexpr bool any_alt() const noexcept {
    return left_alt || right_alt;
  }

  [[nodiscard]] constexpr bool any_meta() const noexcept {
    return left_meta || right_meta;
  }

  /// Сброс всех модификаторов
  constexpr void reset() noexcept {
    left_shift = right_shift = false;
    left_ctrl = right_ctrl = false;
    left_alt = right_alt = false;
    left_meta = right_meta = false;
  }
};

// ===========================================================================
// Структура для хранения символа с метаданными
// ===========================================================================

/// Элемент буфера слова: скан-код + состояние регистра
struct KeyEntry {
  ScanCode code = 0;
  bool shifted = false;

  constexpr KeyEntry() noexcept = default;
  constexpr KeyEntry(ScanCode c, bool s) noexcept : code(c), shifted(s) {}

  constexpr bool operator==(const KeyEntry &) const noexcept = default;
};

// ===========================================================================
// Типы результатов операций
// ===========================================================================

/// Результат парсинга конфигурации
enum class ConfigResult { Ok, FileNotFound, ParseError, InvalidValue };

/// Результат операции с буфером обмена
enum class ClipboardResult {
  Ok,
  NoConnection,
  NoSelection,
  ConversionFailed,
  Timeout
};

/// Тип действия по горячей клавише
enum class HotkeyAction {
  NoAction,              // Нет действия
  InvertLayoutWord,      // Pause
  InvertLayoutSelection, // Shift+Pause
  InvertCaseWord,        // Ctrl+Pause
  InvertCaseSelection,   // Alt+Pause
  TranslitSelection      // LCtrl+LAlt+Pause
};

// ===========================================================================
// Буферы
// ===========================================================================

/// Фиксированный буфер для слова
using WordBuffer = std::array<KeyEntry, kMaxWordLen>;

/// Буфер для trailing whitespace (пробелы/табы после слова)
using TrailingBuffer = std::array<ScanCode, kMaxWordLen>;

// ===========================================================================
// Inline утилиты
// ===========================================================================

/// Проверка, является ли скан-код модификатором
[[nodiscard]] constexpr bool is_modifier(ScanCode code) noexcept {
  return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT ||
         code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL || code == KEY_LEFTALT ||
         code == KEY_RIGHTALT || code == KEY_LEFTMETA || code == KEY_RIGHTMETA;
}

/// Проверка, является ли ключ навигационной клавишей
[[nodiscard]] constexpr bool is_navigation_key(ScanCode code) noexcept {
  return code == KEY_LEFT || code == KEY_RIGHT || code == KEY_UP ||
         code == KEY_DOWN || code == KEY_HOME || code == KEY_END ||
         code == KEY_PAGEUP || code == KEY_PAGEDOWN || code == KEY_INSERT ||
         code == KEY_DELETE;
}

/// Проверка, является ли ключ функциональной клавишей
[[nodiscard]] constexpr bool is_function_key(ScanCode code) noexcept {
  return code >= KEY_F1 && code <= KEY_F12;
}

} // namespace punto
