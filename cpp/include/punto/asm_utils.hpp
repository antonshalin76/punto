/**
 * @file asm_utils.hpp
 * @brief Ассемблерные оптимизации для горячих путей
 *
 * Inline ASM для x86_64 для критичных по производительности операций.
 * Fallback на C++ реализации для других архитектур.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "punto/types.hpp"

namespace punto::asm_utils {

// ===========================================================================
// Быстрый поиск разделителей слов
// ===========================================================================

#if defined(__x86_64__) && !defined(PUNTO_NO_ASM)
#include <immintrin.h>

/**
 * @brief Поиск веса биграммы с использованием SSE (ASM/Intrinsic)
 */
[[nodiscard]] inline std::uint8_t
sse_find_bigram(const punto::BigramEntry *table, std::size_t table_size,
                char first, char second) noexcept {

  // Упаковываем искомые символы в 32-битное целое (первые 2 байта)
  uint32_t target = (static_cast<uint32_t>(static_cast<uint8_t>(first))) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(second)) << 8);
  __m128i target_vec = _mm_set1_epi32(static_cast<int>(target));

  // Маска для сравнения только первых двух байт (first и second)
  __m128i mask = _mm_set1_epi32(0x0000FFFF);
  target_vec = _mm_and_si128(target_vec, mask);

  for (std::size_t i = 0; i < table_size; i += 4) {
    // Загружаем 4 биграммы за раз (16 байт)
    __m128i data =
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(table + i));

    // Сравниваем только первые 2 байта каждой биграммы
    __m128i masked_data = _mm_and_si128(data, mask);
    __m128i cmp = _mm_cmpeq_epi32(masked_data, target_vec);

    int movemask = _mm_movemask_epi8(cmp);
    if (movemask != 0) {
      // Нашли совпадение в одной из 4-х биграмм
      for (std::size_t j = 0; j < 4 && (i + j) < table_size; ++j) {
        if (table[i + j].first == first && table[i + j].second == second) {
          return table[i + j].weight;
        }
      }
    }
  }
  return 0;
}

/**
 * @brief Чтение системного счетчика тактов процессора (RDTSC)
 */
[[nodiscard]] inline uint64_t get_cpu_timestamp() noexcept {
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return (static_cast<uint64_t>(high) << 32) | static_cast<uint64_t>(low);
}

/**
 * @brief Быстрое обнуление буфера (REP STOSQ версия)
 * @param buffer Указатель на буфер
 * @param count Количество 64-битных слов
 */
inline void fast_zero_buffer(void *buffer, std::size_t count) noexcept {
  if (count == 0 || buffer == nullptr)
    return;

  __asm__ volatile("xor %%rax, %%rax\n\t"
                   "rep stosq"
                   : "+D"(buffer), "+c"(count)
                   :
                   : "rax", "memory");
}

#else

// Fallback для non-x86_64
[[nodiscard]] inline std::size_t
find_word_delimiter_asm(const std::uint16_t *buffer, std::size_t len,
                        std::uint16_t space_code,
                        std::uint16_t tab_code) noexcept {

  for (std::size_t i = 0; i < len; ++i) {
    if (buffer[i] == space_code || buffer[i] == tab_code) {
      return i;
    }
  }
  return len;
}

inline void fast_zero_buffer(void *buffer, std::size_t count) noexcept {
  std::memset(buffer, 0, count * sizeof(std::uint64_t));
}

#endif

// ===========================================================================
// Высокоуровневые обёртки
// ===========================================================================

/**
 * @brief Находит первый разделитель слова в буфере KeyEntry
 * @param entries Буфер KeyEntry
 * @return Индекс первого разделителя или размер буфера
 */
[[nodiscard]] inline std::size_t
find_word_end(std::span<const KeyEntry> entries) noexcept {

  // Извлекаем скан-коды для оптимизированного поиска
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].code == KEY_SPACE || entries[i].code == KEY_TAB) {
      return i;
    }
  }
  return entries.size();
}

// ===========================================================================
// Prefetch hints для оптимизации кэша
// ===========================================================================

/// Prefetch данные для чтения (L1 кэш)
inline void prefetch_read(const void *addr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, 0, 3);
#endif
}

/// Prefetch данные для записи (L1 кэш)
inline void prefetch_write(const void *addr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, 1, 3);
#endif
}

} // namespace punto::asm_utils
