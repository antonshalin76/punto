/**
 * @file event_loop.hpp
 * @brief Главный цикл обработки событий ввода
 *
 * Оптимизированный event loop для чтения input_event из stdin.
 * Управляет состоянием модификаторов и диспетчеризует события.
 */

#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "punto/clipboard_manager.hpp"
#include "punto/ipc_server.hpp"
#include "punto/config.hpp"
#include "punto/dictionary.hpp"
#include "punto/input_buffer.hpp"
#include "punto/key_injector.hpp"
#include "punto/layout_analyzer.hpp"
#include "punto/types.hpp"
#include "punto/x11_session.hpp"

namespace punto {

class SoundManager;

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

  /// Обрабатывает входящее событие
  void handle_event(const input_event &ev);

  /// Обновляет состояние модификаторов
  void update_modifier_state(ScanCode code, bool pressed);

  /// Определяет действие по горячей клавише
  [[nodiscard]] HotkeyAction determine_hotkey_action(ScanCode code) const;

  // =========================================================================
  // Действия
  // =========================================================================

  /// Инвертирует раскладку последнего слова (ручной вызов по Pause)
  void action_invert_layout_word();

  /// Автоматическая инверсия текущего слова при нажатии пробела
  /// @param word_to_invert Слово для инверсии (span)
  /// @param space_code Код пробела/таба для ввода после инверсии
  void action_auto_invert_word(std::span<const KeyEntry> word,
                               ScanCode space_code);

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

  /// Переключает раскладку
  void switch_layout();

  /// Перепечатывает слово после инверсии
  void retype_word_inverted(std::span<const KeyEntry> word,
                            std::span<const ScanCode> trailing);

  /// Обрабатывает selection (копирование, трансформация, вставка)
  bool
  process_selection(std::function<std::string(std::string_view)> transform);

  /// Ожидает указанное время, буферизуя входящие события
  void wait_and_buffer(std::chrono::microseconds us);

  /// Обрабатывает все накопленные события
  void drain_pending_events();

  // =========================================================================
  // Состояние
  // =========================================================================

  Config config_;
  ModifierState modifiers_;
  InputBuffer buffer_;
  LayoutAnalyzer analyzer_;
  Dictionary dict_;

  std::unique_ptr<KeyInjector> injector_;
  std::unique_ptr<X11Session> x11_session_;
  std::unique_ptr<ClipboardManager> clipboard_;
  std::unique_ptr<SoundManager> sound_manager_;

  bool initialized_ = false;

  /// Текущая раскладка: 0 = EN (первая), 1 = RU (вторая)
  /// Обновляется при переключении раскладки
  int current_layout_ = 0;

  /// Очередь событий, накопленных во время выполнения макроса коррекции
  std::deque<input_event> pending_events_;

  /// Флаг выполнения макроса (автокоррекции)
  bool is_processing_macro_ = false;

  /// Время последнего замера раскладки
  std::chrono::steady_clock::time_point last_sync_time_;

  // =========================================================================
  // IPC управление
  // =========================================================================

  /// Атомарный флаг включения/выключения автопереключения
  std::atomic<bool> ipc_enabled_{true};

  /// IPC сервер для управления из tray-приложения
  std::unique_ptr<IpcServer> ipc_server_;

  /// Перезагружает конфигурацию (вызывается из IPC потока)
  bool reload_config();
};

} // namespace punto
