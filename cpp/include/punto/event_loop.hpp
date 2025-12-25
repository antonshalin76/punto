/**
 * @file event_loop.hpp
 * @brief Главный цикл обработки событий ввода
 *
 * Оптимизированный event loop для чтения input_event из stdin.
 * Управляет состоянием модификаторов и диспетчеризует события.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "punto/analysis_worker_pool.hpp"
#include "punto/clipboard_manager.hpp"
#include "punto/config.hpp"
#include "punto/dictionary.hpp"
#include "punto/history_manager.hpp"
#include "punto/input_buffer.hpp"
#include "punto/ipc_server.hpp"
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
   * @brief Запрашивает остановку цикла обработки событий
   *
   * Thread-safe. Может вызываться из signal handler.
   */
  void request_stop() noexcept;

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

  /// Passthrough в stdout с трекингом состояния клавиш (key down/up).
  void emit_passthrough_event(const input_event &ev);

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

  /// Переключает раскладку через системный hotkey (toggle).
  /// Используется как fallback, если XKB недоступен.
  void switch_layout(bool play_sound);

  /// Устанавливает раскладку (0/1) через XKB (XkbLockGroup) с fallback на
  /// hotkey.
  /// @param target_layout 0 = EN, 1 = RU
  /// @param play_sound Проигрывать ли звук (только для финального переключения)
  /// @return true если удалось применить переключение
  [[nodiscard]] bool set_layout(int target_layout, bool play_sound);

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

  /// Во время макроса мы буферизуем ввод. Если макрос стартует очень быстро,
  /// key-release (в т.ч. для SPACE/последней буквы) может оказаться в
  /// pending_events_, и тогда инжектируемые нажатия этой же клавиши могут быть
  /// проигнорированы, потому что для приложения клавиша всё ещё "зажата".
  ///
  /// Этот метод "пропускает" наружу только те фреймы (до SYN_REPORT),
  /// которые НЕ содержат EV_KEY press/repeat (т.е. только release и служебные
  /// события). Фреймы с press/repeat остаются в очереди и будут обработаны
  /// после макроса.
  void flush_pending_release_frames();

  /// Проверяет готовые результаты анализа и (при необходимости) применяет
  /// коррекции
  void process_ready_results();

  struct PendingWordMeta {
    std::uint64_t task_id = 0;
    std::vector<KeyEntry> word;
    std::size_t analysis_len = 0;
    int layout_at_boundary = 0;
    std::uint64_t start_pos = 0;
    std::uint64_t end_pos = 0; // конец слова (перед разделителем)

    std::chrono::steady_clock::time_point boundary_at{};
  };

  /// Применяет коррекцию раскладки (v2.6 логика)
  void apply_correction(const PendingWordMeta &meta, int target_layout);

  /// Применяет коррекцию только регистра БЕЗ смены раскладки (sticky shift fix)
  /// Например: ПРивет -> Привет в той же раскладке
  void apply_case_correction(const PendingWordMeta &meta,
                             const std::vector<KeyEntry> &corrected_word);

  /// Применяет комбинированную коррекцию: смена раскладки + исправление
  /// регистра Например: GHbdtn -> Привет (EN -> RU + case fix)
  void apply_combined_correction(const PendingWordMeta &meta, int target_layout,
                                 const std::vector<KeyEntry> &corrected_word);

  // =========================================================================
  // Состояние
  // =========================================================================

  // Конфиг и зависящие от него компоненты обновляются через снапшоты,
  // т.к. reload_config() вызывается из IPC-потока.
  std::shared_ptr<const Config> config_;
  std::shared_ptr<const LayoutAnalyzer> analyzer_;
  std::shared_ptr<const KeyInjector> injector_;

  ModifierState modifiers_;
  InputBuffer buffer_;
  Dictionary dict_;

  // Async pipeline: история + пул анализа
  HistoryManager history_{5};
  AnalysisWorkerPool analysis_pool_{dict_};

  std::uint64_t next_task_id_ = 0;
  std::uint64_t next_apply_task_id_ = 0;

  std::unordered_map<std::uint64_t, PendingWordMeta> pending_words_;
  std::unordered_map<std::uint64_t, WordResult> ready_results_;

  std::vector<KeyEntry> tail_scratch_;

  struct Telemetry {
    std::chrono::steady_clock::time_point last_report_at{};

    std::uint64_t analyzed_words = 0;
    std::uint64_t need_switch_words = 0;

    std::uint64_t analysis_us_sum = 0;
    std::uint64_t analysis_us_max = 0;

    std::uint64_t queue_us_sum = 0;
    std::uint64_t queue_us_max = 0;

    std::uint64_t corrections = 0;
    std::uint64_t correction_us_sum = 0;
    std::uint64_t correction_us_max = 0;

    std::uint64_t tail_len_sum = 0;
    std::uint64_t tail_len_max = 0;
  } telemetry_;

  // Если прямое XKB-переключение не работает в текущем окружении,
  // отключаем его и используем только hotkey-метод.
  bool xkb_set_available_ = true;

  std::unique_ptr<X11Session> x11_session_;
  std::unique_ptr<ClipboardManager> clipboard_;
  std::unique_ptr<SoundManager> sound_manager_;

  bool initialized_ = false;

  /// Текущая раскладка: 0 = EN (первая), 1 = RU (вторая)
  /// Обновляется при переключении раскладки
  int current_layout_ = 0;

  /// Очередь событий, накопленных во время выполнения макроса коррекции
  std::deque<input_event> pending_events_;

  // Best-effort трекер «какие клавиши сейчас зажаты» с точки зрения приложения.
  // Нужен, чтобы безопасно форвардить release-события во время макросов,
  // даже если они пришли в одном SYN-фрейме вместе с press других клавиш.
  std::array<std::uint8_t, KEY_CNT> key_down_{};

  /// Флаг выполнения макроса (автокоррекции)
  bool is_processing_macro_ = false;

  /// Время последнего замера раскладки
  std::chrono::steady_clock::time_point last_sync_time_;

  // =========================================================================
  // IPC управление
  // =========================================================================

  /// Атомарный флаг включения/выключения автопереключения
  std::atomic<bool> ipc_enabled_{true};

  /// Атомарный флаг запроса остановки (от signal handler)
  std::atomic<bool> stop_requested_{false};

  /// IPC сервер для управления из tray-приложения
  std::unique_ptr<IpcServer> ipc_server_;

  /// Перезагружает конфигурацию (вызывается из IPC потока)
  /// Если config_path не пуст, пытается загрузить именно этот файл.
  IpcResult reload_config(const std::string &config_path = {});
};

} // namespace punto
