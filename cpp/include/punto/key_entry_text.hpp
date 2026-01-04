/**
 * @file key_entry_text.hpp
 * @brief Утилиты конвертации KeyEntry -> текст (QWERTY / видимый UTF-8)
 *
 * Нужны для oneshot-замен через Clipboard+Paste: вместо посимвольного retype
 * мы вычисляем итоговую строку и вставляем её одной операцией.
 */

#pragma once

#include <linux/input.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "punto/scancode_map.hpp"
#include "punto/text_processor.hpp"
#include "punto/types.hpp"

namespace punto {

namespace detail {

[[nodiscard]] constexpr char apply_shift_to_qwerty_char(char c) noexcept {
  // Буквы
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - 32);
  }

  // Цифры и знаки (US layout)
  switch (c) {
  case '1':
    return '!';
  case '2':
    return '@';
  case '3':
    return '#';
  case '4':
    return '$';
  case '5':
    return '%';
  case '6':
    return '^';
  case '7':
    return '&';
  case '8':
    return '*';
  case '9':
    return '(';
  case '0':
    return ')';

  case '[':
    return '{';
  case ']':
    return '}';
  case ';':
    return ':';
  case 0x27: // '\''
    return '"';
  case '`':
    return '~';
  case ',':
    return '<';
  case '.':
    return '>';
  case '/':
    return '?';
  case 0x5C: // backslash
    return '|';
  case '-':
    return '_';
  case '=':
    return '+';

  default:
    break;
  }

  return c;
}

} // namespace detail

/// Конвертирует последовательность KeyEntry (скан-коды + shift) в QWERTY-строку.
///
/// Важно: это НЕ «видимый текст» в русской раскладке. Для RU раскладки используйте
/// qwerty_to_visible_text(..., layout=1), которая делает EN->RU маппинг.
[[nodiscard]] inline std::string
key_entries_to_qwerty(std::span<const KeyEntry> entries) {
  std::string out;
  out.reserve(entries.size());

  for (const auto &e : entries) {
    if (e.code == KEY_SPACE) {
      out.push_back(' ');
      continue;
    }
    if (e.code == KEY_TAB) {
      out.push_back('\t');
      continue;
    }

    if (e.code >= kScancodeToChar.size()) {
      continue;
    }

    char c = kScancodeToChar[e.code];
    if (c == '\0') {
      continue;
    }

    if (e.shifted) {
      c = detail::apply_shift_to_qwerty_char(c);
    }

    out.push_back(c);
  }

  return out;
}

/// Применяет «видимую» раскладку к QWERTY-строке.
/// layout: 0=EN, 1=RU.
[[nodiscard]] inline std::string
qwerty_to_visible_text(std::string_view qwerty, int layout) {
  if (layout == 0) {
    return std::string{qwerty};
  }
  if (layout == 1) {
    return en_to_ru(qwerty);
  }

  // Fail-fast: некорректное значение раскладки.
  return {};
}

/// Конвертирует KeyEntry -> «видимый» текст в заданной раскладке.
/// layout: 0=EN, 1=RU.
[[nodiscard]] inline std::string
key_entries_to_visible_text(std::span<const KeyEntry> entries, int layout) {
  std::string qwerty = key_entries_to_qwerty(entries);
  return qwerty_to_visible_text(qwerty, layout);
}

/// Как key_entries_to_visible_text, но fail-fast если какие-то скан-коды не
/// удалось смэппить в QWERTY (чтобы не терять символы при oneshot-замене).
[[nodiscard]] inline std::optional<std::string>
key_entries_to_visible_text_checked(std::span<const KeyEntry> entries,
                                   int layout) {
  std::string qwerty = key_entries_to_qwerty(entries);
  if (qwerty.size() != entries.size()) {
    return std::nullopt;
  }

  std::string visible = qwerty_to_visible_text(qwerty, layout);
  if (visible.empty() && !qwerty.empty()) {
    return std::nullopt;
  }

  return visible;
}

} // namespace punto
