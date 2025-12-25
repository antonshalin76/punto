/**
 * @file smart_bypass.cpp
 * @brief Реализация Smart Bypass — детектирования слов, не требующих анализа
 */

#include "punto/smart_bypass.hpp"
#include "punto/scancode_map.hpp"

#include <linux/input.h>

namespace punto {

namespace {

/// Конвертирует KeyEntry в ASCII символ (lowercase)
[[nodiscard]] char to_ascii_lower(const KeyEntry &entry) noexcept {
  if (entry.code >= kScancodeToChar.size()) {
    return '\0';
  }
  char c = kScancodeToChar[entry.code];
  // Приводим к нижнему регистру если это буква
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }
  return c;
}

/// Проверяет, является ли KeyEntry заглавной буквой
[[nodiscard]] bool is_uppercase(const KeyEntry &entry) noexcept {
  if (!is_typeable_letter(entry.code)) {
    return false;
  }
  return entry.shifted;
}

/// Проверяет, является ли KeyEntry строчной буквой
[[nodiscard]] bool is_lowercase(const KeyEntry &entry) noexcept {
  if (!is_typeable_letter(entry.code)) {
    return false;
  }
  return !entry.shifted;
}

/// Проверяет, является ли символ слешем (/ или \)
[[nodiscard]] bool is_slash(const KeyEntry &entry) noexcept {
  char c = to_ascii_lower(entry);
  return c == '/' || c == '\\';
}

/// Проверяет, является ли символ точкой
[[nodiscard]] bool is_dot(const KeyEntry &entry) noexcept {
  return entry.code == KEY_DOT && !entry.shifted;
}

/// Проверяет, является ли символ подчёркиванием (Shift + -)
[[nodiscard]] bool is_underscore(const KeyEntry &entry) noexcept {
  return entry.code == KEY_MINUS && entry.shifted;
}

/// Проверяет, является ли символ @ (Shift + 2)
[[nodiscard]] bool is_at_symbol(const KeyEntry &entry) noexcept {
  return entry.code == KEY_2 && entry.shifted;
}

/// Проверяет, является ли символ двоеточием (Shift + ;)
[[nodiscard]] bool is_colon(const KeyEntry &entry) noexcept {
  return entry.code == KEY_SEMICOLON && entry.shifted;
}

} // namespace

BypassReason should_bypass(std::span<const KeyEntry> word,
                           std::size_t min_word_len) {
  if (word.size() < min_word_len) {
    return BypassReason::TooShort;
  }

  // Проверяем URL/path паттерны (высокий приоритет)
  if (contains_url_or_path_chars(word)) {
    return BypassReason::UrlDetected;
  }

  // snake_case (my_variable)
  if (is_snake_case(word)) {
    return BypassReason::SnakeCaseDetected;
  }

  // Аббревиатуры из заглавных (API, URL) — проверяем до camelCase
  if (is_all_caps_acronym(word)) {
    return BypassReason::AllCapsAcronym;
  }

  // camelCase (myVariable, getElementById)
  if (is_camel_case(word)) {
    return BypassReason::CamelCaseDetected;
  }

  // PascalCase тоже пропускаем (MyClass, HttpRequest)
  if (is_pascal_case(word)) {
    return BypassReason::CamelCaseDetected;
  }

  return BypassReason::None;
}

bool is_camel_case(std::span<const KeyEntry> word) {
  if (word.size() < 3) {
    return false;
  }

  // camelCase: первая буква строчная
  if (!is_lowercase(word[0])) {
    return false;
  }

  // Ищем переход lower → upper внутри слова
  for (std::size_t i = 1; i + 1 < word.size(); ++i) {
    if (is_lowercase(word[i]) && is_uppercase(word[i + 1])) {
      return true;
    }
  }

  return false;
}

bool is_pascal_case(std::span<const KeyEntry> word) {
  if (word.size() < 3) {
    return false;
  }

  // PascalCase: первая буква заглавная
  if (!is_uppercase(word[0])) {
    return false;
  }

  // Ищем переход lower → upper внутри слова (не в начале)
  for (std::size_t i = 1; i + 1 < word.size(); ++i) {
    if (is_lowercase(word[i]) && is_uppercase(word[i + 1])) {
      return true;
    }
  }

  return false;
}

bool contains_url_or_path_chars(std::span<const KeyEntry> word) {
  if (word.empty()) {
    return false;
  }

  // Начинается с . (скрытые файлы/директории)
  if (is_dot(word[0])) {
    return true;
  }

  std::size_t slash_count = 0;
  bool has_at = false;
  bool has_colon = false;

  for (const auto &entry : word) {
    if (is_slash(entry)) {
      ++slash_count;
    }
    if (is_at_symbol(entry)) {
      has_at = true;
    }
    if (is_colon(entry)) {
      has_colon = true;
    }
  }

  // Путь: содержит слеш
  if (slash_count > 0) {
    return true;
  }

  // Email: содержит @
  if (has_at) {
    return true;
  }

  // URL: содержит : (http:, https:, file:)
  // Но только если слово достаточно длинное (минимум "a:")
  if (has_colon && word.size() >= 2) {
    return true;
  }

  // Проверяем паттерны http, https, www, ftp
  // Конвертируем первые 5 символов в строку для проверки
  if (word.size() >= 3) {
    char prefix[6] = {0};
    std::size_t prefix_len = std::min(word.size(), std::size_t{5});
    for (std::size_t i = 0; i < prefix_len; ++i) {
      prefix[i] = to_ascii_lower(word[i]);
    }

    // www
    if (prefix[0] == 'w' && prefix[1] == 'w' && prefix[2] == 'w') {
      return true;
    }

    // http (также покрывает https)
    if (word.size() >= 4 && prefix[0] == 'h' && prefix[1] == 't' &&
        prefix[2] == 't' && prefix[3] == 'p') {
      return true;
    }

    // ftp
    if (prefix[0] == 'f' && prefix[1] == 't' && prefix[2] == 'p') {
      return true;
    }
  }

  return false;
}

bool is_snake_case(std::span<const KeyEntry> word) {
  if (word.size() < 3) {
    return false;
  }

  // Ищем подчёркивание
  for (const auto &entry : word) {
    if (is_underscore(entry)) {
      return true;
    }
  }

  return false;
}

bool is_all_caps_acronym(std::span<const KeyEntry> word) {
  // Аббревиатуры: 2-5 символов, все заглавные буквы
  if (word.size() < 2 || word.size() > 5) {
    return false;
  }

  std::size_t letter_count = 0;

  for (const auto &entry : word) {
    if (!is_typeable_letter(entry.code)) {
      return false; // Не буква — не аббревиатура
    }

    if (!is_uppercase(entry)) {
      return false; // Есть строчная буква — не чистая аббревиатура
    }

    ++letter_count;
  }

  // Должно быть минимум 2 буквы
  return letter_count >= 2;
}

} // namespace punto
