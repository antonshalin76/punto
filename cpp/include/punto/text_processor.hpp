/**
 * @file text_processor.hpp
 * @brief Обработка текста: инверсия раскладки, регистра, транслитерация
 *
 * Чистые функции для преобразования UTF-8 текста.
 * Замена Python скриптов punto-invert, punto-translit, punto-case-invert.
 */

#pragma once

#include <string>
#include <string_view>

namespace punto {

// ===========================================================================
// Определение типа текста
// ===========================================================================

/**
 * @brief Определяет преобладающий язык текста
 * @param text UTF-8 строка
 * @return true если текст преимущественно кириллический
 */
[[nodiscard]] bool is_predominantly_cyrillic(std::string_view text);

/**
 * @brief Подсчитывает количество кириллических символов
 * @param text UTF-8 строка
 * @return Пара (кириллические, всего букв)
 */
[[nodiscard]] std::pair<std::size_t, std::size_t>
count_letters(std::string_view text);

// ===========================================================================
// Инверсия раскладки
// ===========================================================================

/**
 * @brief Инвертирует раскладку текста (EN <-> RU)
 * @param text UTF-8 строка
 * @return Преобразованная строка
 *
 * Автоматически определяет направление преобразования по содержимому.
 */
[[nodiscard]] std::string invert_layout(std::string_view text);

/**
 * @brief Преобразует EN -> RU
 * @param text ASCII строка с английскими буквами
 * @return UTF-8 строка с русскими буквами
 */
[[nodiscard]] std::string en_to_ru(std::string_view text);

/**
 * @brief Преобразует RU -> EN
 * @param text UTF-8 строка с русскими буквами
 * @return ASCII строка с английскими буквами
 */
[[nodiscard]] std::string ru_to_en(std::string_view text);

// ===========================================================================
// Инверсия регистра
// ===========================================================================

/**
 * @brief Инвертирует регистр каждого символа (swapcase)
 * @param text UTF-8 строка
 * @return Строка с инвертированным регистром
 *
 * Обрабатывает как латиницу, так и кириллицу.
 */
[[nodiscard]] std::string invert_case(std::string_view text);

// ===========================================================================
// Транслитерация
// ===========================================================================

/**
 * @brief Транслитерирует текст (CYR <-> LAT)
 * @param text UTF-8 строка
 * @return Транслитерированная строка
 *
 * Автоматически определяет направление по содержимому.
 */
[[nodiscard]] std::string transliterate(std::string_view text);

/**
 * @brief Транслитерирует CYR -> LAT
 * @param text UTF-8 строка с кириллицей
 * @return ASCII строка (латиница)
 */
[[nodiscard]] std::string cyr_to_lat(std::string_view text);

/**
 * @brief Транслитерирует LAT -> CYR
 * @param text ASCII строка с латиницей
 * @return UTF-8 строка (кириллица)
 */
[[nodiscard]] std::string lat_to_cyr(std::string_view text);

// ===========================================================================
// UTF-8 утилиты
// ===========================================================================

/**
 * @brief Определяет длину UTF-8 символа по первому байту
 * @param first_byte Первый байт UTF-8 последовательности
 * @return Длина в байтах (1-4), или 0 для невалидного байта
 */
[[nodiscard]] constexpr std::size_t
utf8_char_len(unsigned char first_byte) noexcept {
  if ((first_byte & 0x80) == 0)
    return 1; // ASCII
  if ((first_byte & 0xE0) == 0xC0)
    return 2; // 110xxxxx
  if ((first_byte & 0xF0) == 0xE0)
    return 3; // 1110xxxx
  if ((first_byte & 0xF8) == 0xF0)
    return 4; // 11110xxx
  return 0;   // Invalid
}

/**
 * @brief Проверяет, является ли символ кириллическим
 * @param utf8_char UTF-8 последовательность (1-4 байта)
 * @return true если это кириллический символ (U+0400 - U+04FF)
 */
[[nodiscard]] bool is_cyrillic_char(std::string_view utf8_char);

/**
 * @brief Проверяет, является ли символ латинским
 * @param c ASCII символ
 * @return true если это латинская буква
 */
[[nodiscard]] constexpr bool is_latin_char(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

} // namespace punto
