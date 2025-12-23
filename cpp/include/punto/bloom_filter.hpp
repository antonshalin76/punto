/**
 * @file bloom_filter.hpp
 * @brief Вероятностная структура данных для быстрого отсечения отсутствующих слов
 *
 * Bloom Filter позволяет за O(1) с минимальным cache miss сказать:
 * - "Точно НЕТ в словаре" (100% достоверность)
 * - "ВОЗМОЖНО есть" (требует проверки в основной структуре)
 *
 * Размер подобран для ~100k слов с false positive rate < 1%
 */

#pragma once

#include "punto/hasher.hpp"
#include "punto/types.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <span>
#include <string_view>

namespace punto {

/**
 * @brief Компактный Bloom Filter для словарного lookup
 *
 * Параметры:
 * - Размер: 2^20 бит = 128 KB (помещается в L2/L3 кэш)
 * - k = 7 хеш-функций (оптимально для n=100k, m=1M)
 * - Ожидаемый false positive rate: ~0.8% при 100k элементах
 *
 * Использует double hashing для генерации k хешей из двух базовых.
 */
class BloomFilter {
public:
  // Размер фильтра: 2^20 бит = 1,048,576 бит = 128 KB
  static constexpr std::size_t kBitCount = 1U << 20;
  // Количество хеш-функций
  static constexpr std::size_t kHashCount = 7;
  // Маска для быстрого модуля (размер — степень двойки)
  static constexpr std::uint64_t kMask = kBitCount - 1;

  BloomFilter() = default;

  /**
   * @brief Добавляет слово в фильтр (при загрузке словаря)
   * @param word Строка слова (lowercase ASCII)
   */
  void add(std::string_view word) noexcept {
    std::uint64_t h1, h2;
    Hasher::hash_string_double(word, h1, h2);
    add_hashes(h1, h2);
  }

  /**
   * @brief Добавляет элемент по предвычисленным хешам
   */
  void add_hashes(std::uint64_t h1, std::uint64_t h2) noexcept {
    for (std::size_t i = 0; i < kHashCount; ++i) {
      std::size_t idx = static_cast<std::size_t>((h1 + i * h2) & kMask);
      bits_[idx] = true;
    }
  }

  /**
   * @brief Проверяет, может ли слово быть в словаре
   * @param entries Span скан-кодов слова
   * @return false = точно НЕТ, true = возможно есть
   */
  [[nodiscard]] bool maybe_contains(
      std::span<const KeyEntry> entries) const noexcept {
    std::uint64_t h1, h2;
    Hasher::hash_entries_double(entries, h1, h2);
    return maybe_contains_hashes(h1, h2);
  }

  /**
   * @brief Проверяет по предвычисленным хешам
   */
  [[nodiscard]] bool maybe_contains_hashes(std::uint64_t h1,
                                           std::uint64_t h2) const noexcept {
    for (std::size_t i = 0; i < kHashCount; ++i) {
      std::size_t idx = static_cast<std::size_t>((h1 + i * h2) & kMask);
      if (!bits_[idx]) {
        return false; // Точно нет
      }
    }
    return true; // Возможно есть
  }

  /**
   * @brief Сбрасывает фильтр
   */
  void clear() noexcept { bits_.reset(); }

  /**
   * @brief Возвращает количество установленных бит (для статистики)
   */
  [[nodiscard]] std::size_t popcount() const noexcept { return bits_.count(); }

  /**
   * @brief Возвращает заполненность фильтра (0.0 - 1.0)
   */
  [[nodiscard]] double fill_ratio() const noexcept {
    return static_cast<double>(bits_.count()) / static_cast<double>(kBitCount);
  }

private:
  std::bitset<kBitCount> bits_;
};

} // namespace punto
