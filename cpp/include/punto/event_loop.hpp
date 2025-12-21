/**
 * @file event_loop.hpp
 * @brief Главный цикл обработки событий ввода
 *
 * Оптимизированный event loop для чтения input_event из stdin.
 * Управляет состоянием модификаторов и диспетчеризует события.
 */

#pragma once

#include <functional>
#include <memory>

#include "punto/clipboard_manager.hpp"
#include "punto/config.hpp"
#include "punto/input_buffer.hpp"
#include "punto/key_injector.hpp"
#include "punto/types.hpp"
#include "punto/x11_session.hpp"

namespace punto {

/**
 * @brief Главный класс приложения
 *
 * Управляет циклом обработки событий, буферизацией ввода и
 * выполнением действий по горячим клавишам.
 */
class EventLoop {
public:
  /**
   * @brief Конструктор
   * @param config Конфигурация приложения
   */
  explicit EventLoop(Config config);

  ~EventLoop();

  // Запрет копирования
  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;

  /**
   * @brief Инициализирует компоненты (X11 сессия, буфер обмена)
   * @return true если инициализация успешна
   */
  bool initialize();

  /**
   * @brief Запускает главный цикл
   * @return Код возврата (0 = успех)
   *
   * Блокирующий вызов — читает stdin до EOF или ошибки.
   */
  [[nodiscard]] int run();

private:
  // =========================================================================
  // Обработчики событий
  // =========================================================================

  /// Обрабатывает событие клавиши
  void handle_key_event(const input_event &ev);

  /// Обновляет состояние модификаторов
  void update_modifier_state(ScanCode code, bool pressed);

  /// Определяет действие по горячей клавише
  [[nodiscard]] HotkeyAction determine_hotkey_action(ScanCode code) const;

  // =========================================================================
  // Действия
  // =========================================================================

  /// Инвертирует раскладку последнего слова
  void action_invert_layout_word();

  /// Инвертирует раскладку выделенного текста
  void action_invert_layout_selection();

  /// Инвертирует регистр последнего слова
  void action_invert_case_word();

  /// Инвертирует регистр выделенного текста
  void action_invert_case_selection();

  /// Транслитерирует выделенный текст
  void action_transliterate_selection();

  // =========================================================================
  // Вспомогательные методы
  // =========================================================================

  /// Переключает раскладку через hotkey
  void switch_layout();

  /// Перепечатывает слово после инверсии
  void retype_word_inverted(std::span<const KeyEntry> word,
                            std::span<const ScanCode> trailing);

  /// Обрабатывает selection (копирование, трансформация, вставка)
  bool
  process_selection(std::function<std::string(std::string_view)> transform);

  // =========================================================================
  // Состояние
  // =========================================================================

  Config config_;
  ModifierState modifiers_;
  InputBuffer buffer_;

  std::unique_ptr<KeyInjector> injector_;
  std::unique_ptr<X11Session> x11_session_;
  std::unique_ptr<ClipboardManager> clipboard_;

  bool initialized_ = false;
};

} // namespace punto
