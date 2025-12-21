/**
 * @file dictionary.hpp
 * @brief Словарный анализатор для определения языка слова
 *
 * Загружает словари из системных hunspell файлов для высокой точности
 * определения языка.
 */

#pragma once

#include <span>
#include <string>
#include <unordered_set>

#include "punto/types.hpp"

namespace punto {

/// Результат словарной проверки
enum class DictResult {
  Unknown, // Слово не найдено в словарях
  English, // Слово найдено в EN словаре
  Russian, // Слово найдено в RU словаре (как QWERTY-последовательность)
  Both     // Слово найдено в обоих (редко)
};

/**
 * @brief Словарный анализатор языка
 *
 * Загружает словари из hunspell файлов:
 * - /usr/share/hunspell/en_US.dic — английские слова
 * - /usr/share/hunspell/ru_RU.dic — русские слова (конвертируются в QWERTY)
 */
class Dictionary {
public:
  /**
   * @brief Инициализирует словари из hunspell файлов
   * @return true если успешно загружены
   */
  bool initialize();

  /**
   * @brief Ищет слово в словарях
   * @param entries Буфер скан-кодов (слово)
   * @return Результат поиска
   */
  [[nodiscard]] DictResult lookup(std::span<const KeyEntry> entries) const;

  /**
   * @brief Проверяет, инициализирован ли словарь
   */
  [[nodiscard]] bool is_ready() const noexcept { return initialized_; }

  /**
   * @brief Возвращает размер EN словаря
   */
  [[nodiscard]] std::size_t en_size() const noexcept {
    return en_words_.size();
  }

  /**
   * @brief Возвращает размер RU словаря
   */
  [[nodiscard]] std::size_t ru_size() const noexcept {
    return ru_words_.size();
  }

private:
  /**
   * @brief Конвертирует буфер скан-кодов в строку (lowercase)
   */
  [[nodiscard]] static std::string
  entries_to_key(std::span<const KeyEntry> entries);

  /**
   * @brief Загружает английский словарь из hunspell
   * @param path Путь к .dic файлу
   * @return Количество загруженных слов
   */
  std::size_t load_en_dictionary(const std::string &path);

  /**
   * @brief Загружает русский словарь из hunspell, конвертируя в QWERTY
   * @param path Путь к .dic файлу
   * @return Количество загруженных слов
   */
  std::size_t load_ru_dictionary(const std::string &path);

  /**
   * @brief Конвертирует UTF-8 кириллицу в QWERTY-последовательность
   * @param cyrillic UTF-8 строка с кириллицей
   * @return QWERTY-эквивалент (только ASCII)
   */
  [[nodiscard]] static std::string
  cyrillic_to_qwerty(const std::string &cyrillic);

  std::unordered_set<std::string> en_words_;
  std::unordered_set<std::string> ru_words_; // QWERTY-последовательности
  bool initialized_ = false;
};

} // namespace punto
