/**
 * @file asm_utils.hpp
 * @brief Ассемблерные оптимизации для горячих путей
 *
 * Inline ASM для x86_64 для критичных по производительности операций.
 * Fallback на C++ реализации для других архитектур.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "punto/types.hpp"

namespace punto::asm_utils {

// ===========================================================================
// Быстрый поиск разделителей слов
// ===========================================================================

#if defined(__x86_64__) && !defined(PUNTO_NO_ASM)

/**
 * @brief Поиск разделителя слова в буфере скан-кодов (ASM версия)
 * @param buffer Буфер скан-кодов
 * @param len Длина буфера
 * @param space_code Скан-код пробела (KEY_SPACE)
 * @param tab_code Скан-код таба (KEY_TAB)
 * @return Индекс первого разделителя, или len если не найден
 *
 * Использует SSE2 для параллельного сравнения.
 */
[[nodiscard]] inline std::size_t
find_word_delimiter_asm(const std::uint16_t *buffer, std::size_t len,
                        std::uint16_t space_code,
                        std::uint16_t tab_code) noexcept {

  if (len == 0)
    return 0;

  std::size_t result = len;

  // Fallback для небольших буферов или невыровненных данных
  // Полная SSE2 оптимизация может быть добавлена в Phase 3
  for (std::size_t i = 0; i < len; ++i) {
    if (buffer[i] == space_code || buffer[i] == tab_code) {
      result = i;
      break;
    }
  }

  return result;
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
  // (В Phase 3 можно оптимизировать через переупаковку данных)
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
