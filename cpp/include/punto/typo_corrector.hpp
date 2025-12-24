/**
 * @file typo_corrector.hpp
 * @brief Алгоритмы исправления опечаток и ошибок регистра
 *
 * Реализует:
 * 1. Перестановки букв (приевт -> привет)
 * 2. Замены букв (пипца -> пицца)
 * 3. Пропуски букв (поврот -> поворот)
 * 4. Дубли букв (нассекомое -> насекомое)
 * 5. Залипший Shift (ПРивет -> Привет, кОЛБАСА -> Колбаса)
 */

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "punto/types.hpp"

namespace punto {

// ===========================================================================
// Результаты коррекции
// ===========================================================================

/// Результат анализа sticky shift
struct StickyShiftResult {
  bool detected = false;           ///< Обнаружен ли паттерн залипшего Shift
  bool needs_layout_fix = false;   ///< Требуется ли также смена раскладки
  std::vector<KeyEntry> corrected; ///< Исправленное слово (если detected)
};

/// Результат исправления опечатки
struct TypoCorrectionResult {
  bool found = false;                   ///< Найдено ли исправление
  std::string corrected_word;           ///< Исправленное слово (UTF-8)
  std::vector<KeyEntry> corrected_keys; ///< Исправленное слово в KeyEntry
  std::size_t edit_distance = 0;        ///< Расстояние редактирования
};

// ===========================================================================
// Определение паттернов регистра
// ===========================================================================

/// Паттерн регистра в слове
enum class CasePattern {
  Unknown,       ///< Неопределённый паттерн
  AllLower,      ///< все строчные (привет)
  AllUpper,      ///< ВСЕ ЗАГЛАВНЫЕ
  TitleCase,     ///< Первая заглавная, остальные строчные (Привет)
  StickyShiftUU, ///< Залипший Shift: UU+L+ (ПРивет, ДЕРЕвянный)
  StickyShiftLU, ///< Залипший Caps Lock: L+U+ (кОЛБАСА)
  Mixed          ///< Смешанный регистр (СНиП) — НЕ исправляем
};

/**
 * @brief Определяет паттерн регистра в слове
 * @param word Буфер слова (KeyEntry)
 * @return Определённый паттерн
 */
[[nodiscard]] CasePattern detect_case_pattern(std::span<const KeyEntry> word);

// ===========================================================================
// Исправление залипшего Shift (Case 5)
// ===========================================================================

/**
 * @brief Исправляет залипший Shift в слове
 *
 * Обрабатывает паттерны:
 * - UU+L+ (ПРивет) -> U+L+ (Привет)
 * - L+U+ (кОЛБАСА) -> U+L+ (Колбаса)
 *
 * НЕ исправляет смешанный регистр (СНиП).
 *
 * @param word Исходное слово
 * @return Результат с исправленным словом или detected=false
 */
[[nodiscard]] StickyShiftResult
detect_sticky_shift(std::span<const KeyEntry> word);

/**
 * @brief Проверяет sticky shift с учётом возможной ошибки раскладки
 *
 * Для случая GHbdtn -> Привет:
 * 1. Конвертирует раскладку (GHbdtn -> ПРивет)
 * 2. Проверяет паттерн регистра
 * 3. Исправляет регистр
 *
 * @param word Исходное слово
 * @param current_layout Текущая раскладка (0=EN, 1=RU)
 * @return Результат с исправленным словом
 */
[[nodiscard]] StickyShiftResult
detect_sticky_shift_with_layout(std::span<const KeyEntry> word,
                                int current_layout);

// ===========================================================================
// Расстояние редактирования
// ===========================================================================

/**
 * @brief Вычисляет расстояние Дамерау-Левенштейна между двумя строками
 *
 * Учитывает операции:
 * - Вставка символа
 * - Удаление символа
 * - Замена символа
 * - Перестановка соседних символов (транспозиция)
 *
 * @param s1 Первая строка
 * @param s2 Вторая строка
 * @return Минимальное количество операций для преобразования s1 в s2
 */
[[nodiscard]] std::size_t damerau_levenshtein_distance(std::string_view s1,
                                                       std::string_view s2);

/**
 * @brief Вычисляет расстояние Дамерау-Левенштейна для KeyEntry буферов
 * @param word1 Первый буфер
 * @param word2 Второй буфер
 * @return Расстояние редактирования
 */
[[nodiscard]] std::size_t
damerau_levenshtein_distance(std::span<const KeyEntry> word1,
                             std::span<const KeyEntry> word2);

// ===========================================================================
// Исправление опечаток (Cases 1-4)
// ===========================================================================

/**
 * @brief Генерирует кандидатов для исправления опечатки
 *
 * Использует комбинацию:
 * - Hunspell suggest() (если доступен)
 * - Простые эвристики (для коротких слов)
 *
 * @param word Слово с ошибкой (в ASCII/QWERTY)
 * @param max_distance Максимальное расстояние редактирования
 * @return Список кандидатов, отсортированных по приоритету
 */
[[nodiscard]] std::vector<std::string>
generate_typo_candidates(std::string_view word, std::size_t max_distance);

// ===========================================================================
// Утилиты преобразования
// ===========================================================================

/**
 * @brief Конвертирует KeyEntry буфер в ASCII строку (нижний регистр)
 * @param word Буфер слова
 * @return ASCII строка
 */
[[nodiscard]] std::string keys_to_ascii(std::span<const KeyEntry> word);

/**
 * @brief Конвертирует KeyEntry буфер в UTF-8 строку
 *
 * Для русской раскладки (is_english=false) конвертирует scancode в кириллицу.
 * Для английской — в ASCII латиницу.
 *
 * @param word Буфер слова
 * @param is_english true для EN раскладки, false для RU
 * @return UTF-8 строка (lowercase)
 */
[[nodiscard]] std::string keys_to_utf8(std::span<const KeyEntry> word,
                                       bool is_english);

/**
 * @brief Конвертирует ASCII строку в KeyEntry буфер
 * @param ascii ASCII строка
 * @param preserve_case Сохранять ли регистр из исходного слова
 * @param original_word Исходное слово для восстановления регистра
 * @return Буфер KeyEntry
 */
[[nodiscard]] std::vector<KeyEntry>
ascii_to_keys(std::string_view ascii, bool preserve_case = false,
              std::span<const KeyEntry> original_word = {});

/**
 * @brief Конвертирует UTF-8 строку в KeyEntry буфер
 *
 * Для русского текста конвертирует кириллицу в scancode.
 * Для английского — латиницу.
 *
 * @param utf8 UTF-8 строка
 * @param is_english true для EN раскладки, false для RU
 * @param preserve_case Сохранять ли регистр из исходного слова
 * @param original_word Исходное слово для восстановления регистра
 * @return Буфер KeyEntry
 */
[[nodiscard]] std::vector<KeyEntry>
utf8_to_keys(std::string_view utf8, bool is_english, bool preserve_case = false,
             std::span<const KeyEntry> original_word = {});

/**
 * @brief Применяет регистр исходного слова к исправленному
 *
 * Например, если исходное слово "ПРивет" (с sticky shift),
 * а исправление "привет", результат будет "Привет".
 *
 * @param corrected Исправленное слово (без учёта регистра)
 * @param original Исходное слово (с информацией о регистре)
 * @return Слово с восстановленным регистром
 */
[[nodiscard]] std::vector<KeyEntry>
apply_case_pattern(std::span<const KeyEntry> corrected,
                   CasePattern target_pattern);

} // namespace punto
