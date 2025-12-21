/**
 * @file key_injector.hpp
 * @brief Генерация событий ввода для stdout
 *
 * Замена xdotool — генерирует struct input_event напрямую в stdout,
 * который читается interception-tools и передаётся в uinput.
 */

#pragma once

#include <linux/input.h>

#include <chrono>
#include <cstdint>
#include <span>

#include "punto/config.hpp"
#include "punto/types.hpp"

namespace punto {

/**
 * @brief Генератор событий ввода для interception-tools пайплайна
 *
 * Записывает структуры input_event в stdout, которые percolate
 * обратно в ядро через uinput. Это zero-overhead замена xdotool.
 */
class KeyInjector {
public:
  explicit KeyInjector(const DelayConfig &delays) noexcept;

  // =========================================================================
  // Базовые операции
  // =========================================================================

  /**
   * @brief Отправляет одиночное событие клавиши
   * @param code Скан-код клавиши
   * @param state Состояние (нажатие/отпускание)
   */
  void send_key(ScanCode code, KeyState state) const;

  /**
   * @brief Отправляет нажатие и отпускание клавиши
   * @param code Скан-код клавиши
   * @param with_shift Нажать с Shift
   */
  void tap_key(ScanCode code, bool with_shift = false) const;

  /**
   * @brief Отправляет несколько Backspace
   * @param count Количество Backspace
   */
  void send_backspace(std::size_t count) const;

  // =========================================================================
  // Высокоуровневые операции
  // =========================================================================

  /**
   * @brief Перепечатывает буфер с сохранением регистра
   * @param entries Буфер KeyEntry
   */
  void retype_buffer(std::span<const KeyEntry> entries) const;

  /**
   * @brief Перепечатывает trailing whitespace (пробелы/табы)
   * @param codes Скан-коды пробелов/табов
   */
  void retype_trailing(std::span<const ScanCode> codes) const;

  /**
   * @brief Отправляет горячую клавишу переключения раскладки
   * @param modifier Модификатор (Ctrl, Alt, etc.)
   * @param key Основная клавиша
   */
  void send_layout_hotkey(ScanCode modifier, ScanCode key) const;

  /**
   * @brief Освобождает все зажатые модификаторы
   */
  void release_all_modifiers() const;

  // =========================================================================
  // Низкоуровневые операции
  // =========================================================================

  /**
   * @brief Пропускает событие без изменений (passthrough)
   * @param ev Оригинальное событие
   */
  static void emit_event(const input_event &ev);

private:
  /**
   * @brief Отправляет EV_SYN для синхронизации
   */
  static void send_sync();

  /**
   * @brief Задержка между событиями
   */
  void delay(std::chrono::microseconds us) const noexcept;

  DelayConfig delays_;
};

} // namespace punto
