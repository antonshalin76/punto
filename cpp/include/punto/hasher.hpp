/**
 * @file hasher.hpp
 * @brief Zero-allocation хеширование для быстрого dictionary lookup
 *
 * Реализация FNV-1a 64-bit хеша с поддержкой:
 * - Хеширование напрямую из std::span<KeyEntry> (без аллокаций)
 * - Хеширование строк при загрузке словаря
 */

#pragma once

#include "punto/scancode_map.hpp"
#include "punto/types.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace punto {

/**
 * @brief FNV-1a 64-bit хешер
 *
 * Выбран FNV-1a за:
 * - Простоту реализации (инлайнится полностью)
 * - Хорошее распределение для коротких строк
 * - Отсутствие внешних зависимостей
 */
class Hasher {
public:
  // FNV-1a константы для 64-bit
  static constexpr std::uint64_t kFnvBasis = 14695981039346656037ULL;
  static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

  /**
   * @brief Хеширует строку (используется при загрузке словаря)
   * @param str Строка для хеширования
   * @return 64-bit хеш
   */
  [[nodiscard]] static constexpr std::uint64_t
  hash_string(std::string_view str) noexcept {
    std::uint64_t hash = kFnvBasis;
    for (char c : str) {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
      hash *= kFnvPrime;
    }
    return hash;
  }

  /**
   * @brief Хеширует span KeyEntry без аллокаций
   *
   * Конвертирует скан-коды в lowercase ASCII и хеширует на лету.
   * Полностью инлайнится компилятором.
   *
   * @param entries Span скан-кодов слова
   * @return 64-bit хеш (0 если пустой или невалидный)
   */
  [[nodiscard]] static std::uint64_t
  hash_entries(std::span<const KeyEntry> entries) noexcept {
    if (entries.empty()) {
      return 0;
    }

    std::uint64_t hash = kFnvBasis;
    bool has_valid_char = false;

    for (const auto &entry : entries) {
      if (entry.code < kScancodeToChar.size()) {
        char c = kScancodeToChar[entry.code];
        if (c != '\0') {
          // Приводим к нижнему регистру
          if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
          }
          hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
          hash *= kFnvPrime;
          has_valid_char = true;
        }
      }
    }

    return has_valid_char ? hash : 0;
  }

  /**
   * @brief Вычисляет два независимых хеша для Bloom Filter
   *
   * Использует технику double hashing: h1 и h2 комбинируются
   * для получения k хешей: h_i = h1 + i * h2
   *
   * @param entries Span скан-кодов
   * @param[out] h1 Первый хеш
   * @param[out] h2 Второй хеш (производный)
   */
  static void hash_entries_double(std::span<const KeyEntry> entries,
                                  std::uint64_t &h1,
                                  std::uint64_t &h2) noexcept {
    h1 = hash_entries(entries);
    // Второй хеш — XOR с другой константой и повторный проход
    // Для простоты используем битовые операции над h1
    h2 = (h1 >> 17) | (h1 << 47);
    h2 *= kFnvPrime;
    h2 ^= (h1 >> 31);
  }

  /**
   * @brief Вычисляет два хеша для строки (Bloom Filter при загрузке)
   */
  static void hash_string_double(std::string_view str, std::uint64_t &h1,
                                 std::uint64_t &h2) noexcept {
    h1 = hash_string(str);
    h2 = (h1 >> 17) | (h1 << 47);
    h2 *= kFnvPrime;
    h2 ^= (h1 >> 31);
  }
};

} // namespace punto
