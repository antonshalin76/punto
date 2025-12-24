/**
 * @file scancode_map.hpp
 * @brief Маппинг скан-кодов на символы и раскладки EN/RU
 *
 * Constexpr таблицы для преобразования между скан-кодами и символами.
 * Используются compile-time lookup tables для максимальной производительности.
 */

#pragma once

#include <linux/input.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace punto {

// ===========================================================================
// Маппинг скан-кодов на ASCII-символы (для идентификации "буквенных" клавиш)
// ===========================================================================

inline constexpr std::array<char, 256> kScancodeToChar = [] {
  std::array<char, 256> map{};
  // Буквы QWERTY
  map[KEY_Q] = 'q';
  map[KEY_W] = 'w';
  map[KEY_E] = 'e';
  map[KEY_R] = 'r';
  map[KEY_T] = 't';
  map[KEY_Y] = 'y';
  map[KEY_U] = 'u';
  map[KEY_I] = 'i';
  map[KEY_O] = 'o';
  map[KEY_P] = 'p';
  map[KEY_A] = 'a';
  map[KEY_S] = 's';
  map[KEY_D] = 'd';
  map[KEY_F] = 'f';
  map[KEY_G] = 'g';
  map[KEY_H] = 'h';
  map[KEY_J] = 'j';
  map[KEY_K] = 'k';
  map[KEY_L] = 'l';
  map[KEY_Z] = 'z';
  map[KEY_X] = 'x';
  map[KEY_C] = 'c';
  map[KEY_V] = 'v';
  map[KEY_B] = 'b';
  map[KEY_N] = 'n';
  map[KEY_M] = 'm';
  // Скобки и знаки препинания (важно для русских х, ъ, ж, э)
  map[KEY_LEFTBRACE] = '[';
  map[KEY_RIGHTBRACE] = ']';
  map[KEY_SEMICOLON] = ';';
  map[KEY_APOSTROPHE] = '\'';
  map[KEY_GRAVE] = '`';
  map[KEY_SLASH] = '/';
  // Цифры основной клавиатуры
  map[KEY_1] = '1';
  map[KEY_2] = '2';
  map[KEY_3] = '3';
  map[KEY_4] = '4';
  map[KEY_5] = '5';
  map[KEY_6] = '6';
  map[KEY_7] = '7';
  map[KEY_8] = '8';
  map[KEY_9] = '9';
  map[KEY_0] = '0';
  // Numpad цифры
  map[KEY_KP0] = '0';
  map[KEY_KP1] = '1';
  map[KEY_KP2] = '2';
  map[KEY_KP3] = '3';
  map[KEY_KP4] = '4';
  map[KEY_KP5] = '5';
  map[KEY_KP6] = '6';
  map[KEY_KP7] = '7';
  map[KEY_KP8] = '8';
  map[KEY_KP9] = '9';
  // Numpad операторы
  map[KEY_KPMINUS] = '-';
  map[KEY_KPPLUS] = '+';
  map[KEY_KPASTERISK] = '*';
  map[KEY_KPSLASH] = '/';
  map[KEY_KPDOT] = '.';
  // Дополнительные символы
  map[KEY_MINUS] = '-';
  map[KEY_EQUAL] = '=';
  map[KEY_BACKSLASH] = '\\';
  // Важно: точка и запятая — это русские "ю" и "б"!
  map[KEY_COMMA] = ',';
  map[KEY_DOT] = '.';
  return map;
}();

/// Проверка, является ли скан-код "буквенной" клавишей
[[nodiscard]] constexpr bool is_letter_key(std::uint16_t code) noexcept {
  return code < kScancodeToChar.size() && kScancodeToChar[code] != '\0';
}

/// Проверка, является ли скан-код клавишей, которая генерирует БУКВУ
/// (для EN раскладки — a-z, для RU раскладки — а-я включая б, ю)
[[nodiscard]] constexpr bool is_typeable_letter(std::uint16_t code) noexcept {
  // Буквы A-Z
  if (code >= KEY_Q && code <= KEY_P)
    return true; // Q-P row
  if (code >= KEY_A && code <= KEY_L)
    return true; // A-L row
  if (code >= KEY_Z && code <= KEY_M)
    return true; // Z-M row
  // Русские буквы на небуквенных клавишах EN:
  // [ = х, ] = ъ, ; = ж, ' = э
  if (code == KEY_LEFTBRACE || code == KEY_RIGHTBRACE ||
      code == KEY_SEMICOLON || code == KEY_APOSTROPHE)
    return true;
  // , = б, . = ю (важно для русского!)
  if (code == KEY_COMMA || code == KEY_DOT)
    return true;
  // ` = ё
  if (code == KEY_GRAVE)
    return true;
  return false;
}

// ===========================================================================
// Маппинг EN -> RU (QWERTY -> ЙЦУКЕН)
// ===========================================================================

/// Структура для хранения пары char -> UTF-8 строка
struct CharMapping {
  char from;
  std::string_view to;
};

// clang-format off
inline constexpr std::array kEnToRuLower = std::to_array<CharMapping>({
    {'q', "й"}, {'w', "ц"}, {'e', "у"}, {'r', "к"}, {'t', "е"}, {'y', "н"},
    {'u', "г"}, {'i', "ш"}, {'o', "щ"}, {'p', "з"}, {'[', "х"}, {']', "ъ"},
    {'a', "ф"}, {'s', "ы"}, {'d', "в"}, {'f', "а"}, {'g', "п"}, {'h', "р"},
    {'j', "о"}, {'k', "л"}, {'l', "д"}, {';', "ж"}, {'\'', "э"},
    {'z', "я"}, {'x', "ч"}, {'c', "с"}, {'v', "м"}, {'b', "и"}, {'n', "т"},
    {'m', "ь"}, {',', "б"}, {'.', "ю"}, {'`', "ё"}, {'/', "."},
});

inline constexpr std::array kEnToRuUpper = std::to_array<CharMapping>({
    {'Q', "Й"}, {'W', "Ц"}, {'E', "У"}, {'R', "К"}, {'T', "Е"}, {'Y', "Н"},
    {'U', "Г"}, {'I', "Ш"}, {'O', "Щ"}, {'P', "З"}, {'{', "Х"}, {'}', "Ъ"},
    {'A', "Ф"}, {'S', "Ы"}, {'D', "В"}, {'F', "А"}, {'G', "П"}, {'H', "Р"},
    {'J', "О"}, {'K', "Л"}, {'L', "Д"}, {':', "Ж"}, {'"', "Э"},
    {'Z', "Я"}, {'X', "Ч"}, {'C', "С"}, {'V', "М"}, {'B', "И"}, {'N', "Т"},
    {'M', "Ь"}, {'<', "Б"}, {'>', "Ю"}, {'~', "Ё"},
});

// ===========================================================================
// Маппинг RU -> EN (ЙЦУКЕН -> QWERTY)
// ===========================================================================

/// Структура для UTF-8 -> char маппинга
struct Utf8Mapping {
    std::string_view from;
    char to;
};

inline constexpr std::array kRuToEnLower = std::to_array<Utf8Mapping>({
    {"й", 'q'}, {"ц", 'w'}, {"у", 'e'}, {"к", 'r'}, {"е", 't'}, {"н", 'y'},
    {"г", 'u'}, {"ш", 'i'}, {"щ", 'o'}, {"з", 'p'}, {"х", '['}, {"ъ", ']'},
    {"ф", 'a'}, {"ы", 's'}, {"в", 'd'}, {"а", 'f'}, {"п", 'g'}, {"р", 'h'},
    {"о", 'j'}, {"л", 'k'}, {"д", 'l'}, {"ж", ';'}, {"э", '\''},
    {"я", 'z'}, {"ч", 'x'}, {"с", 'c'}, {"м", 'v'}, {"и", 'b'}, {"т", 'n'},
    {"ь", 'm'}, {"б", ','}, {"ю", '.'}, {"ё", '`'},
});

inline constexpr std::array kRuToEnUpper = std::to_array<Utf8Mapping>({
    {"Й", 'Q'}, {"Ц", 'W'}, {"У", 'E'}, {"К", 'R'}, {"Е", 'T'}, {"Н", 'Y'},
    {"Г", 'U'}, {"Ш", 'I'}, {"Щ", 'O'}, {"З", 'P'}, {"Х", '{'}, {"Ъ", '}'},
    {"Ф", 'A'}, {"Ы", 'S'}, {"В", 'D'}, {"А", 'F'}, {"П", 'G'}, {"Р", 'H'},
    {"О", 'J'}, {"Л", 'K'}, {"Д", 'L'}, {"Ж", ':'}, {"Э", '"'},
    {"Я", 'Z'}, {"Ч", 'X'}, {"С", 'C'}, {"М", 'V'}, {"И", 'B'}, {"Т", 'N'},
    {"Ь", 'M'}, {"Б", '<'}, {"Ю", '>'}, {"Ё", '~'},
});
// clang-format on

// ===========================================================================
// Транслитерация CYR -> LAT
// ===========================================================================

inline constexpr std::array kCyrToLatLower = std::to_array<Utf8Mapping>({
    {"а", 'a'}, {"б", 'b'}, {"в", 'v'}, {"г", 'g'}, {"д", 'd'}, {"е", 'e'},
    {"з", 'z'}, {"и", 'i'}, {"й", 'j'}, {"к", 'k'}, {"л", 'l'}, {"м", 'm'},
    {"н", 'n'}, {"о", 'o'}, {"п", 'p'}, {"р", 'r'}, {"с", 's'}, {"т", 't'},
    {"у", 'u'}, {"ф", 'f'}, {"х", 'h'}, {"ц", 'c'}, {"ы", 'y'},
});

inline constexpr std::array kCyrToLatUpper = std::to_array<Utf8Mapping>({
    {"А", 'A'}, {"Б", 'B'}, {"В", 'V'}, {"Г", 'G'}, {"Д", 'D'}, {"Е", 'E'},
    {"З", 'Z'}, {"И", 'I'}, {"Й", 'J'}, {"К", 'K'}, {"Л", 'L'}, {"М", 'M'},
    {"Н", 'N'}, {"О", 'O'}, {"П", 'P'}, {"Р", 'R'}, {"С", 'S'}, {"Т", 'T'},
    {"У", 'U'}, {"Ф", 'F'}, {"Х", 'H'}, {"Ц", 'C'}, {"Ы", 'Y'},
});

/// Многосимвольные транслитерации
struct MultiCharTranslit {
  std::string_view from;
  std::string_view to;
};

inline constexpr std::array kCyrToLatMulti = std::to_array<MultiCharTranslit>({
    {"ё", "yo"}, {"ж", "zh"}, {"ч", "ch"}, {"ш", "sh"}, {"щ", "shch"},
    {"ъ", ""},   {"ь", "'"},  {"э", "e"},  {"ю", "yu"}, {"я", "ya"},
    {"Ё", "Yo"}, {"Ж", "Zh"}, {"Ч", "Ch"}, {"Ш", "Sh"}, {"Щ", "Shch"},
    {"Ъ", ""},   {"Ь", "'"},  {"Э", "E"},  {"Ю", "Yu"}, {"Я", "Ya"},
});

/// Обратные многосимвольные транслитерации LAT -> CYR
inline constexpr std::array kLatToCyrMulti = std::to_array<MultiCharTranslit>({
    {"shch", "щ"}, {"Shch", "Щ"}, {"SHCH", "Щ"}, {"yo", "ё"}, {"Yo", "Ё"},
    {"YO", "Ё"},   {"zh", "ж"},   {"Zh", "Ж"},   {"ZH", "Ж"}, {"ch", "ч"},
    {"Ch", "Ч"},   {"CH", "Ч"},   {"sh", "ш"},   {"Sh", "Ш"}, {"SH", "Ш"},
    {"yu", "ю"},   {"Yu", "Ю"},   {"YU", "Ю"},   {"ya", "я"}, {"Ya", "Я"},
    {"YA", "Я"},
});

// ===========================================================================
// Имена клавиш для парсинга конфигурации
// ===========================================================================

struct KeyNameMapping {
  std::string_view name;
  std::uint16_t code;
};

inline constexpr std::array kKeyNames = std::to_array<KeyNameMapping>({
    {"leftctrl", KEY_LEFTCTRL},
    {"rightctrl", KEY_RIGHTCTRL},
    {"leftalt", KEY_LEFTALT},
    {"rightalt", KEY_RIGHTALT},
    {"leftshift", KEY_LEFTSHIFT},
    {"rightshift", KEY_RIGHTSHIFT},
    {"leftmeta", KEY_LEFTMETA},
    {"rightmeta", KEY_RIGHTMETA},
    {"grave", KEY_GRAVE},
    {"space", KEY_SPACE},
    {"tab", KEY_TAB},
    {"backslash", KEY_BACKSLASH},
    {"capslock", KEY_CAPSLOCK},
});

/// Поиск кода клавиши по имени
[[nodiscard]] constexpr std::optional<std::uint16_t>
key_name_to_code(std::string_view name) noexcept {
  for (const auto &mapping : kKeyNames) {
    if (mapping.name == name) {
      return mapping.code;
    }
  }
  return std::nullopt;
}

} // namespace punto
