/**
 * @file key_injector.hpp
 * @brief Генератор событий ввода
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <span>

#include "punto/types.hpp"

namespace punto {

/**
 * @brief Класс для эмуляции ввода через uinput
 *
 * Использует stdout для записи событий в пайп uinput.
 */
class KeyInjector {
public:
  /// Конструктор (задержки внутри зашиты; config.delays удалён)
  KeyInjector() noexcept;

  // =========================================================================
  // Низкоуровневые операции
  // =========================================================================

  /**
   * @brief Записывает событие в stdout
   * @param ev Событие для записи
   */
  static void emit_event(const input_event &ev);

  /**
   * @brief Записывает пачку событий в stdout одним системным вызовом
   * @param events Пачка событий
   */
  static void emit_events(std::span<const input_event> events);

  /**
   * @brief Отправляет событие нажатия/отпускания и SYN
   * @param code Скан-код
   * @param state Состояние
   */
  void send_key(ScanCode code, KeyState state) const;

  /// Тип функции ожидания (для интеграции с Input Guard)
  using WaitFunc = std::function<void(std::chrono::microseconds)>;

  /**
   * @brief Устанавливает функцию ожидания
   * @param func Функция, которая будет вызываться вместо usleep
   */
  void set_wait_func(WaitFunc func) noexcept { wait_func_ = std::move(func); }

  /**
   * @brief Отправляет нажатие клавиши (Press + Delay + Release + Delay)
   * @param code Скан-код клавиши
   * @param with_shift Нужно ли зажимать Shift
   * @param turbo Использовать ли ускоренные задержки
   */
  void tap_key(ScanCode code, bool with_shift = false,
               bool turbo = false) const;

  // =========================================================================
  // Высокоуровневые макросы
  // =========================================================================

  /**
   * @brief Отправляет N бекспейсов
   * @param count Количество
   * @param turbo Использовать ли ускоренные задержки
   */
  void send_backspace(std::size_t count, bool turbo = false) const;

  /**
   * @brief Перепечатывает буфер символов
   * @param entries Буфер KeyEntry
   * @param turbo Использовать ли ускоренные задержки
   */
  void retype_buffer(std::span<const KeyEntry> entries,
                     bool turbo = false) const;

  /**
   * @brief Перепечатывает хвост слова (пробелы и т.д.)
   * @param codes Список скан-кодов
   * @param turbo Использовать ли ускоренные задержки
   */
  void retype_trailing(std::span<const ScanCode> codes,
                       bool turbo = false) const;

  /**
   * @brief Отправляет hotkey для переключения раскладки
   * @param modifier Модификатор
   * @param key Клавиша
   */
  void send_layout_hotkey(ScanCode modifier, ScanCode key) const;

  /**
   * @brief Вставка текста из буфера обмена
   *
   * - is_terminal=true: Ctrl+Shift+V
   * - is_terminal=false: Shift+Insert
   */
  void send_paste(bool is_terminal) const;

  /**
   * @brief Отпускает все модификаторы
   */
  void release_all_modifiers() const;

  /// Задержка (использует wait_func_ если установлена, иначе usleep)
  void delay(std::chrono::microseconds us) const noexcept;

private:
  static void write_all_or_die(int fd, const void *data, std::size_t bytes);

  // Встроенные задержки (в микросекундах).
  // Значения соответствуют прежним рекомендуемым параметрам из config.yaml.
  static constexpr std::chrono::microseconds kKeyPress{12000};
  static constexpr std::chrono::microseconds kLayoutSwitch{150000};
  static constexpr std::chrono::microseconds kRetype{15000};
  static constexpr std::chrono::microseconds kTurboKeyPress{15000};
  static constexpr std::chrono::microseconds kTurboRetype{35000};
  static constexpr std::chrono::microseconds kKeyHold{20000};
  static constexpr std::chrono::microseconds kModifierHold{15000};
  static constexpr std::chrono::microseconds kModifierRelease{8000};
  static constexpr std::chrono::microseconds kBackspaceHold{18000};

  WaitFunc wait_func_;
};

} // namespace punto
