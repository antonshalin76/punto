/**
 * @file dictionary.hpp
 * @brief Словарный анализатор для определения языка слова
 *
 * Загружает словари из системных hunspell файлов для высокой точности
 * определения языка.
 */

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#ifdef HAVE_HUNSPELL
#include <hunspell/hunspell.hxx>
#endif

#include "punto/bloom_filter.hpp"
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
 * Использует libhunspell для проверки словоформ с учётом:
 * - Падежей, склонений, времён, родов и т.п.
 * - Аффиксов из .aff файлов
 *
 * Двусторонняя проверка: конвертирует слово в обе раскладки и ищет в словарях.
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
    return en_hashes_.size();
  }

  /**
   * @brief Возвращает размер RU словаря
   */
  [[nodiscard]] std::size_t ru_size() const noexcept {
    return ru_hashes_.size();
  }

  [[nodiscard]] double en_bloom_fill() const noexcept {
    return en_bloom_.fill_ratio();
  }
  [[nodiscard]] double ru_bloom_fill() const noexcept {
    return ru_bloom_.fill_ratio();
  }

  /**
   * @brief Проверяет, доступен ли Hunspell для предложений
   */
  [[nodiscard]] bool is_hunspell_available() const noexcept {
    return hunspell_available_;
  }

  /**
   * @brief Генерирует предложения исправления для слова
   *
   * Использует Hunspell suggest() для генерации кандидатов.
   * Работает только если Hunspell доступен.
   *
   * @param word Слово с ошибкой (ASCII для EN, UTF-8 для RU)
   * @param is_english true для EN словаря, false для RU
   * @param max_suggestions Максимальное количество предложений
   * @return Список предложений (UTF-8)
   */
  [[nodiscard]] std::vector<std::string>
  suggest(const std::string &word, bool is_english,
          std::size_t max_suggestions = 10) const;

  /**
   * @brief Проверяет правильность слова через Hunspell
   *
   * @param word Слово для проверки (UTF-8)
   * @param is_english true для EN словаря, false для RU
   * @return true если слово правильное
   */
  [[nodiscard]] bool spell(const std::string &word, bool is_english) const {
    return check_hunspell(word, is_english);
  }

private:
  /**
   * @brief Проверяет наличие хеша в отсортированном векторе
   * @param hash Хеш слова
   * @param hashes Отсортированный вектор хешей
   * @return true если найден
   */
  [[nodiscard]] static bool
  hash_exists(std::uint64_t hash,
              const std::vector<std::uint64_t> &hashes) noexcept;

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

  /**
   * @brief Финализирует хеши после загрузки всех словарей
   *
   * Сортирует и удаляет дубликаты для эффективного бинарного поиска.
   */
  void finalize_hashes();

  /**
   * @brief Конвертирует QWERTY в кириллицу (UTF-8)
   * @param qwerty ASCII строка (QWERTY клавиши)
   * @return UTF-8 строка с кириллицей
   */
  [[nodiscard]] static std::string
  qwerty_to_cyrillic(const std::string &qwerty);

  /**
   * @brief Проверяет слово через hunspell (c учётом словоформ)
   * @param word Слово для проверки
   * @param is_english true для EN словаря, false для RU
   * @return true если слово корректно
   */
  [[nodiscard]] bool check_hunspell(const std::string &word,
                                    bool is_english) const;

  // Bloom Filters для быстрого отсечения (Level 0)
  BloomFilter en_bloom_;
  BloomFilter ru_bloom_;

  // Отсортированные векторы хешей (Level 1-2) — резервный метод
  std::vector<std::uint64_t> en_hashes_;
  std::vector<std::uint64_t> ru_hashes_;

#ifdef HAVE_HUNSPELL
  // Hunspell для проверки словоформ
  std::unique_ptr<Hunspell> hunspell_en_;
  std::unique_ptr<Hunspell> hunspell_ru_;
#endif

  bool initialized_ = false;
  bool hunspell_available_ = false;
};

} // namespace punto
