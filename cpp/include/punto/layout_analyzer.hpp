/**
 * @file layout_analyzer.hpp
 * @brief Анализатор раскладки для автоматического переключения
 *
 * Использует частотный анализ биграмм (пар букв) для определения
 * вероятности того, что слово набрано в неправильной раскладке.
 */

#pragma once

#include <span>

#include "punto/config.hpp"
#include "punto/types.hpp"

namespace punto {

/// Язык раскладки
enum class Language { English, Russian };

/// Результат анализа слова
struct AnalysisResult {
  double en_score = 0.0; // Скор для английской раскладки
  double ru_score = 0.0; // Скор для русской раскладки
  Language likely_lang = Language::English;
  bool should_switch = false;
};

/**
 * @brief Анализатор раскладки на основе биграмм
 *
 * Вычисляет вероятность принадлежности слова к EN или RU языку
 * на основе частотности биграмм (пар символов).
 */
class LayoutAnalyzer {
public:
  /**
   * @brief Конструктор
   * @param config Конфигурация автопереключения
   */
  explicit LayoutAnalyzer(AutoSwitchConfig config);

  /**
   * @brief Определяет, нужно ли переключать раскладку
   * @param word Буфер слова (скан-коды с информацией о Shift)
   * @return true если слово вероятно набрано в неправильной раскладке
   */
  [[nodiscard]] bool should_switch(std::span<const KeyEntry> word) const;

  /**
   * @brief Выполняет полный анализ слова
   * @param word Буфер слова (скан-коды с информацией о Shift)
   * @return Структура с результатами анализа
   */
  [[nodiscard]] AnalysisResult analyze(std::span<const KeyEntry> word) const;

  /**
   * @brief Вычисляет скор для указанной раскладки
   * @param word Буфер слова
   * @param lang Язык для анализа
   * @return Числовой скор (чем выше, тем вероятнее)
   */
  [[nodiscard]] double calculate_score(std::span<const KeyEntry> word,
                                       Language lang) const;

private:
  /**
   * @brief Конвертирует скан-код в ASCII символ (нижний регистр)
   * @param code Скан-код клавиши
   * @return Символ или '\0' если не буква
   */
  [[nodiscard]] static char scancode_to_lowercase(ScanCode code);

  /**
   * @brief Проверяет, содержит ли слово цифры или спецсимволы
   * @param word Буфер слова
   * @return true если слово содержит нежелательные символы
   */
  [[nodiscard]] static bool has_invalid_chars(std::span<const KeyEntry> word);

  /**
   * @brief Конвертирует буфер в строку ASCII символов
   * @param word Буфер слова
   * @param buffer Выходной буфер (должен быть >= word.size())
   * @return Количество записанных символов
   */
  [[nodiscard]] static std::size_t word_to_ascii(std::span<const KeyEntry> word,
                                                 char *buffer);

  AutoSwitchConfig config_;
};

} // namespace punto
