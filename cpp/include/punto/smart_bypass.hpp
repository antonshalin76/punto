/**
 * @file smart_bypass.hpp
 * @brief Детектирование слов, которые не требуют автокоррекции
 *
 * Фильтрует URL, пути, camelCase, snake_case и другие технические паттерны,
 * чтобы избежать ложных срабатываний автопереключения раскладки.
 */

#pragma once

#include <span>

#include "punto/types.hpp"

namespace punto {

/// Причина пропуска анализа слова
enum class BypassReason {
  None,              /// Слово требует анализа
  UrlDetected,       /// Обнаружен URL (http://, www., @)
  PathDetected,      /// Обнаружен путь (/, \, начинается с .)
  CamelCaseDetected, /// Обнаружен camelCase (myVariable)
  SnakeCaseDetected, /// Обнаружен snake_case (my_variable)
  AllCapsAcronym,    /// Аббревиатура из заглавных (API, URL)
  TooShort,          /// Слово слишком короткое для надёжного анализа
};

/**
 * @brief Проверяет, нужно ли пропустить анализ слова
 *
 * @param word Слово в виде массива KeyEntry
 * @param min_word_len Минимальная длина слова для анализа
 * @return Причина пропуска или None если анализ нужен
 */
[[nodiscard]] BypassReason should_bypass(std::span<const KeyEntry> word,
                                         std::size_t min_word_len = 2);

/**
 * @brief Проверяет, является ли слово в формате camelCase
 *
 * CamelCase: первая буква строчная, затем есть переход lower→upper.
 * Например: myVariable, iPhone, getElementById
 *
 * @param word Слово в виде массива KeyEntry
 * @return true если обнаружен camelCase паттерн
 */
[[nodiscard]] bool is_camel_case(std::span<const KeyEntry> word);

/**
 * @brief Проверяет, является ли слово в формате PascalCase
 *
 * PascalCase: первая буква заглавная, затем есть переход lower→upper.
 * Например: MyVariable, HttpRequest
 *
 * @param word Слово в виде массива KeyEntry
 * @return true если обнаружен PascalCase паттерн
 */
[[nodiscard]] bool is_pascal_case(std::span<const KeyEntry> word);

/**
 * @brief Проверяет, содержит ли слово символы пути или URL
 *
 * Детектирует: /, \, @, http, www, начало с .
 *
 * @param word Слово в виде массива KeyEntry
 * @return true если обнаружены URL/path паттерны
 */
[[nodiscard]] bool contains_url_or_path_chars(std::span<const KeyEntry> word);

/**
 * @brief Проверяет, содержит ли слово подчёркивание (snake_case)
 *
 * @param word Слово в виде массива KeyEntry
 * @return true если слово содержит подчёркивание
 */
[[nodiscard]] bool is_snake_case(std::span<const KeyEntry> word);

/**
 * @brief Проверяет, является ли слово аббревиатурой из заглавных букв
 *
 * Аббревиатуры: 2-5 символов, все заглавные.
 * Например: API, URL, HTTP, DNS
 *
 * @param word Слово в виде массива KeyEntry
 * @return true если слово похоже на аббревиатуру
 */
[[nodiscard]] bool is_all_caps_acronym(std::span<const KeyEntry> word);

} // namespace punto
