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
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "punto/analysis_worker_pool.hpp"
#include "punto/config.hpp"
#include "punto/control_plane_state.hpp"
#include "punto/dictionary.hpp"
#include "punto/input_buffer.hpp"
#include "punto/ipc_server.hpp"
#include "punto/layout_analyzer.hpp"
#include "punto/runtime_health.hpp"
#include "punto/runtime_tuning.hpp"
#include "punto/types.hpp"
#include "punto/x11_session.hpp"
#include "punto/word_editor.hpp"

namespace punto {

class SoundManager;
class UndoDetector;

namespace event_loop_detail {
[[nodiscard]] std::size_t count_running_punto_daemons(
    const std::filesystem::path &proc_root = "/proc",
    std::size_t max_numeric_candidates = 4096,
    std::chrono::milliseconds time_budget = std::chrono::milliseconds{20});
} // namespace event_loop_detail

/**
 * @brief Главный класс приложения
 *
 * Управляет циклом обработки событий, буферизацией ввода и
 * выполнением действий по горячим клавишам.
 */
class EventLoop {
public:
  using ConfigLoaderFunction = std::function<ConfigLoadOutcome(
      const std::filesystem::path &,
      const std::optional<std::filesystem::path> &, const std::string &)>;
  using DictionaryLoaderFunction = std::function<DictionaryLoadOutcome()>;

  /**
   * @brief Конструктор
   * @param config Конфигурация приложения
   * @param x11_probe Необязательный источник сессии для изолированных тестов
   */
  explicit EventLoop(Config config, X11Session::ProbeFunction x11_probe = {},
                     ConfigLoaderFunction config_loader = {},
                     DictionaryLoaderFunction dictionary_loader = {});

  ~EventLoop();

  // Запрет копирования
  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;

  /**
   * @brief Инициализирует runtime-компоненты
   * @return true если инициализация успешна
   */
  bool initialize();

  /**
   * @brief Запрашивает остановку цикла обработки событий
   *
   * Thread-safe. Для signal handler используется self-pipe в main.cpp.
   */
  void request_stop() noexcept;

  /// Передаёт read-end self-pipe для корректной обработки SIGINT/SIGTERM.
  void set_stop_signal_fd(int fd) noexcept;

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

  /// Writes an input event to stdout, including partial-write handling.
  void emit_passthrough_event(const input_event &ev);

  /// Обновляет состояние модификаторов
  void update_modifier_state(ScanCode code, bool pressed);

  /// Проверяет готовые результаты анализа и (при необходимости) применяет
  /// коррекции
  void process_ready_results();
  void queue_manual_word_edit(HotkeyAction action);
  void queue_auto_word_edit(const WordResult &result);
  void finish_word_candidate(
      const X11Session::KeyboardObservation *observation = nullptr);
  void process_word_observation(bool input_idle = false);
  void process_pending_word_edit();
  bool wait_and_buffer(std::chrono::steady_clock::time_point deadline);
  void drain_pending_events();
  void clear_word_history();
  void finalize_queued_words();
  [[nodiscard]] HotkeyAction determine_hotkey_action() const;

  /// Сбрасывает async state и выставляет новый barrier для task_id.
  void reset_async_state(bool bump_task_barrier = true,
                         bool preserve_completed_selection = false);

  void note_input_event_accepted(const input_event &event);
  void note_input_event_committed(const input_event &event);
  void fail_input_pipeline() noexcept;
  void refresh_analysis_health_head();
  void commit_analysis_terminal(std::uint64_t task_id);
  void maybe_promote_to_control_plane_primary();
  [[nodiscard]] bool reconcile_control_plane_before_promotion();
  void sync_control_plane_from_shared_state(bool force);
  ControlPlanePublicationResult
  publish_control_plane_state(bool bump_config_generation,
                              bool bump_status_generation, const Config &config,
                              bool auto_enabled);
  [[nodiscard]] bool start_primary_ipc_server();
  void service_ipc_commands() noexcept;
  void cancel_ipc_commands_for_shutdown() noexcept;
  [[nodiscard]] IpcResult execute_ipc_command(const IpcRequest &request);
  void observe_ipc_fatal() noexcept;
  [[nodiscard]] bool start_config_loader() noexcept;
  void poll_config_load_completion();
  void request_x11_config_reload();
  void retry_pending_x11_config_reload();
  [[nodiscard]] bool
  stop_config_loader(std::chrono::milliseconds timeout) noexcept;
  [[nodiscard]] bool start_dictionary_loader() noexcept;
  void poll_dictionary_load_completion();
  [[nodiscard]] bool
  stop_dictionary_loader(std::chrono::milliseconds timeout) noexcept;
  void shutdown_runtime() noexcept;

  [[nodiscard]] IpcResult stats_report() const;

  struct ConfigLoadTask {
    std::uint64_t generation = 0;
    std::uint64_t status_generation_at_admission = 0;
    std::filesystem::path system_root;
    std::optional<std::filesystem::path> user_root;
    std::optional<X11SessionInfo> session_authority;
    std::optional<std::uint64_t> control_plane_generation;
    std::optional<std::uint64_t> x11_config_generation;
    std::string requested_path;
    bool promotion_reconciliation = false;
  };

  struct ConfigLoadCompletion {
    ConfigLoadTask task;
    ConfigLoadOutcome outcome;
    bool used_promotion_fallback = false;
  };

  struct ConfigLoaderState {
    std::mutex mutex;
    std::condition_variable condition;
    ConfigLoaderFunction loader;
    std::optional<ConfigLoadTask> request;
    std::optional<ConfigLoadCompletion> completion;
    bool stop_requested = false;
    bool exited = false;
  };

  enum class ConfigLoadStatus { None, Ok, Error };

  struct DictionaryLoaderState {
    std::mutex mutex;
    std::condition_variable condition;
    DictionaryLoaderFunction loader;
    std::optional<DictionaryLoadOutcome> completion;
    bool stop_requested = false;
    bool exited = false;
  };

  // =========================================================================
  // Состояние
  // =========================================================================

  // Config snapshots are published on the event-loop thread.
  std::shared_ptr<const Config> config_;
  bool runtime_auto_enabled_ = true;
  bool runtime_status_established_ = false;

  ModifierState modifiers_;
  std::bitset<KEY_CNT> held_keys_;
  InputBuffer buffer_;
  struct RawWordCandidate {
    enum class Kind { Automatic, ManualLayout, ManualCase, SelectionLayout,
                      SelectionCase, SelectionTranslit, Undo, Tail };
    Kind kind;
    std::uint64_t request_id;
    std::vector<KeyEntry> word;
    std::string trailing;
    std::size_t analysis_len;
    int diagnostic_layout;
    std::shared_ptr<const Config> config;
    bool observing = false;
    std::uint64_t word_id = 0;
    std::optional<std::string> visible = std::nullopt;
    std::uint64_t input_sequence = 0;
    bool analyze = true;
    bool allow_correction = true;
  };
  std::optional<RawWordCandidate> raw_word_candidate_;
  std::deque<RawWordCandidate> queued_word_candidates_;
  std::uint64_t next_word_observation_id_ = 0;
  struct TrackedWord {
    std::uint64_t id;
    std::vector<KeyEntry> word;
    std::string trailing;
    std::optional<std::string> visible = std::nullopt;
    std::optional<std::uint64_t> task_id = std::nullopt;
    std::optional<std::string> correction = std::nullopt;
    int target_layout = -1;
    int source_layout = -1;
    std::uint64_t session_generation = 0;
    std::uint32_t focus_window = 0;
    bool eligible = true;
    bool allow_terminal = true;
  };
  std::deque<TrackedWord> word_history_;
  std::uint64_t next_word_id_ = 0;
  std::optional<std::string> active_word_visible_;
  bool active_word_manually_edited_ = false;
  std::optional<X11Session::KeyboardObservation> keyboard_observation_;
  std::optional<WordEditRequest> pending_word_edit_;
  bool pending_is_undo_ = false;
  std::optional<WordEditRequest> undo_request_;
  std::chrono::steady_clock::time_point undo_applied_at_{};
  std::uint64_t user_input_sequence_ = 0;
  std::uint64_t undo_input_sequence_ = 0;
  bool swallow_z_until_release_ = false;
  std::unique_ptr<WordEditor> word_editor_;
  std::unique_ptr<SoundManager> sound_manager_;
  std::unique_ptr<UndoDetector> undo_detector_;
  std::deque<input_event> pending_events_;
  bool macro_active_ = false;
  bool macro_input_eof_ = false;
  std::chrono::steady_clock::time_point last_key_event_at_{};
  std::uint64_t word_dispatches_ = 0;
  bool pause_down_ = false;

  // Read-only analysis pipeline.
  std::shared_ptr<const Dictionary> dictionary_;
  std::unique_ptr<AnalysisWorkerPool> analysis_pool_;
  bool analysis_pool_failed_ = false;

  std::uint64_t next_task_id_ = 0;
  std::uint64_t next_apply_task_id_ = 0;

  std::unordered_map<std::uint64_t, WordResult> ready_results_;
  std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
      analysis_accepted_at_;
  StallHealthPolicy analysis_health_;

  struct Telemetry {
    std::chrono::steady_clock::time_point last_report_at{};

    std::uint64_t analyzed_words = 0;
    std::uint64_t need_switch_words = 0;

    std::uint64_t analysis_us_sum = 0;
    std::uint64_t analysis_us_max = 0;

    std::uint64_t queue_us_sum = 0;
    std::uint64_t queue_us_max = 0;

  } telemetry_;

  struct LifetimeTelemetry {
    std::atomic<std::uint64_t> analyzed_words{0};
    std::atomic<std::uint64_t> need_switch_words{0};
    std::atomic<std::uint64_t> analysis_us_sum{0};
    std::atomic<std::uint64_t> queue_us_sum{0};
    std::atomic<std::size_t> ready_results{0};
  } lifetime_telemetry_;

  std::unique_ptr<X11Session> x11_session_;
  ExplicitHealthPolicy x11_health_{ComponentHealth::Degraded};
  bool x11_refresh_pending_{false}; // Флаг фонового refresh
  bool x11_dependencies_ready_{false};
  bool wayland_warning_emitted_{false};

  std::shared_ptr<ConfigLoaderState> config_loader_state_;
  std::thread config_loader_thread_;
  bool config_load_pending_ = false;
  std::uint64_t config_load_generation_ = 0;
  ConfigLoadStatus config_load_status_ = ConfigLoadStatus::None;
  std::uint64_t x11_config_generation_ = 0;
  std::optional<std::uint64_t> pending_x11_config_generation_;

  std::shared_ptr<DictionaryLoaderState> dictionary_loader_state_;
  std::thread dictionary_loader_thread_;
  bool dictionary_load_pending_ = false;

  bool initialized_ = false;
  AnalysisThreadBudget analysis_thread_budget_{};

  /// Последняя наблюдавшаяся раскладка: 0 = EN (первая), 1 = RU (вторая).
  /// До первой успешной X11 snapshot-публикации используется 0.
  int current_layout_ = 0;
  int stop_signal_fd_ = -1;
  int exit_code_ = 0;

  // stdin is a byte stream: one input_event may arrive across several reads.
  std::array<std::uint8_t, sizeof(input_event)> input_frame_bytes_{};
  std::size_t input_frame_size_ = 0;

  // Logical evdev frames are accepted at the first event and committed at the
  // matching SYN_REPORT after all output/intentional-consumption work finishes.
  std::deque<std::chrono::steady_clock::time_point> input_frame_accepts_;
  bool input_read_frame_started_ = false;
  StallHealthPolicy input_health_;

  // =========================================================================
  // IPC управление
  // =========================================================================

  /// Атомарный флаг запроса остановки (от signal handler)
  std::atomic<bool> stop_requested_{false};

  /// Bounded SPSC handoff: socket poller produces, EventLoop consumes.
  /// Declaration order keeps the mailbox alive until after IpcServer teardown.
  std::shared_ptr<IpcCommandMailbox> ipc_mailbox_{
      std::make_shared<IpcCommandMailbox>()};

  /// IPC сервер для управления из tray-приложения
  std::unique_ptr<IpcServer> ipc_server_;
  bool ipc_server_is_fallback_ = false;
  bool ipc_fatal_reported_ = false;
  bool runtime_shutdown_started_ = false;
  ControlPlaneLease control_plane_lease_{};

  /// Роль читается из IPC-потока (stats/reload), пишется main-потоком при
  /// старте/промоушене — поэтому атомик.
  std::atomic<bool> control_plane_primary_{false};

  /// Защищает shared_control_plane_state_ и applied_*_generation_:
  /// publish_control_plane_state() вызывается и из main-потока (initialize,
  /// failover, X11 refresh -> reload), и из IPC-потока (RELOAD/SET_STATUS).
  std::mutex control_plane_mutex_;
  SharedControlPlaneState shared_control_plane_state_{};
  std::uint64_t applied_config_generation_ = 0;
  std::uint64_t applied_status_generation_ = 0;
  std::optional<std::uint64_t> promotion_fallback_applied_generation_;
  std::chrono::steady_clock::time_point last_control_plane_poll_{};

  /// Schedules a generation-fenced background configuration load.
  IpcResult reload_config(
      const std::string &config_path = {},
      std::optional<std::uint64_t> control_plane_generation = std::nullopt,
      std::optional<std::uint64_t> x11_config_generation = std::nullopt,
      bool promotion_reconciliation = false);
};

} // namespace punto
