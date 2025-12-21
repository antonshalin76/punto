/**
 * @file text_processor.cpp
 * @brief Реализация обработки текста
 *
 * UTF-8 aware функции для инверсии раскладки, регистра и транслитерации.
 */

#include "punto/text_processor.hpp"
#include "punto/scancode_map.hpp"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace punto {

namespace {

/// Lazy-initialized lookup table для EN->RU (lower)
const std::unordered_map<char, std::string_view> &get_en_to_ru_lower() {
  static const auto map = [] {
    std::unordered_map<char, std::string_view> m;
    for (const auto &entry : kEnToRuLower) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Lazy-initialized lookup table для EN->RU (upper)
const std::unordered_map<char, std::string_view> &get_en_to_ru_upper() {
  static const auto map = [] {
    std::unordered_map<char, std::string_view> m;
    for (const auto &entry : kEnToRuUpper) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Lazy-initialized lookup table для RU->EN (lower)
const std::unordered_map<std::string_view, char> &get_ru_to_en_lower() {
  static const auto map = [] {
    std::unordered_map<std::string_view, char> m;
    for (const auto &entry : kRuToEnLower) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Lazy-initialized lookup table для RU->EN (upper)
const std::unordered_map<std::string_view, char> &get_ru_to_en_upper() {
  static const auto map = [] {
    std::unordered_map<std::string_view, char> m;
    for (const auto &entry : kRuToEnUpper) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Lazy-initialized lookup table для CYR->LAT multi
const std::unordered_map<std::string_view, std::string_view> &
get_cyr_to_lat_multi() {
  static const auto map = [] {
    std::unordered_map<std::string_view, std::string_view> m;
    for (const auto &entry : kCyrToLatMulti) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Lazy-initialized lookup table для CYR->LAT single (lower + upper)
const std::unordered_map<std::string_view, char> &get_cyr_to_lat_single() {
  static const auto map = [] {
    std::unordered_map<std::string_view, char> m;
    for (const auto &entry : kCyrToLatLower) {
      m[entry.from] = entry.to;
    }
    for (const auto &entry : kCyrToLatUpper) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Lazy-initialized lookup table для LAT->CYR multi
const std::unordered_map<std::string_view, std::string_view> &
get_lat_to_cyr_multi() {
  static const auto map = [] {
    std::unordered_map<std::string_view, std::string_view> m;
    for (const auto &entry : kLatToCyrMulti) {
      m[entry.from] = entry.to;
    }
    return m;
  }();
  return map;
}

/// Кириллический нижний регистр -> верхний
std::string_view cyr_to_upper(std::string_view lower) {
  static const std::unordered_map<std::string_view, std::string_view> map = {
      {"а", "А"}, {"б", "Б"}, {"в", "В"}, {"г", "Г"}, {"д", "Д"}, {"е", "Е"},
      {"ё", "Ё"}, {"ж", "Ж"}, {"з", "З"}, {"и", "И"}, {"й", "Й"}, {"к", "К"},
      {"л", "Л"}, {"м", "М"}, {"н", "Н"}, {"о", "О"}, {"п", "П"}, {"р", "Р"},
      {"с", "С"}, {"т", "Т"}, {"у", "У"}, {"ф", "Ф"}, {"х", "Х"}, {"ц", "Ц"},
      {"ч", "Ч"}, {"ш", "Ш"}, {"щ", "Щ"}, {"ъ", "Ъ"}, {"ы", "Ы"}, {"ь", "Ь"},
      {"э", "Э"}, {"ю", "Ю"}, {"я", "Я"},
  };
  auto it = map.find(lower);
  return it != map.end() ? it->second : lower;
}

/// Кириллический верхний регистр -> нижний
std::string_view cyr_to_lower(std::string_view upper) {
  static const std::unordered_map<std::string_view, std::string_view> map = {
      {"А", "а"}, {"Б", "б"}, {"В", "в"}, {"Г", "г"}, {"Д", "д"}, {"Е", "е"},
      {"Ё", "ё"}, {"Ж", "ж"}, {"З", "з"}, {"И", "и"}, {"Й", "й"}, {"К", "к"},
      {"Л", "л"}, {"М", "м"}, {"Н", "н"}, {"О", "о"}, {"П", "п"}, {"Р", "р"},
      {"С", "с"}, {"Т", "т"}, {"У", "у"}, {"Ф", "ф"}, {"Х", "х"}, {"Ц", "ц"},
      {"Ч", "ч"}, {"Ш", "ш"}, {"Щ", "щ"}, {"Ъ", "ъ"}, {"Ы", "ы"}, {"Ь", "ь"},
      {"Э", "э"}, {"Ю", "ю"}, {"Я", "я"},
  };
  auto it = map.find(upper);
  return it != map.end() ? it->second : upper;
}

/// Проверка, является ли кириллический символ заглавным
bool is_cyr_upper(std::string_view ch) {
  static const std::unordered_set<std::string_view> upper_chars = {
      "А", "Б", "В", "Г", "Д", "Е", "Ё", "Ж", "З", "И", "Й",
      "К", "Л", "М", "Н", "О", "П", "Р", "С", "Т", "У", "Ф",
      "Х", "Ц", "Ч", "Ш", "Щ", "Ъ", "Ы", "Ь", "Э", "Ю", "Я",
  };
  return upper_chars.count(ch) > 0;
}

} // namespace

bool is_cyrillic_char(std::string_view utf8_char) {
  // Кириллица: U+0400-U+04FF (2 байта в UTF-8: 0xD0 0x80 - 0xD1 0xBF)
  if (utf8_char.size() != 2)
    return false;

  auto b0 = static_cast<unsigned char>(utf8_char[0]);
  auto b1 = static_cast<unsigned char>(utf8_char[1]);

  // Basic Cyrillic: U+0400-U+04FF
  // D0 80-BF (U+0400-U+043F) and D1 80-BF (U+0440-U+047F)
  if (b0 == 0xD0 || b0 == 0xD1) {
    return (b1 >= 0x80 && b1 <= 0xBF);
  }

  return false;
}

std::pair<std::size_t, std::size_t> count_letters(std::string_view text) {
  std::size_t cyrillic = 0;
  std::size_t total = 0;

  std::size_t i = 0;
  while (i < text.size()) {
    auto len = utf8_char_len(static_cast<unsigned char>(text[i]));
    if (len == 0 || i + len > text.size()) {
      ++i;
      continue;
    }

    std::string_view ch = text.substr(i, len);

    if (len == 1 && is_latin_char(text[i])) {
      ++total;
    } else if (len == 2 && is_cyrillic_char(ch)) {
      ++cyrillic;
      ++total;
    }

    i += len;
  }

  return {cyrillic, total};
}

bool is_predominantly_cyrillic(std::string_view text) {
  auto [cyr, total] = count_letters(text);
  if (total == 0)
    return false;
  return static_cast<double>(cyr) / static_cast<double>(total) > 0.5;
}

std::string en_to_ru(std::string_view text) {
  std::string result;
  result.reserve(text.size() * 2); // Русские буквы занимают 2 байта

  const auto &lower_map = get_en_to_ru_lower();
  const auto &upper_map = get_en_to_ru_upper();

  for (char c : text) {
    if (auto it = lower_map.find(c); it != lower_map.end()) {
      result += it->second;
    } else if (auto it2 = upper_map.find(c); it2 != upper_map.end()) {
      result += it2->second;
    } else {
      result += c;
    }
  }

  return result;
}

std::string ru_to_en(std::string_view text) {
  std::string result;
  result.reserve(text.size());

  const auto &lower_map = get_ru_to_en_lower();
  const auto &upper_map = get_ru_to_en_upper();

  std::size_t i = 0;
  while (i < text.size()) {
    auto len = utf8_char_len(static_cast<unsigned char>(text[i]));
    if (len == 0 || i + len > text.size()) {
      result += text[i];
      ++i;
      continue;
    }

    std::string_view ch = text.substr(i, len);

    if (auto it = lower_map.find(ch); it != lower_map.end()) {
      result += it->second;
    } else if (auto it2 = upper_map.find(ch); it2 != upper_map.end()) {
      result += it2->second;
    } else {
      result += ch;
    }

    i += len;
  }

  return result;
}

std::string invert_layout(std::string_view text) {
  if (is_predominantly_cyrillic(text)) {
    return ru_to_en(text);
  }
  return en_to_ru(text);
}

std::string invert_case(std::string_view text) {
  std::string result;
  result.reserve(text.size());

  std::size_t i = 0;
  while (i < text.size()) {
    auto len = utf8_char_len(static_cast<unsigned char>(text[i]));
    if (len == 0 || i + len > text.size()) {
      result += text[i];
      ++i;
      continue;
    }

    std::string_view ch = text.substr(i, len);

    if (len == 1) {
      // ASCII
      char c = text[i];
      if (std::isupper(static_cast<unsigned char>(c))) {
        result +=
            static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else if (std::islower(static_cast<unsigned char>(c))) {
        result +=
            static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      } else {
        result += c;
      }
    } else if (len == 2 && is_cyrillic_char(ch)) {
      // Кириллица
      if (is_cyr_upper(ch)) {
        result += cyr_to_lower(ch);
      } else {
        result += cyr_to_upper(ch);
      }
    } else {
      result += ch;
    }

    i += len;
  }

  return result;
}

std::string cyr_to_lat(std::string_view text) {
  std::string result;
  result.reserve(text.size());

  const auto &multi_map = get_cyr_to_lat_multi();
  const auto &single_map = get_cyr_to_lat_single();

  std::size_t i = 0;
  while (i < text.size()) {
    auto len = utf8_char_len(static_cast<unsigned char>(text[i]));
    if (len == 0 || i + len > text.size()) {
      result += text[i];
      ++i;
      continue;
    }

    std::string_view ch = text.substr(i, len);

    // Сначала проверяем многосимвольные замены
    if (auto it = multi_map.find(ch); it != multi_map.end()) {
      result += it->second;
    } else if (auto it2 = single_map.find(ch); it2 != single_map.end()) {
      result += it2->second;
    } else {
      result += ch;
    }

    i += len;
  }

  return result;
}

std::string lat_to_cyr(std::string_view text) {
  std::string result;
  result.reserve(text.size() * 2);

  const auto &multi_map = get_lat_to_cyr_multi();

  // Сначала заменяем многосимвольные последовательности
  std::string temp{text};
  for (const auto &[from, to] : multi_map) {
    std::size_t pos = 0;
    while ((pos = temp.find(from, pos)) != std::string::npos) {
      temp.replace(pos, from.size(), to);
      pos += to.size();
    }
  }

  // Затем односимвольные
  static const std::unordered_map<char, std::string_view> single_map = {
      {'a', "а"}, {'b', "б"}, {'v', "в"}, {'g', "г"}, {'d', "д"},  {'e', "е"},
      {'z', "з"}, {'i', "и"}, {'j', "й"}, {'k', "к"}, {'l', "л"},  {'m', "м"},
      {'n', "н"}, {'o', "о"}, {'p', "п"}, {'r', "р"}, {'s', "с"},  {'t', "т"},
      {'u', "у"}, {'f', "ф"}, {'h', "х"}, {'c', "ц"}, {'y', "ы"},  {'A', "А"},
      {'B', "Б"}, {'V', "В"}, {'G', "Г"}, {'D', "Д"}, {'E', "Е"},  {'Z', "З"},
      {'I', "И"}, {'J', "Й"}, {'K', "К"}, {'L', "Л"}, {'M', "М"},  {'N', "Н"},
      {'O', "О"}, {'P', "П"}, {'R', "Р"}, {'S', "С"}, {'T', "Т"},  {'U', "У"},
      {'F', "Ф"}, {'H', "Х"}, {'C', "Ц"}, {'Y', "Ы"}, {'\'', "ь"},
  };

  for (char c : temp) {
    if (auto it = single_map.find(c); it != single_map.end()) {
      result += it->second;
    } else {
      result += c;
    }
  }

  return result;
}

std::string transliterate(std::string_view text) {
  if (is_predominantly_cyrillic(text)) {
    return cyr_to_lat(text);
  }
  return lat_to_cyr(text);
}

} // namespace punto
