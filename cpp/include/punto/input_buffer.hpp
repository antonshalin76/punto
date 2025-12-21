/**
 * @file input_buffer.hpp
 * @brief Кольцевой буфер для хранения введённых символов
 *
 * Оптимизированная структура данных для трекинга последнего слова.
 * Поддерживает сохранение состояния регистра для каждого символа.
 */

#pragma once

#include <cstddef>
#include <span>

#include "punto/types.hpp"

namespace punto {

/**
 * @brief Буфер для хранения текущего и последнего слова
 *
 * Хранит скан-коды нажатых клавиш вместе с информацией о регистре (Shift).
 * При нажатии пробела/таба текущее слово перемещается в "последнее слово",
 * что позволяет инвертировать его при повторном нажатии Pause.
 */
class InputBuffer {
public:
  InputBuffer() = default;

  // Запрет копирования (для явного управления состоянием)
  InputBuffer(const InputBuffer &) = delete;
  InputBuffer &operator=(const InputBuffer &) = delete;
  InputBuffer(InputBuffer &&) = default;
  InputBuffer &operator=(InputBuffer &&) = default;

  // =========================================================================
  // Операции с текущим словом
  // =========================================================================

  /**
   * @brief Добавляет символ в буфер текущего слова
   * @param code Скан-код клавиши
   * @param shifted Состояние Shift при нажатии
   * @return true если символ добавлен, false если буфер полон
   */
  bool push_char(ScanCode code, bool shifted) noexcept;

  /**
   * @brief Удаляет последний символ (Backspace)
   * @return true если символ удалён, false если буфер пуст
   */
  bool pop_char() noexcept;

  /**
   * @brief Фиксирует текущее слово как "последнее" и очищает текущий буфер
   *
   * Вызывается при нажатии пробела, таба или успешной инверсии.
   */
  void commit_word() noexcept;

  /**
   * @brief Полный сброс обоих буферов (при Enter или навигации)
   */
  void reset_all() noexcept;

  /**
   * @brief Сброс только текущего слова (при модификаторах + клавиша)
   */
  void reset_current() noexcept;

  // =========================================================================
  // Trailing whitespace
  // =========================================================================

  /**
   * @brief Добавляет trailing пробел или таб
   */
  bool push_trailing(ScanCode code) noexcept;

  /**
   * @brief Сбрасывает буфер trailing whitespace
   */
  void reset_trailing() noexcept;

  // =========================================================================
  // Доступ к данным
  // =========================================================================

  /// Возвращает view на текущее слово (или последнее, если текущее пустое)
  [[nodiscard]] std::span<const KeyEntry> get_active_word() const noexcept;

  /// Возвращает view на текущее слово
  [[nodiscard]] std::span<const KeyEntry> current_word() const noexcept;

  /// Возвращает view на последнее слово
  [[nodiscard]] std::span<const KeyEntry> last_word() const noexcept;

  /// Возвращает view на trailing whitespace
  [[nodiscard]] std::span<const ScanCode> trailing() const noexcept;

  /// Длина текущего слова
  [[nodiscard]] std::size_t current_length() const noexcept;

  /// Длина последнего слова
  [[nodiscard]] std::size_t last_length() const noexcept;

  /// Длина trailing whitespace
  [[nodiscard]] std::size_t trailing_length() const noexcept;

  /// Проверка наличия данных для инверсии
  [[nodiscard]] bool has_data() const noexcept;

private:
  WordBuffer current_buf_{};
  WordBuffer last_buf_{};
  TrailingBuffer trailing_buf_{};

  std::size_t current_len_ = 0;
  std::size_t last_len_ = 0;
  std::size_t trailing_len_ = 0;
};

} // namespace punto
