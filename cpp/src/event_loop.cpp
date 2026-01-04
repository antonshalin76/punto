/**
 * @file event_loop.cpp
 * @brief Реализация главного цикла обработки событий
 */

#include "punto/event_loop.hpp"
#include "punto/key_entry_text.hpp"
#include "punto/scancode_map.hpp"
#include "punto/sound_manager.hpp"
#include "punto/text_processor.hpp"

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace punto {

namespace {

struct OneshotPasteWaits {
  std::chrono::microseconds pre_paste;
  std::chrono::microseconds post_paste;
  std::chrono::microseconds after_backspace;
};

[[nodiscard]] constexpr OneshotPasteWaits oneshot_paste_waits(
    bool is_terminal) noexcept {
  if (is_terminal) {
    return {
        /*pre_paste=*/std::chrono::microseconds{150000},
        /*post_paste=*/std::chrono::microseconds{250000},
        /*after_backspace=*/std::chrono::microseconds{60000},
    };
  }

  return {
      /*pre_paste=*/std::chrono::microseconds{100000},
      // В некоторых приложениях (особенно IDE) обработка paste может быть
      // асинхронной. Даем больше времени, чтобы не восстановить CLIPBOARD слишком
      // рано.
      /*post_paste=*/std::chrono::microseconds{250000},
      /*after_backspace=*/std::chrono::microseconds{0},
  };
}

} // namespace

EventLoop::EventLoop(Config config)
    : config_{std::make_shared<Config>(std::move(config))},
      analyzer_{std::make_shared<LayoutAnalyzer>(config_->auto_switch)} {
  ipc_enabled_.store(config_->auto_switch.enabled, std::memory_order_relaxed);
  history_.set_max_words(config_->auto_switch.max_rollback_words);
}

EventLoop::~EventLoop() = default;

bool EventLoop::initialize() {
  if (initialized_) {
    return true;
  }

  // Создаём минимальный KeyInjector из текущего снапшота конфига.
  // Важно: reload_config() может заменить его на новый.
  {
    auto injector = std::make_shared<KeyInjector>();
    injector->set_wait_func(
        [this](std::chrono::microseconds us) { this->wait_and_buffer(us); });

    std::shared_ptr<const KeyInjector> injector_const = injector;
    std::atomic_store(&injector_, std::move(injector_const));
  }

  // Инициализируем словарь для гибридного анализа
  dict_.initialize();

  // Инициализируем X11 сессию
  x11_session_ = std::make_unique<X11Session>();
  bool x11_ok = x11_session_->initialize();

  // После X11 инициализации можем вычислить реальный $HOME активного
  // пользователя и перечитать ~/.config/... (если существует).
  {
    IpcResult res = reload_config();
    if (!res.success) {
      std::cerr << "[punto] Warning: initial config reload failed: "
                << res.message << "\n";
    }
  }

  // Применяем настройки истории (max_rollback_words) и поднимаем пул анализа.
  {
    auto cfg = std::atomic_load(&config_);
    history_.set_max_words(cfg->auto_switch.max_rollback_words);

    std::size_t hc = std::thread::hardware_concurrency();
    std::size_t threads = 1;
    if (hc > 1) {
      threads = hc - 1;
    }

    analysis_pool_.start(threads);
    std::cerr << "[punto] Analysis pool: " << threads << " threads\n";
  }

  // SoundManager зависит от X11Session (uid/gid/env активного пользователя)
  {
    auto cfg = std::atomic_load(&config_);
    sound_manager_ = std::make_unique<SoundManager>(*x11_session_, cfg->sound);
  }

  if (!x11_ok) {
    std::cerr
        << "[punto] Предупреждение: X11 сессия не инициализирована (нет активной "
           "user-сессии или недоступен DISPLAY/XAUTHORITY).\n"
        << "[punto] Ожидается на экране логина: сервис автоматически "
           "перепривяжется после входа пользователя.\n";
    // Не фатальная ошибка — базовая функциональность работает
  } else {
    // Создаём ClipboardManager
    clipboard_ = std::make_unique<ClipboardManager>(*x11_session_);

    // Определяем текущую раскладку
    x11_session_->apply_environment();
    current_layout_ = x11_session_->get_current_keyboard_layout();
    if (current_layout_ < 0) {
      current_layout_ = 0; // По умолчанию английская
    }
    std::cerr << "[punto] Текущая раскладка: "
              << (current_layout_ == 0 ? "EN" : "RU") << "\n";
  }

  // Создаём IPC сервер для управления из tray-приложения
  ipc_server_ = std::make_unique<IpcServer>(
      ipc_enabled_,
      [this](const std::string &path) { return reload_config(path); });
  if (!ipc_server_->start()) {
    std::cerr << "[punto] Warning: IPC server failed to start. "
                 "Tray control will be unavailable.\n";
    // Не фатальная ошибка — базовая функциональность работает
  }

  initialized_ = true;
  return true;
}

void EventLoop::request_stop() noexcept {
  stop_requested_.store(true, std::memory_order_relaxed);
}

int EventLoop::run() {
  if (!initialize()) {
    std::cerr << "[punto] Failed to initialize event loop\n";
    return 1;
  }

  // Пытаемся синхронизировать раскладку на старте
  x11_session_->apply_environment();
  current_layout_ = x11_session_->get_current_keyboard_layout();
  if (current_layout_ < 0) {
    current_layout_ = 0;
  }
  std::cerr << "[punto] Startup layout group: " << current_layout_ << "\n";
  last_sync_time_ = std::chrono::steady_clock::now();

  // Полностью отключаем буферизацию stdin для корректной работы poll() +
  // fread()
  std::setvbuf(stdin, nullptr, _IONBF, 0);
  std::setbuf(stdout, nullptr);

  input_event ev{};

  pollfd pfd{STDIN_FILENO, POLLIN, 0};

  // Время последней проверки/обновления X11-сессии.
  // Нужен для фикса "прилипание к GDM" на буте и корректной работы после logout/login.
  auto last_x11_check_time = std::chrono::steady_clock::now();
  constexpr auto kX11CheckInterval = std::chrono::seconds{3};

  bool x11_wait_log_emitted = false;

  auto rebuild_x11_deps = [&]() {
    // Смена GUI-сессии может менять источник конфигурации (~/.config/...)
    // поэтому всегда делаем best-effort reload.
    {
      IpcResult res = reload_config();
      if (!res.success) {
        std::cerr << "[punto] Warning: config reload after X11 refresh failed: "
                  << res.message << "\n";
      }
    }

    // На новой сессии пробуем снова включить прямой XKB set.
    xkb_set_available_ = true;

    {
      const X11SessionInfo info = x11_session_->info();
      std::cerr << "[punto] X11 session: id=" << info.session_id
                << " user=" << info.username << " display=" << info.display
                << "\n";
    }

    // Пересоздаём Clipboard/Sound (они завязаны на DISPLAY/XDG_RUNTIME_DIR/uid).
    clipboard_ = std::make_unique<ClipboardManager>(*x11_session_);

    auto cfg = std::atomic_load(&config_);
    sound_manager_ = std::make_unique<SoundManager>(*x11_session_, cfg->sound);

    // Синхронизируем раскладку.
    x11_session_->apply_environment();
    current_layout_ = x11_session_->get_current_keyboard_layout();
    if (current_layout_ < 0) {
      current_layout_ = 0;
    }
    std::cerr << "[punto] X11 session refreshed, layout: "
              << (current_layout_ == 0 ? "EN" : "RU") << "\n";

    last_sync_time_ = std::chrono::steady_clock::now();
  };

  auto teardown_x11_deps = [&]() {
    clipboard_.reset();
    sound_manager_.reset();
    // Не трогаем current_layout_: оно используется как внутренний стейт,
    // но без валидной X11-сессии операции set/get всё равно станут no-op.
  };

  // Главный цикл: проверяем флаг остановки на каждой итерации
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    // Обслуживаем X11 selection ownership (clipboard/primary).
    if (clipboard_) {
      clipboard_->pump_events();
    }

    // Периодически проверяем, не сменилась ли активная user-сессия.
    {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_x11_check_time >= kX11CheckInterval) {
        last_x11_check_time = now;

        const X11Session::RefreshResult rr = x11_session_->refresh();
        if (rr == X11Session::RefreshResult::Updated) {
          rebuild_x11_deps();
        } else if (rr == X11Session::RefreshResult::Invalidated) {
          teardown_x11_deps();
          std::cerr << "[punto] X11 session invalidated (no active user session)\n";
        }
      }
    }

    // Одноразовый лог "ждём user-сессию" (чтобы не спамить в journalctl).
    // На экране логина активна greeter-сессия (Class=greeter) и X11 контекст
    // пользователя ещё недоступен.
    if (!x11_session_->is_valid()) {
      if (!x11_wait_log_emitted) {
        x11_wait_log_emitted = true;
        std::cerr << "[punto] X11: активная пользовательская сессия не обнаружена "
                     "(возможно экран логина). Ожидаю входа пользователя...\n";
      }
    } else {
      x11_wait_log_emitted = false;
    }

    // Важно: даже если пользователь перестал печатать, мы должны
    // применять готовые результаты анализа (иначе автоинверсия может не
    // сработать).
    process_ready_results();

    pfd.revents = 0;
    int ret = poll(&pfd, 1, 1); // 1ms тик

    if (ret > 0) {
      // Проверяем флаги закрытия канала или ошибки.
      // POLLHUP возникает когда udevmon закрывает пайп (остановка сервиса).
      // Если установлен POLLIN вместе с POLLHUP, сначала читаем оставшиеся
      // данные.
      if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        // Если данных нет (только HUP/ERR), выходим из цикла
        if (!(pfd.revents & POLLIN)) {
          std::cerr << "[punto] stdin closed (revents=0x" << std::hex
                    << pfd.revents << std::dec << "), exiting gracefully\n";
          break;
        }
        // Иначе сначала читаем оставшиеся данные, а выйдем на следующей
        // итерации
      }

      if (pfd.revents & POLLIN) {
        if (std::fread(&ev, sizeof(ev), 1, stdin) != 1) {
          break; // EOF или ошибка
        }
        handle_event(ev);
        // После обработки события ещё раз проверим результаты.
        process_ready_results();
        continue;
      }
    }

    if (ret == 0) {
      continue; // timeout
    }

    if (ret < 0) {
      if (errno == EINTR) {
        // Сигнал прервал poll — проверяем флаг остановки на следующей итерации
        continue;
      }
      break;
    }
  }

  std::cerr << "[punto] Event loop terminated gracefully\n";
  return 0;
}

void EventLoop::emit_passthrough_event(const input_event &ev) {
  if (ev.type == EV_KEY) {
    const ScanCode code = ev.code;
    if (code < key_down_.size()) {
      key_down_[code] = (ev.value != 0) ? 1U : 0U;
    }
  }

  KeyInjector::emit_event(ev);
}

void EventLoop::handle_event(const input_event &ev) {
  // If we are processing a macro, buffer ALL events (including EV_SYN, EV_MSC)
  if (is_processing_macro_) {
    constexpr std::size_t kPendingEventsCap = 5000;
    if (pending_events_.size() < kPendingEventsCap) {
      pending_events_.push_back(ev);
    } else {
      // Fail-fast на уровне логов: потеря событий ввода недопустима,
      // но лучше деградировать, чем зависнуть.
      static bool warned = false;
      if (!warned) {
        warned = true;
        std::cerr << "[punto] Input Guard: pending_events overflow cap="
                  << kPendingEventsCap << " (dropping input events)\n";
      }
    }
    return;
  }

  if (ev.type != EV_KEY) {
    emit_passthrough_event(ev);
    return;
  }

  const ScanCode code = ev.code;
  const bool pressed = ev.value != 0;
  const bool is_press = ev.value == 1; // Только первое нажатие, не repeat

  // Если мы перехватили Ctrl+Z и не отдали press наружу, то все последующие
  // repeat/release для Z нужно проглотить (иначе приложение увидит release без press).
  if (code == KEY_Z && swallow_z_until_release_) {
    if (!pressed) {
      swallow_z_until_release_ = false;
    }
    return;
  }

  auto cfg = std::atomic_load(&config_);

  // Применяем изменения max_rollback_words безопасно (только в main thread).
  if (history_.max_words() != cfg->auto_switch.max_rollback_words) {
    history_.set_max_words(cfg->auto_switch.max_rollback_words);
    pending_words_.clear();
    ready_results_.clear();
    next_apply_task_id_ = next_task_id_;
  }

  // =========================================================================
  // Обновление состояния модификаторов
  // =========================================================================

  if (is_modifier(code)) {
    update_modifier_state(code, pressed);
    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Обрабатываем только нажатия (не отпускания и не repeat)
  // =========================================================================

  if (!is_press) {
    emit_passthrough_event(ev);
    return;
  }

  // Ctrl+Z: Undo последнего исправления. Перехватываем только если:
  //  - есть валидная запись последней коррекции;
  //  - после неё не было другого ввода;
  //  - прошло мало времени.
  // Иначе Ctrl+Z пропускается в приложение как обычный системный хоткей.
  if (code == KEY_Z && modifiers_.any_ctrl() && !modifiers_.any_alt() &&
      !modifiers_.any_meta()) {
    if (action_undo_last_correction()) {
      swallow_z_until_release_ = true;
      return;
    }
  }

  // Любой key-press (кроме модификаторов) считаем "разрывом" для undo.
  // Ctrl+Z обработан выше и сюда не попадает.
  ++user_seq_;
  last_undo_.reset();

  // =========================================================================
  // Backspace
  // =========================================================================

  if (code == KEY_BACKSPACE) {
    buffer_.pop_char();
    (void)history_.pop_token();

    // Детектируем отмену коррекции (быстрые Backspace сразу после auto-fix)
    if (undo_detector_.on_backspace(std::chrono::steady_clock::now())) {
      std::cerr << "[punto] Undo detected! Word added to session exclusions\n";
    }

    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Горячие клавиши (PAUSE + модификаторы)
  // =========================================================================

  if (code == KEY_PAUSE) {
    // Любой макрос по хоткею делает "переписывание" текста.
    // Чтобы избежать гонок/рассинхронизации, сбрасываем async-состояние.
    pending_words_.clear();
    ready_results_.clear();
    next_apply_task_id_ = next_task_id_;
    history_.reset();

    auto action = determine_hotkey_action(code);

    switch (action) {
    case HotkeyAction::TranslitSelection:
      action_transliterate_selection();
      return;
    case HotkeyAction::InvertLayoutSelection:
      action_invert_layout_selection();
      return;
    case HotkeyAction::InvertCaseSelection:
      action_invert_case_selection();
      return;
    case HotkeyAction::InvertCaseWord:
      action_invert_case_word();
      return;
    case HotkeyAction::InvertLayoutWord:
      action_invert_layout_word();
      return;
    case HotkeyAction::NoAction:
      break;
    }
    return;
  }

  // =========================================================================
  // Bypass для системных hotkeys (Ctrl+C, etc.)
  // =========================================================================

  if (modifiers_.any_ctrl() || modifiers_.any_alt() || modifiers_.any_meta()) {
    // Системные хоткеи/комбинации потенциально меняют курсор/контекст.
    // Для надёжности сбрасываем async-состояние.
    pending_words_.clear();
    ready_results_.clear();
    next_apply_task_id_ = next_task_id_;
    history_.reset();

    // Детектируем пользовательское переключение раскладки (Ctrl+`)
    // Это важно для синхронизации current_layout_
    ScanCode mod_key = cfg->hotkey.modifier;
    ScanCode layout_key = cfg->hotkey.key;

    bool mod_pressed = false;
    if (mod_key == KEY_LEFTCTRL)
      mod_pressed = modifiers_.left_ctrl;
    else if (mod_key == KEY_RIGHTCTRL)
      mod_pressed = modifiers_.right_ctrl;
    else if (mod_key == KEY_LEFTALT)
      mod_pressed = modifiers_.left_alt;
    else if (mod_key == KEY_RIGHTALT)
      mod_pressed = modifiers_.right_alt;
    else if (mod_key == KEY_LEFTSHIFT)
      mod_pressed = modifiers_.left_shift;
    else if (mod_key == KEY_RIGHTSHIFT)
      mod_pressed = modifiers_.right_shift;

    if (mod_pressed && code == layout_key) {
      // Пользователь переключает раскладку вручную!
      current_layout_ = (current_layout_ == 0) ? 1 : 0;
      std::cerr << "[punto] USER layout switch -> "
                << (current_layout_ == 0 ? "EN" : "RU") << "\n";

      if (sound_manager_) {
        sound_manager_->play_for_layout(current_layout_);
      }
    }

    buffer_.reset_current();
    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Разделители слов (пробел, таб)
  // =========================================================================

  if (code == KEY_SPACE || code == KEY_TAB) {
    auto full_word = buffer_.current_word();

    // 1. Синхронизируем раскладку (на каждом разделителе!)
    if (!is_processing_macro_) {
      x11_session_->apply_environment();
      int os_layout = x11_session_->get_current_keyboard_layout();
      if (os_layout != -1 && os_layout != current_layout_) {
        std::cerr << "[punto] Layout SYNC: " << current_layout_ << " -> "
                  << os_layout << " (from "
                  << (code == KEY_SPACE ? "space" : "tab") << ")\n";
        current_layout_ = os_layout;
        last_sync_time_ = std::chrono::steady_clock::now();
      }
    }

    // 2. Очищаем слово от пунктуации для анализа
    std::span<const KeyEntry> analysis_word = full_word;
    while (!analysis_word.empty()) {
      ScanCode last = analysis_word.back().code;
      if (last == KEY_DOT || last == KEY_COMMA || last == KEY_SEMICOLON ||
          last == KEY_APOSTROPHE || last == KEY_SLASH || last == KEY_MINUS) {
        analysis_word = analysis_word.subspan(0, analysis_word.size() - 1);
      } else {
        break;
      }
    }

    // Сначала эмулируем ввод разделителя (чтобы ввод не тормозил).
    history_.push_token(KeyEntry{code, false});
    buffer_.commit_word();
    buffer_.push_trailing(code);
    emit_passthrough_event(ev);

    // Пустое слово (двойной пробел/таб) не анализируем.
    if (full_word.empty()) {
      return;
    }

    // Каждому слову — свой task_id для строгого порядка применения.
    const std::uint64_t task_id = next_task_id_++;

    // Проверяем, включено ли автопереключение через IPC.
    const bool ipc_enabled = ipc_enabled_.load(std::memory_order_relaxed);

    if (!ipc_enabled || analysis_word.size() < cfg->auto_switch.min_word_len) {
      WordResult res;
      res.task_id = task_id;
      res.need_switch = false;
      res.word_len = full_word.size();
      res.analysis_len = analysis_word.size();
      res.layout_at_boundary = current_layout_;
      ready_results_[task_id] = res;
      return;
    }

    PendingWordMeta meta;
    meta.task_id = task_id;
    meta.word.assign(full_word.begin(), full_word.end());
    meta.analysis_len = analysis_word.size();
    meta.layout_at_boundary = current_layout_;
    meta.boundary_at = std::chrono::steady_clock::now();

    // Координаты: delimiter уже в истории.
    const std::uint64_t delim_pos = history_.cursor_pos() - 1;
    meta.end_pos = delim_pos;

    if (meta.end_pos < meta.word.size()) {
      // Не должно происходить, но fail-fast: не ставим задачу, чтобы не сломать
      // sequencer.
      WordResult res;
      res.task_id = task_id;
      res.need_switch = false;
      res.word_len = meta.word.size();
      res.analysis_len = meta.analysis_len;
      res.layout_at_boundary = meta.layout_at_boundary;
      ready_results_[task_id] = res;
      return;
    }

    meta.start_pos =
        meta.end_pos - static_cast<std::uint64_t>(meta.word.size());

    pending_words_[task_id] = meta;

    WordTask task;
    task.task_id = task_id;
    task.word = meta.word;
    task.analysis_len = meta.analysis_len;
    task.layout_at_boundary = meta.layout_at_boundary;
    task.cfg = cfg->auto_switch;
    task.submitted_at = std::chrono::steady_clock::now();

    analysis_pool_.submit(std::move(task));

    // Ограничиваем память: храним метаданные только для последних N слов.
    const std::uint64_t max_words =
        static_cast<std::uint64_t>(cfg->auto_switch.max_rollback_words);
    if (max_words > 0 && task_id + 1 > max_words) {
      const std::uint64_t min_keep = (task_id + 1) - max_words;
      for (auto it = pending_words_.begin(); it != pending_words_.end();) {
        if (it->first < min_keep) {
          it = pending_words_.erase(it);
        } else {
          ++it;
        }
      }
    }

    return;
  }

  if (code == KEY_DOT || code == KEY_COMMA || code == KEY_SEMICOLON ||
      code == KEY_APOSTROPHE || code == KEY_SLASH || code == KEY_MINUS) {

    // Синхронизируем раскладку (только синхронизация, без анализа)
    if (!is_processing_macro_) {
      int os_layout = x11_session_->get_current_keyboard_layout();
      if (os_layout != -1 && os_layout != current_layout_) {
        std::cerr << "[punto] Layout SYNC: " << current_layout_ << " -> "
                  << os_layout << " (from punct)\n";
        current_layout_ = os_layout;
      }
    }

    buffer_.push_char(code, modifiers_.any_shift());
    history_.push_token(KeyEntry{code, modifiers_.any_shift()});
    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Enter — полный сброс
  // =========================================================================

  if (code == KEY_ENTER || code == KEY_KPENTER) {
    buffer_.reset_all();
    history_.reset();
    pending_words_.clear();
    ready_results_.clear();
    next_apply_task_id_ = next_task_id_;

    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Буквенные клавиши
  // =========================================================================

  if (is_letter_key(code)) {
    buffer_.push_char(code, modifiers_.any_shift());
    history_.push_token(KeyEntry{code, modifiers_.any_shift()});

    // Сбрасываем счётчик backspace при наборе буквы
    undo_detector_.on_key_typed();

    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Навигационные клавиши — полный сброс буфера
  // =========================================================================

  if (is_navigation_key(code)) {
    buffer_.reset_all();
    history_.reset();
    pending_words_.clear();
    ready_results_.clear();
    next_apply_task_id_ = next_task_id_;

    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Функциональные клавиши — пропускаем без сброса
  // =========================================================================

  if (is_function_key(code)) {
    emit_passthrough_event(ev);
    return;
  }

  // =========================================================================
  // Неизвестные клавиши — сброс текущего слова
  // =========================================================================

  buffer_.reset_current();
  // Неизвестная клавиша = потенциальный разрыв контекста.
  history_.reset();
  pending_words_.clear();
  ready_results_.clear();
  next_apply_task_id_ = next_task_id_;

  emit_passthrough_event(ev);
}

void EventLoop::update_modifier_state(ScanCode code, bool pressed) {
  switch (code) {
  case KEY_LEFTSHIFT:
    modifiers_.left_shift = pressed;
    break;
  case KEY_RIGHTSHIFT:
    modifiers_.right_shift = pressed;
    break;
  case KEY_LEFTCTRL:
    modifiers_.left_ctrl = pressed;
    break;
  case KEY_RIGHTCTRL:
    modifiers_.right_ctrl = pressed;
    break;
  case KEY_LEFTALT:
    modifiers_.left_alt = pressed;
    break;
  case KEY_RIGHTALT:
    modifiers_.right_alt = pressed;
    break;
  case KEY_LEFTMETA:
    modifiers_.left_meta = pressed;
    break;
  case KEY_RIGHTMETA:
    modifiers_.right_meta = pressed;
    break;
  default:
    break;
  }
}

HotkeyAction EventLoop::determine_hotkey_action(ScanCode code) const {
  if (code != KEY_PAUSE) {
    return HotkeyAction::NoAction;
  }

  // LCtrl + LAlt + Pause = Transliterate
  if (modifiers_.left_ctrl && modifiers_.left_alt) {
    return HotkeyAction::TranslitSelection;
  }

  // Shift + Pause = Invert Layout Selection
  if (modifiers_.any_shift()) {
    return HotkeyAction::InvertLayoutSelection;
  }

  // Alt + Pause = Invert Case Selection
  if (modifiers_.any_alt()) {
    return HotkeyAction::InvertCaseSelection;
  }

  // Ctrl + Pause = Invert Case Word
  if (modifiers_.any_ctrl()) {
    return HotkeyAction::InvertCaseWord;
  }

  // Pause only = Invert Layout Word
  return HotkeyAction::InvertLayoutWord;
}

void EventLoop::switch_layout(bool play_sound) {
  // Инвертируем внутреннее состояние
  current_layout_ = (current_layout_ == 0) ? 1 : 0;
  last_sync_time_ = std::chrono::steady_clock::now();

  if (play_sound && sound_manager_) {
    sound_manager_->play_for_layout(current_layout_);
  }

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return;
  }

  // Эмулируем нажатие горячей клавиши (fallback).
  injector->send_layout_hotkey(cfg->hotkey.modifier, cfg->hotkey.key);
}

bool EventLoop::set_layout(int target_layout, bool play_sound) {
  if (target_layout != 0 && target_layout != 1) {
    return false;
  }

  // Всегда стараемся синхронизировать внутреннее состояние с ОС перед решением.
  if (x11_session_ && x11_session_->is_valid()) {
    x11_session_->apply_environment();
    int os_layout = x11_session_->get_current_keyboard_layout();
    if (os_layout == 0 || os_layout == 1) {
      if (os_layout != current_layout_) {
        std::cerr << "[punto] Layout SYNC(set): " << current_layout_ << " -> "
                  << os_layout << "\n";
        current_layout_ = os_layout;
        last_sync_time_ = std::chrono::steady_clock::now();
      }
    }
  }

  if (current_layout_ == target_layout) {
    return true;
  }

  // Быстрый путь: XKB LockGroup (но только если реально работает).
  if (xkb_set_available_ && x11_session_ && x11_session_->is_valid()) {
    x11_session_->apply_environment();

    bool ok = x11_session_->set_keyboard_layout(target_layout);

    // Важно: set_keyboard_layout() в некоторых окружениях может возвращать
    // успех, но фактически раскладка не меняется. Проверяем состоянием XKB.
    int os_layout = x11_session_->get_current_keyboard_layout();
    if (ok && os_layout == target_layout) {
      current_layout_ = target_layout;
      last_sync_time_ = std::chrono::steady_clock::now();

      if (play_sound && sound_manager_) {
        sound_manager_->play_for_layout(current_layout_);
      }

      return true;
    }

    std::cerr << "[punto] Layout SET via XKB did not apply (ok=" << ok
              << " os_layout=" << os_layout << "); disable XKB set\n";
    xkb_set_available_ = false;
  }

  // Fallback: hotkey toggle (работает только если 2 раскладки).
  if ((current_layout_ == 0 || current_layout_ == 1) &&
      target_layout == ((current_layout_ == 0) ? 1 : 0)) {

    switch_layout(play_sound);

    // Best-effort: сверяемся с ОС после хоткея.
    // Важно: сразу после переключения XKB state может обновиться с задержкой,
    // поэтому не «откатываем» внутреннее состояние назад, если ОС ещё не
    // успела.
    if (x11_session_ && x11_session_->is_valid()) {
      x11_session_->apply_environment();
      int os_layout = x11_session_->get_current_keyboard_layout();
      if (os_layout == 0 || os_layout == 1) {
        if (os_layout == target_layout) {
          current_layout_ = os_layout;
          last_sync_time_ = std::chrono::steady_clock::now();
        } else {
          std::cerr << "[punto] Layout SYNC(hotkey): os_layout=" << os_layout
                    << " expected=" << target_layout << " (may be delayed)\n";
        }
      }
    }

    return current_layout_ == target_layout;
  }

  return false;
}

std::optional<int> EventLoop::maybe_switch_layout_to_en_for_terminal_paste(
    bool is_terminal) {
  if (!is_terminal) {
    return std::nullopt;
  }

  // 0 = EN, 1 = RU. По умолчанию используем текущий внутренний стейт,
  // но если есть валидная X11-сессия — уточняем у ОС.
  int before = current_layout_;

  if (x11_session_ && x11_session_->is_valid()) {
    x11_session_->apply_environment();
    const int os_layout = x11_session_->get_current_keyboard_layout();
    if (os_layout == 0 || os_layout == 1) {
      before = os_layout;
    }
  }

  if (before == 0) {
    return std::nullopt;
  }
  if (before != 1) {
    // Неизвестное значение — не меняем раскладку.
    return std::nullopt;
  }

  // В терминалах Ctrl+Shift+V может быть чувствителен к раскладке,
  // поэтому временно ставим EN.
  (void)set_layout(/*target_layout=*/0, /*play_sound=*/false);

  return before; // restore to RU
}

bool EventLoop::paste_text_oneshot(std::string_view text,
                                  bool restore_clipboard) {
  if (!clipboard_) {
    std::cerr << "[punto] Clipboard: недоступен (oneshot paste skipped)\n";
    return false;
  }

  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return false;
  }

  const bool is_terminal = clipboard_->is_active_window_terminal();

  // Пытаемся сохранить CLIPBOARD (best-effort), чтобы не ломать пользовательский
  // буфер обмена.
  std::optional<std::string> prev_clip;
  if (restore_clipboard) {
    prev_clip = clipboard_->get_text(Selection::Clipboard);
    if (!prev_clip.has_value()) {
      std::cerr << "[punto] Clipboard: cannot read CLIPBOARD for restore "
                   "(oneshot paste skipped)\n";
      return false;
    }
  }

  // Для Shift+Insert нам нужен PRIMARY, но для Ctrl+Shift+V достаточно CLIPBOARD.
  std::optional<std::string> prev_primary;
  if (!is_terminal) {
    prev_primary = clipboard_->get_text(Selection::Primary);
  }

  const ClipboardResult set_clip =
      clipboard_->set_text(Selection::Clipboard, text);
  if (set_clip != ClipboardResult::Ok) {
    std::cerr << "[punto] Clipboard: failed to set CLIPBOARD (res="
              << static_cast<int>(set_clip) << ")\n";
    return false;
  }

  if (!is_terminal) {
    const ClipboardResult set_primary =
        clipboard_->set_text(Selection::Primary, text);
    if (set_primary != ClipboardResult::Ok) {
      std::cerr << "[punto] Clipboard: failed to set PRIMARY (res="
                << static_cast<int>(set_primary) << ")\n";
      return false;
    }
  }

  // Даём X11 шанс обновить CLIPBOARD, иначе приложение может прочитать
  // старое значение.
  //
  // В терминалах (и в целом в «медленных» UI-циклах) запрос буфера обмена может
  // приходить с задержкой.
  const OneshotPasteWaits waits = oneshot_paste_waits(is_terminal);

  wait_and_buffer(waits.pre_paste);

  injector->release_all_modifiers();

  // Для Ctrl+Shift+V в некоторых терминалах нужен EN layout.
  const std::optional<int> restore_layout =
      maybe_switch_layout_to_en_for_terminal_paste(is_terminal);

  injector->send_paste(is_terminal);

  // Даем приложению время запросить содержимое clipboard.
  wait_and_buffer(waits.post_paste);

  if (restore_layout.has_value()) {
    (void)set_layout(*restore_layout, /*play_sound=*/false);
  }

  // Восстанавливаем PRIMARY только если трогали его.
  if (!is_terminal) {
    if (prev_primary.has_value()) {
      (void)clipboard_->set_text(Selection::Primary, *prev_primary);
    } else {
      // Если PRIMARY не удаётся прочитать (нет selection owner / таймаут),
      // лучше не оставлять в PRIMARY подстановочный текст.
      (void)clipboard_->set_text(Selection::Primary, "");
    }
  }

  if (restore_clipboard && prev_clip.has_value()) {
    (void)clipboard_->set_text(Selection::Clipboard, *prev_clip);
  }

  return true;
}

bool EventLoop::replace_text_oneshot(std::size_t backspace_count,
                                    std::string_view text,
                                    std::optional<int> final_layout,
                                    bool play_sound) {
  if (!clipboard_) {
    std::cerr << "[punto] Clipboard: недоступен (oneshot replace skipped)\n";
    return false;
  }

  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return false;
  }

  const bool is_terminal = clipboard_->is_active_window_terminal();

  // Важно: стараемся НЕ удалять текст, пока не убедились, что можем выставить
  // буфер для вставки.
  std::optional<std::string> prev_clip = clipboard_->get_text(Selection::Clipboard);
  if (!prev_clip.has_value()) {
    std::cerr << "[punto] Clipboard: cannot read CLIPBOARD for restore "
                 "(oneshot replace skipped)\n";
    return false;
  }

  // PRIMARY нужен только для Shift+Insert.
  std::optional<std::string> prev_primary;
  if (!is_terminal) {
    prev_primary = clipboard_->get_text(Selection::Primary);
  }

  const ClipboardResult set_clip =
      clipboard_->set_text(Selection::Clipboard, text);
  if (set_clip != ClipboardResult::Ok) {
    std::cerr << "[punto] Clipboard: failed to set CLIPBOARD (res="
              << static_cast<int>(set_clip) << ")\n";
    return false;
  }

  if (!is_terminal) {
    const ClipboardResult set_primary =
        clipboard_->set_text(Selection::Primary, text);
    if (set_primary != ClipboardResult::Ok) {
      std::cerr << "[punto] Clipboard: failed to set PRIMARY (res="
                << static_cast<int>(set_primary) << ")\n";
      return false;
    }
  }

  // Даём X11 шанс обновить CLIPBOARD.
  //
  // В терминалах часто есть дополнительная задержка между hotkey paste и
  // фактическим запросом clipboard.
  const OneshotPasteWaits waits = oneshot_paste_waits(is_terminal);

  wait_and_buffer(waits.pre_paste);

  // Важно: replace вызывается только из main thread в режиме macro.
  injector->release_all_modifiers();

  // Даем физическим key-release шанс прийти в stdin.
  wait_and_buffer(std::chrono::microseconds{30000});
  flush_pending_release_frames();

  // Удаляем то, что заменяем.
  if (backspace_count > 0) {
    injector->send_backspace(backspace_count, true);
    flush_pending_release_frames();

    // КРИТИЧЕСКАЯ ПАУЗА: терминалы/TTY UI иногда применяют Backspace не мгновенно.
    // Без паузы Paste может прийти до фактического удаления символов.
    if (waits.after_backspace.count() > 0) {
      wait_and_buffer(waits.after_backspace);
    }
  }

  // Для Ctrl+Shift+V в некоторых терминалах нужен EN layout.
  const std::optional<int> restore_layout =
      maybe_switch_layout_to_en_for_terminal_paste(is_terminal);

  injector->send_paste(is_terminal);

  // Даем приложению время запросить содержимое clipboard.
  wait_and_buffer(waits.post_paste);

  if (prev_clip.has_value()) {
    (void)clipboard_->set_text(Selection::Clipboard, *prev_clip);
  }

  // Восстанавливаем PRIMARY только если трогали его.
  if (!is_terminal) {
    if (prev_primary.has_value()) {
      (void)clipboard_->set_text(Selection::Primary, *prev_primary);
    } else {
      (void)clipboard_->set_text(Selection::Primary, "");
    }
  }

  if (final_layout.has_value()) {
    // Best-effort: если не получилось — текст всё равно уже вставлен.
    (void)set_layout(*final_layout, play_sound);
  } else if (restore_layout.has_value()) {
    (void)set_layout(*restore_layout, /*play_sound=*/false);
  }

  return true;
}

void EventLoop::set_last_undo_record(std::string original_text,
                                   std::string_view inserted_text,
                                   std::optional<int> restore_layout,
                                   bool is_auto_correction) {
  UndoRecord rec;
  rec.original_text = std::move(original_text);
  rec.inserted_len = utf8_codepoint_count(inserted_text);
  rec.restore_layout = restore_layout;
  rec.is_auto_correction = is_auto_correction;
  rec.applied_at = std::chrono::steady_clock::now();
  rec.user_seq_at_apply = user_seq_;

  last_undo_ = std::move(rec);
}

bool EventLoop::action_undo_last_correction() {
  if (!last_undo_.has_value()) {
    return false;
  }

  // Перехватываем Ctrl+Z только в узком окне времени.
  // Иначе это должен быть обычный undo приложения.
  static constexpr auto kUndoWindow = std::chrono::milliseconds{2500};

  const auto now = std::chrono::steady_clock::now();
  if (last_undo_->applied_at.time_since_epoch().count() == 0 ||
      now - last_undo_->applied_at > kUndoWindow) {
    last_undo_.reset();
    return false;
  }

  // Если после исправления был любой другой key-press — не перехватываем.
  if (last_undo_->user_seq_at_apply != user_seq_) {
    last_undo_.reset();
    return false;
  }

  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    last_undo_.reset();
    return false;
  }

  // Переносим запись в локальную переменную (и очищаем state) до выполнения
  // макроса — чтобы не переиспользовать её в случае реэнтранси.
  UndoRecord rec = std::move(*last_undo_);
  last_undo_.reset();

  if (rec.inserted_len == 0 && !rec.original_text.empty()) {
    // Некорректное состояние: нечего удалять, но есть что вставлять.
    // Лучше не делать ничего, чем повредить текст.
    return false;
  }

  // Undo — это макрос переписывания текста; сбрасываем async-состояние.
  pending_words_.clear();
  ready_results_.clear();
  next_apply_task_id_ = next_task_id_;
  history_.reset();
  buffer_.reset_all();

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  std::cerr << "[punto] Undo: start (erase=" << rec.inserted_len
            << " restore_layout="
            << (rec.restore_layout.has_value() ? std::to_string(*rec.restore_layout)
                                               : std::string{"-"})
            << ")\n";

  const bool ok = replace_text_oneshot(
      rec.inserted_len, rec.original_text,
      /*final_layout=*/rec.restore_layout,
      /*play_sound=*/false);
  if (!ok) {
    std::cerr << "[punto] Undo: oneshot replace failed (skip)\n";
    return false;
  }

  if (rec.is_auto_correction) {
    // Ctrl+Z как явный сигнал "отменяю автокоррекцию" → пишем слово в exclusions.
    undo_detector_.on_undo();
  }

  std::cerr << "[punto] Undo: done\n";
  return true;
}

void EventLoop::action_invert_layout_word() {
  auto word = buffer_.get_active_word();
  if (word.empty()) {
    return;
  }

  const int restore_layout_for_undo = current_layout_;

  // Важно: слово инвертируем относительно текущей раскладки.
  const int target_layout = (current_layout_ == 0) ? 1 : 0;

  // Оригинальный текст (для Ctrl+Z).
  std::optional<std::string> original_text_opt =
      key_entries_to_visible_text_checked(word, restore_layout_for_undo);

  // Строим видимый текст так, как если бы мы перепечатали те же скан-коды в
  // target_layout.
  auto replacement_opt = key_entries_to_visible_text_checked(word, target_layout);
  if (!replacement_opt.has_value()) {
    std::cerr << "[punto] Invert-layout: cannot build visible text (layout="
              << target_layout << ")\n";
    return;
  }
  std::string replacement = std::move(*replacement_opt);

  // Добавляем trailing whitespace (пробелы/табы) в конец.
  for (ScanCode c : buffer_.trailing()) {
    if (c == KEY_SPACE) {
      replacement.push_back(' ');
      if (original_text_opt.has_value()) {
        original_text_opt->push_back(' ');
      }
    } else if (c == KEY_TAB) {
      replacement.push_back('\t');
      if (original_text_opt.has_value()) {
        original_text_opt->push_back('\t');
      }
    }
  }

  // Удаляем слово + trailing и вставляем replacement одной операцией.
  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const std::size_t total_len = word.size() + buffer_.trailing_length();
  const bool ok = replace_text_oneshot(total_len, replacement,
                                      /*final_layout=*/target_layout,
                                      /*play_sound=*/true);
  if (ok && original_text_opt.has_value()) {
    set_last_undo_record(std::move(*original_text_opt), replacement,
                         /*restore_layout=*/restore_layout_for_undo,
                         /*is_auto_correction=*/false);
  }
}

void EventLoop::action_auto_invert_word(std::span<const KeyEntry> word,
                                        ScanCode trigger_code) {
  if (word.empty())
    return;

  std::cerr << "[punto] AUTO-INVERT: start (len=" << word.size() << ")\n";
  is_processing_macro_ = true;

  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return;
  }

  // 1. Отпускаем всё
  injector->release_all_modifiers();
  wait_and_buffer(std::chrono::microseconds{5000});

  // 2. Удаляем слово (turbo)
  injector->send_backspace(word.size(), true);

  // КРИТИЧЕСКАЯ ПАУЗА: даём приложению время обработать все Backspace
  // и обновить позицию курсора перед началом ввода новых символов.
  // Без этой паузы в медленных приложениях (Electron, Java Swing)
  // возникает гонка: новые символы вводятся до того, как удалены старые.
  wait_and_buffer(std::chrono::microseconds{60000}); // 60ms

  // 3. Переключаем раскладку
  switch_layout(true);

  // ДАЕМ ОС ВРЕМЯ НА ПЕРЕКЛЮЧЕНИЕ
  wait_and_buffer(std::chrono::microseconds{110000});

  // 4. Перепечатываем слово (turbo)
  injector->retype_buffer(word, true);

  // 5. Печатаем финализирующий символ, если он есть
  if (trigger_code != 0) {
    // Даем время на завершение перепечатывания слова
    wait_and_buffer(std::chrono::microseconds{25000});
    // Печатаем триггерный символ без turbo для надежности
    injector->tap_key(trigger_code, false, false);
  }

  std::cerr << "[punto] AUTO-INVERT: done\n";
  drain_pending_events();
}

void EventLoop::action_invert_case_word() {
  auto word = buffer_.get_active_word();
  if (word.empty()) {
    return;
  }

  auto visible_opt = key_entries_to_visible_text_checked(word, current_layout_);
  if (!visible_opt.has_value()) {
    std::cerr << "[punto] Invert-case: cannot build visible text (layout="
              << current_layout_ << ")\n";
    return;
  }

  std::string original_text = *visible_opt;
  std::string replacement = invert_case(*visible_opt);

  // Добавляем trailing whitespace (пробелы/табы) в конец.
  for (ScanCode c : buffer_.trailing()) {
    if (c == KEY_SPACE) {
      original_text.push_back(' ');
      replacement.push_back(' ');
    } else if (c == KEY_TAB) {
      original_text.push_back('\t');
      replacement.push_back('\t');
    }
  }

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const std::size_t total_len = word.size() + buffer_.trailing_length();
  const bool ok = replace_text_oneshot(total_len, replacement,
                                      /*final_layout=*/std::nullopt,
                                      /*play_sound=*/false);
  if (ok) {
    set_last_undo_record(std::move(original_text), replacement,
                         /*restore_layout=*/std::nullopt,
                         /*is_auto_correction=*/false);
  }
}

bool EventLoop::process_selection(
    std::function<std::string(std::string_view)> transform,
    std::optional<int> restore_layout_for_undo) {

  if (!clipboard_) {
    std::cerr << "[punto] Clipboard: недоступен\n";
    return false;
  }

  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return false;
  }

  const bool is_terminal = clipboard_->is_active_window_terminal();

  injector->release_all_modifiers();
  wait_and_buffer(std::chrono::microseconds{30000});
  flush_pending_release_frames();

  std::optional<std::string> text;

  if (is_terminal) {
    // В терминале: читаем PRIMARY selection (автоматически заполняется при выделении).
    text = clipboard_->get_text(Selection::Primary);
  } else {
    // В обычных приложениях: Ctrl+C для копирования выделения.
    //
    // Важно: если выделения нет, то Ctrl+C обычно не меняет CLIPBOARD.
    // Чтобы не трансформировать "старый" clipboard, читаем значение до и после.
    const std::optional<std::string> before_clip =
        clipboard_->get_text(Selection::Clipboard);
    if (!before_clip.has_value()) {
      std::cerr << "[punto] Clipboard: cannot read CLIPBOARD before copy\n";
      return false;
    }

    injector->send_key(KEY_LEFTCTRL, KeyState::Press);
    wait_and_buffer(std::chrono::microseconds{20000});
    injector->send_key(KEY_C, KeyState::Press);
    wait_and_buffer(std::chrono::microseconds{20000});
    injector->send_key(KEY_C, KeyState::Release);
    wait_and_buffer(std::chrono::microseconds{20000});
    injector->send_key(KEY_LEFTCTRL, KeyState::Release);

    // Даём приложению время выставить CLIPBOARD.
    wait_and_buffer(std::chrono::microseconds{200000});

    text = clipboard_->get_text(Selection::Clipboard);

    if (!text.has_value() || text->empty()) {
      return false;
    }

    if (*text == *before_clip) {
      // Скорее всего, выделения не было.
      return false;
    }
  }

  if (!text || text->empty()) {
    return false;
  }

  const std::string transformed = transform(*text);

  const ClipboardResult set_clip =
      clipboard_->set_text(Selection::Clipboard, transformed);
  if (set_clip != ClipboardResult::Ok) {
    std::cerr << "[punto] Clipboard: failed to set CLIPBOARD (res="
              << static_cast<int>(set_clip) << ")\n";
    return false;
  }

  const ClipboardResult set_primary =
      clipboard_->set_text(Selection::Primary, transformed);
  if (set_primary != ClipboardResult::Ok) {
    std::cerr << "[punto] Clipboard: failed to set PRIMARY (res="
              << static_cast<int>(set_primary) << ")\n";
    return false;
  }

  // Даём X11 шанс обновить selections.
  wait_and_buffer(std::chrono::microseconds{150000});

  // Для Ctrl+Shift+V в некоторых терминалах нужен EN layout.
  const std::optional<int> restore_layout =
      maybe_switch_layout_to_en_for_terminal_paste(is_terminal);

  if (!is_terminal) {
    // Критично: некоторые приложения НЕ заменяют выделение при paste hotkey.
    // Удаляем выделение вручную (Backspace удаляет весь selection).
    injector->tap_key(KEY_BACKSPACE, /*with_shift=*/false, /*turbo=*/false);

    wait_and_buffer(std::chrono::microseconds{30000});
    flush_pending_release_frames();
  }

  injector->send_paste(is_terminal);

  // Даем приложению время запросить содержимое clipboard.
  wait_and_buffer(std::chrono::microseconds{250000});

  if (restore_layout.has_value()) {
    (void)set_layout(*restore_layout, /*play_sound=*/false);
  }

  // Undo: в терминале мы вставляем "как есть" в позицию курсора, т.е.
  // откат = удалить вставку (в исходном состоянии текста не было).
  std::string undo_original = is_terminal ? std::string{} : *text;
  set_last_undo_record(std::move(undo_original), transformed,
                       restore_layout_for_undo,
                       /*is_auto_correction=*/false);

  return true;
}

void EventLoop::wait_and_buffer(std::chrono::microseconds us) {
  if (us.count() <= 0) {
    return;
  }

  const auto start = std::chrono::steady_clock::now();
  const auto end = start + us;

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= end) {
      break;
    }

    // Обслуживаем X11 selection ownership, даже когда stdin молчит.
    if (clipboard_) {
      clipboard_->pump_events();
    }

    const auto remaining_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - now);

    int timeout_ms = static_cast<int>(remaining_us.count() / 1000);

    // Ensure timeout is at least 1ms if there's time remaining.
    if (timeout_ms < 1 && remaining_us.count() > 0) {
      timeout_ms = 1;
    }

    // Мы должны регулярно обслуживать X11 события, иначе paste может не
    // получить содержимое selection. Поэтому дробим длинные ожидания.
    constexpr int kMaxPollSliceMs = 5;
    if (timeout_ms > kMaxPollSliceMs) {
      timeout_ms = kMaxPollSliceMs;
    }

    pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    const int ret = poll(&pfd, 1, timeout_ms);

    if (ret > 0 && (pfd.revents & POLLIN)) {
      input_event ev;
      while (true) {
        if (std::fread(&ev, sizeof(ev), 1, stdin) == 1) {
          constexpr std::size_t kPendingEventsCap = 5000;
          if (pending_events_.size() < kPendingEventsCap) {
            pending_events_.push_back(ev);
          } else {
            static bool warned = false;
            if (!warned) {
              warned = true;
              std::cerr
                  << "[punto] Input Guard(wait): pending_events overflow cap="
                  << kPendingEventsCap << " (dropping input events)\n";
            }
          }
        }
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
          break;
        }
      }
      continue;
    }

    if (ret < 0) {
      if (errno == EINTR) {
        continue; // Interrupted by signal, retry poll
      }
      break; // Other error
    }

    // ret == 0 (timeout) или без POLLIN: продолжаем ждать до end.
  }
}

void EventLoop::flush_pending_release_frames() {
  if (pending_events_.empty()) {
    return;
  }

  std::deque<input_event> keep;
  std::vector<input_event> frame;
  frame.reserve(16);

  std::size_t forwarded = 0;
  std::size_t kept = 0;
  std::size_t forwarded_safe_release = 0;
  std::size_t forwarded_full_frames = 0;

  auto flush_frame = [&]() {
    if (frame.empty()) {
      return;
    }

    bool frame_has_press = false;
    for (const auto &e : frame) {
      if (e.type == EV_KEY && e.value != 0) {
        frame_has_press = true;
        break;
      }
    }

    if (!frame_has_press) {
      // Fast path: release-only frame (safe to forward whole frame).
      for (const auto &e : frame) {
        emit_passthrough_event(e);
      }
      forwarded += frame.size();
      forwarded_full_frames++;
      frame.clear();
      return;
    }

    // Mixed frame: try to forward ONLY those EV_KEY releases that correspond
    // to keys already pressed in the app (key_down_ == true). Keep everything
    // else.
    bool forwarded_any = false;

    for (const auto &e : frame) {
      if (e.type == EV_KEY && e.value == 0) {
        const ScanCode code = e.code;
        if (code < key_down_.size() && key_down_[code] != 0) {
          emit_passthrough_event(e);
          forwarded++;
          forwarded_safe_release++;
          forwarded_any = true;
          continue;
        }
      }

      keep.push_back(e);
      kept++;
    }

    // If we forwarded releases from this mixed frame, emit an extra SYN_REPORT
    // so that the target app applies them immediately.
    if (forwarded_any) {
      input_event syn{};
      syn.type = EV_SYN;
      syn.code = SYN_REPORT;
      syn.value = 0;
      emit_passthrough_event(syn);
      forwarded++;
    }

    frame.clear();
  };

  while (!pending_events_.empty()) {
    input_event ev = pending_events_.front();
    pending_events_.pop_front();

    frame.push_back(ev);

    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
      flush_frame();
    }
  }

  // Хвост без SYN_REPORT: безопаснее не выпускать наружу (оставляем в keep).
  if (!frame.empty()) {
    for (const auto &e : frame) {
      keep.push_back(e);
    }
    kept += frame.size();
  }

  if (forwarded > 0) {
    std::cerr << "[punto] Input Guard: early-flush releases: forwarded="
              << forwarded << " (full_frames=" << forwarded_full_frames
              << " safe_release=" << forwarded_safe_release << ") kept=" << kept
              << "\n";
  }

  pending_events_ = std::move(keep);
}

void EventLoop::drain_pending_events() {
  if (pending_events_.empty()) {
    is_processing_macro_ = false;
    return;
  }

  std::cerr << "[punto] Input Guard: draining " << pending_events_.size()
            << " events\n";
  is_processing_macro_ = false;

  while (!pending_events_.empty()) {
    input_event ev = pending_events_.front();
    pending_events_.pop_front();
    handle_event(ev);
  }
}

void EventLoop::process_ready_results() {
  if (is_processing_macro_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (telemetry_.last_report_at.time_since_epoch().count() == 0) {
    telemetry_.last_report_at = now;
  }

  // Забираем все готовые результаты от воркеров.
  WordResult r;
  while (analysis_pool_.try_pop_result(r)) {
    // Stale results после сброса контекста.
    if (r.task_id < next_apply_task_id_) {
      continue;
    }

    telemetry_.analyzed_words++;
    telemetry_.analysis_us_sum += r.analysis_us;
    telemetry_.queue_us_sum += r.queue_us;

    if (r.analysis_us > telemetry_.analysis_us_max) {
      telemetry_.analysis_us_max = r.analysis_us;
    }
    if (r.queue_us > telemetry_.queue_us_max) {
      telemetry_.queue_us_max = r.queue_us;
    }

    if (r.need_switch) {
      telemetry_.need_switch_words++;
    }

    ready_results_[r.task_id] = r;
  }

  // Применяем строго по порядку.
  while (true) {
    auto it = ready_results_.find(next_apply_task_id_);
    if (it == ready_results_.end()) {
      break;
    }

    WordResult res = std::move(it->second);
    ready_results_.erase(it);

    // Проверяем, нужна ли какая-либо коррекция
    const bool has_correction =
        (res.correction_type != CorrectionType::NoCorrection);

    if (has_correction) {
      std::cerr << "[punto] Async-DECISION: task_id=" << res.task_id
                << " word_len=" << res.word_len
                << " analysis_len=" << res.analysis_len
                << " correction_type=" << static_cast<int>(res.correction_type)
                << " queue_us=" << res.queue_us
                << " analysis_us=" << res.analysis_us << "\n";

      auto mit = pending_words_.find(res.task_id);
      if (mit != pending_words_.end()) {
        // Проверяем, не находится ли слово в сессионных исключениях
        // (пользователь ранее отменял коррекцию этого слова)
        std::string word_ascii;
        for (const auto &entry : mit->second.word) {
          if (entry.code < kScancodeToChar.size()) {
            char c = kScancodeToChar[entry.code];
            if (c >= 'A' && c <= 'Z') {
              c = static_cast<char>(c + 32);
            }
            if (c != '\0') {
              word_ascii += c;
            }
          }
        }

        if (undo_detector_.is_excluded(word_ascii)) {
          std::cerr << "[punto] Skipping correction for excluded word: "
                    << word_ascii << "\n";
          pending_words_.erase(res.task_id);
          ++next_apply_task_id_;
          continue;
        }

        // Определяем тип коррекции и вызываем соответствующий метод
        switch (res.correction_type) {
        case CorrectionType::LayoutSwitch: {
          // Стандартное переключение раскладки (v2.6 логика)
          const int target_layout =
              (mit->second.layout_at_boundary == 0) ? 1 : 0;
          apply_correction(mit->second, target_layout);
          undo_detector_.on_correction_applied(res.task_id, word_ascii);
          break;
        }

        case CorrectionType::StickyShiftFix: {
          // Исправление регистра БЕЗ смены раскладки (ПРивет -> Привет)
          if (res.correction.has_value()) {
            apply_case_correction(mit->second, res.correction.value());
            undo_detector_.on_correction_applied(res.task_id, word_ascii);
          }
          break;
        }

        case CorrectionType::CombinedFix: {
          // Комбинированное исправление: смена раскладки + исправление регистра
          // (GHbdtn -> Привет)
          if (res.correction.has_value()) {
            const int target_layout =
                (mit->second.layout_at_boundary == 0) ? 1 : 0;
            apply_combined_correction(mit->second, target_layout,
                                      res.correction.value());
            undo_detector_.on_correction_applied(res.task_id, word_ascii);
          }
          break;
        }

        case CorrectionType::TypoFix: {
          // Исправление опечатки (v2.7+)
          // Используем ту же логику, что и для case fix —
          // она поддерживает разную длину слов
          if (res.correction.has_value()) {
            std::cerr << "[punto] TYPO-FIX: task_id=" << res.task_id
                      << " original_len=" << mit->second.word.size()
                      << " corrected_len=" << res.correction.value().size()
                      << "\n";
            apply_case_correction(mit->second, res.correction.value());
            undo_detector_.on_correction_applied(res.task_id, word_ascii);
          }
          break;
        }

        case CorrectionType::NoCorrection:
        default:
          break;
        }
      } else {
        std::cerr << "[punto] Async: missing meta for task_id=" << res.task_id
                  << " (skip)\n";
      }
    }

    pending_words_.erase(res.task_id);
    ++next_apply_task_id_;
  }

  // Периодическая телеметрия (агрегировано, чтобы не спамить).
  const auto dt = now - telemetry_.last_report_at;
  if (dt >= std::chrono::seconds{1}) {
    const std::uint64_t words = telemetry_.analyzed_words;
    const std::uint64_t corr = telemetry_.corrections;

    const std::uint64_t avg_queue =
        (words > 0) ? (telemetry_.queue_us_sum / words) : 0;
    const std::uint64_t avg_analysis =
        (words > 0) ? (telemetry_.analysis_us_sum / words) : 0;

    const std::uint64_t avg_tail =
        (corr > 0) ? (telemetry_.tail_len_sum / corr) : 0;
    const std::uint64_t avg_macro =
        (corr > 0) ? (telemetry_.correction_us_sum / corr) : 0;

    std::cerr << "[punto] Telemetry: words=" << words
              << " need_switch=" << telemetry_.need_switch_words
              << " avg_queue_us=" << avg_queue
              << " max_queue_us=" << telemetry_.queue_us_max
              << " avg_analysis_us=" << avg_analysis
              << " max_analysis_us=" << telemetry_.analysis_us_max
              << " corrections=" << corr << " avg_macro_us=" << avg_macro
              << " max_macro_us=" << telemetry_.correction_us_max
              << " avg_tail_len=" << avg_tail
              << " max_tail_len=" << telemetry_.tail_len_max
              << " xkb_set=" << (xkb_set_available_ ? "on" : "off") << "\n";

    telemetry_.last_report_at = now;

    // Сбрасываем интервальные счётчики.
    telemetry_.analyzed_words = 0;
    telemetry_.need_switch_words = 0;
    telemetry_.analysis_us_sum = 0;
    telemetry_.analysis_us_max = 0;
    telemetry_.queue_us_sum = 0;
    telemetry_.queue_us_max = 0;
    telemetry_.corrections = 0;
    telemetry_.correction_us_sum = 0;
    telemetry_.correction_us_max = 0;
    telemetry_.tail_len_sum = 0;
    telemetry_.tail_len_max = 0;
  }
}

void EventLoop::apply_correction(const PendingWordMeta &meta,
                                 int target_layout) {
  if (meta.word.empty()) {
    return;
  }

  const int original_layout = meta.layout_at_boundary;
  if ((original_layout != 0 && original_layout != 1) ||
      (target_layout != 0 && target_layout != 1)) {
    std::cerr << "[punto] Async: invalid layout values for task_id="
              << meta.task_id << " original=" << original_layout
              << " target=" << target_layout << "\n";
    return;
  }

  const std::uint64_t cursor = history_.cursor_pos();
  const std::uint64_t base = history_.base_pos();

  if (meta.start_pos < base || meta.end_pos > cursor) {
    std::cerr << "[punto] Async: history window miss for task_id="
              << meta.task_id << " (base=" << base << " end=" << cursor
              << " start=" << meta.start_pos << " word_end=" << meta.end_pos
              << ")\n";
    return;
  }

  // Tail = всё после слова (включая разделитель) до текущей позиции.
  if (!history_.get_range(meta.end_pos, cursor, tail_scratch_)) {
    std::cerr << "[punto] Async: failed to get tail for task_id=" << meta.task_id
              << "\n";
    return;
  }

  const std::uint64_t erase64 = cursor - meta.start_pos;
  const std::size_t erase = static_cast<std::size_t>(erase64);

  const std::size_t expected_retype = meta.word.size() + tail_scratch_.size();
  if (expected_retype != erase) {
    std::cerr << "[punto] Async: length invariant violated for task_id="
              << meta.task_id << " erase=" << erase
              << " retype=" << expected_retype << " (skip)\n";
    return;
  }

  auto word_text_opt = key_entries_to_visible_text_checked(
      std::span<const KeyEntry>{meta.word.data(), meta.word.size()}, target_layout);
  if (!word_text_opt.has_value()) {
    std::cerr << "[punto] Async: cannot build corrected word text for task_id="
              << meta.task_id << " (layout=" << target_layout << ")\n";
    return;
  }

  auto tail_text_opt = key_entries_to_visible_text_checked(
      std::span<const KeyEntry>{tail_scratch_.data(), tail_scratch_.size()},
      original_layout);
  if (!tail_text_opt.has_value()) {
    std::cerr << "[punto] Async: cannot build tail text for task_id=" << meta.task_id
              << " (layout=" << original_layout << ")\n";
    return;
  }

  std::string replacement;
  replacement.reserve(word_text_opt->size() + tail_text_opt->size());
  replacement += *word_text_opt;
  replacement += *tail_text_opt;

  // Оригинальный текст (для Ctrl+Z).
  std::optional<std::string> original_text_opt;
  {
    auto orig_word_text_opt = key_entries_to_visible_text_checked(
        std::span<const KeyEntry>{meta.word.data(), meta.word.size()},
        original_layout);
    if (orig_word_text_opt.has_value()) {
      std::string tmp = std::move(*orig_word_text_opt);
      tmp += *tail_text_opt;
      original_text_opt = std::move(tmp);
    }
  }

  std::cerr << "[punto] Async-CORRECT(oneshot): task_id=" << meta.task_id
            << " word_len=" << meta.word.size()
            << " tail_len=" << tail_scratch_.size() << " erase=" << erase
            << "\n";

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const auto macro_start = std::chrono::steady_clock::now();

  const bool ok = replace_text_oneshot(erase, replacement,
                                      /*final_layout=*/target_layout,
                                      /*play_sound=*/true);
  if (!ok) {
    std::cerr << "[punto] Async: oneshot replace failed for task_id=" << meta.task_id
              << " (skip)\n";
    return;
  }

  if (original_text_opt.has_value()) {
    set_last_undo_record(std::move(*original_text_opt), replacement,
                         /*restore_layout=*/original_layout,
                         /*is_auto_correction=*/true);
  }

  const auto macro_end = std::chrono::steady_clock::now();
  const auto macro_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            macro_end - macro_start)
                            .count();

  telemetry_.corrections++;
  telemetry_.correction_us_sum += static_cast<std::uint64_t>(macro_us);
  if (static_cast<std::uint64_t>(macro_us) > telemetry_.correction_us_max) {
    telemetry_.correction_us_max = static_cast<std::uint64_t>(macro_us);
  }

  telemetry_.tail_len_sum += static_cast<std::uint64_t>(tail_scratch_.size());
  if (tail_scratch_.size() > telemetry_.tail_len_max) {
    telemetry_.tail_len_max = tail_scratch_.size();
  }

  std::cerr << "[punto] Async-MACRO: task_id=" << meta.task_id
            << " macro_us=" << macro_us << "\n";
}

void EventLoop::apply_case_correction(
    const PendingWordMeta &meta, const std::vector<KeyEntry> &corrected_word) {

  if (meta.word.empty() || corrected_word.empty()) {
    return;
  }

  const int layout = meta.layout_at_boundary;
  if (layout != 0 && layout != 1) {
    std::cerr << "[punto] Async: invalid layout for case correction task_id="
              << meta.task_id << " layout=" << layout << "\n";
    return;
  }

  const std::uint64_t cursor = history_.cursor_pos();
  const std::uint64_t base = history_.base_pos();

  if (meta.start_pos < base || meta.end_pos > cursor) {
    std::cerr
        << "[punto] Async: history window miss for case correction task_id="
        << meta.task_id << "\n";
    return;
  }

  // Tail = всё после слова до текущей позиции
  if (!history_.get_range(meta.end_pos, cursor, tail_scratch_)) {
    std::cerr
        << "[punto] Async: failed to get tail for case correction task_id="
        << meta.task_id << "\n";
    return;
  }

  const std::uint64_t erase64 = cursor - meta.start_pos;
  const std::size_t erase = static_cast<std::size_t>(erase64);

  std::cerr << "[punto] Async-CASE-FIX(oneshot): task_id=" << meta.task_id
            << " word_len=" << meta.word.size()
            << " corrected_len=" << corrected_word.size()
            << " tail_len=" << tail_scratch_.size() << " erase=" << erase
            << "\n";

  auto word_text_opt = key_entries_to_visible_text_checked(
      std::span<const KeyEntry>{corrected_word.data(), corrected_word.size()},
      layout);
  if (!word_text_opt.has_value()) {
    std::cerr << "[punto] Async: cannot build corrected word text for task_id="
              << meta.task_id << " (layout=" << layout << ")\n";
    return;
  }

  auto tail_text_opt = key_entries_to_visible_text_checked(
      std::span<const KeyEntry>{tail_scratch_.data(), tail_scratch_.size()},
      layout);
  if (!tail_text_opt.has_value()) {
    std::cerr << "[punto] Async: cannot build tail text for task_id=" << meta.task_id
              << " (layout=" << layout << ")\n";
    return;
  }

  std::string replacement;
  replacement.reserve(word_text_opt->size() + tail_text_opt->size());
  replacement += *word_text_opt;
  replacement += *tail_text_opt;

  // Оригинальный текст (для Ctrl+Z).
  std::optional<std::string> original_text_opt;
  {
    auto orig_word_text_opt = key_entries_to_visible_text_checked(
        std::span<const KeyEntry>{meta.word.data(), meta.word.size()}, layout);
    if (orig_word_text_opt.has_value()) {
      std::string tmp = std::move(*orig_word_text_opt);
      tmp += *tail_text_opt;
      original_text_opt = std::move(tmp);
    }
  }

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const auto macro_start = std::chrono::steady_clock::now();

  const bool ok = replace_text_oneshot(erase, replacement,
                                      /*final_layout=*/std::nullopt,
                                      /*play_sound=*/false);
  if (!ok) {
    std::cerr << "[punto] Async: oneshot replace failed for task_id=" << meta.task_id
              << " (skip)\n";
    return;
  }

  if (original_text_opt.has_value()) {
    set_last_undo_record(std::move(*original_text_opt), replacement,
                         /*restore_layout=*/std::nullopt,
                         /*is_auto_correction=*/true);
  }

  const auto macro_end = std::chrono::steady_clock::now();
  const auto macro_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            macro_end - macro_start)
                            .count();

  telemetry_.corrections++;
  telemetry_.correction_us_sum += static_cast<std::uint64_t>(macro_us);
  if (static_cast<std::uint64_t>(macro_us) > telemetry_.correction_us_max) {
    telemetry_.correction_us_max = static_cast<std::uint64_t>(macro_us);
  }

  std::cerr << "[punto] Async-CASE-MACRO: task_id=" << meta.task_id
            << " macro_us=" << macro_us << "\n";
}

void EventLoop::apply_combined_correction(
    const PendingWordMeta &meta, int target_layout,
    const std::vector<KeyEntry> &corrected_word) {

  if (meta.word.empty() || corrected_word.empty()) {
    return;
  }

  const int original_layout = meta.layout_at_boundary;
  if ((original_layout != 0 && original_layout != 1) ||
      (target_layout != 0 && target_layout != 1)) {
    std::cerr << "[punto] Async: invalid layout values for combined fix task_id="
              << meta.task_id << " original=" << original_layout
              << " target=" << target_layout << "\n";
    return;
  }

  const std::uint64_t cursor = history_.cursor_pos();
  const std::uint64_t base = history_.base_pos();

  if (meta.start_pos < base || meta.end_pos > cursor) {
    std::cerr
        << "[punto] Async: history window miss for combined correction task_id="
        << meta.task_id << "\n";
    return;
  }

  if (!history_.get_range(meta.end_pos, cursor, tail_scratch_)) {
    std::cerr
        << "[punto] Async: failed to get tail for combined correction task_id="
        << meta.task_id << "\n";
    return;
  }

  const std::uint64_t erase64 = cursor - meta.start_pos;
  const std::size_t erase = static_cast<std::size_t>(erase64);

  std::cerr << "[punto] Async-COMBINED-FIX(oneshot): task_id=" << meta.task_id
            << " word_len=" << meta.word.size()
            << " corrected_len=" << corrected_word.size()
            << " tail_len=" << tail_scratch_.size() << " erase=" << erase
            << " target_layout=" << target_layout << "\n";

  auto word_text_opt = key_entries_to_visible_text_checked(
      std::span<const KeyEntry>{corrected_word.data(), corrected_word.size()},
      target_layout);
  if (!word_text_opt.has_value()) {
    std::cerr << "[punto] Async: cannot build corrected word text for task_id="
              << meta.task_id << " (layout=" << target_layout << ")\n";
    return;
  }

  auto tail_text_opt = key_entries_to_visible_text_checked(
      std::span<const KeyEntry>{tail_scratch_.data(), tail_scratch_.size()},
      original_layout);
  if (!tail_text_opt.has_value()) {
    std::cerr << "[punto] Async: cannot build tail text for task_id=" << meta.task_id
              << " (layout=" << original_layout << ")\n";
    return;
  }

  std::string replacement;
  replacement.reserve(word_text_opt->size() + tail_text_opt->size());
  replacement += *word_text_opt;
  replacement += *tail_text_opt;

  // Оригинальный текст (для Ctrl+Z).
  std::optional<std::string> original_text_opt;
  {
    auto orig_word_text_opt = key_entries_to_visible_text_checked(
        std::span<const KeyEntry>{meta.word.data(), meta.word.size()},
        original_layout);
    if (orig_word_text_opt.has_value()) {
      // Tail в original_layout уже посчитан в tail_text_opt.
      std::string tmp = std::move(*orig_word_text_opt);
      tmp += *tail_text_opt;
      original_text_opt = std::move(tmp);
    }
  }

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const auto macro_start = std::chrono::steady_clock::now();

  const bool ok = replace_text_oneshot(erase, replacement,
                                      /*final_layout=*/target_layout,
                                      /*play_sound=*/true);
  if (!ok) {
    std::cerr << "[punto] Async: oneshot replace failed for task_id=" << meta.task_id
              << " (skip)\n";
    return;
  }

  if (original_text_opt.has_value()) {
    set_last_undo_record(std::move(*original_text_opt), replacement,
                         /*restore_layout=*/original_layout,
                         /*is_auto_correction=*/true);
  }

  const auto macro_end = std::chrono::steady_clock::now();
  const auto macro_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            macro_end - macro_start)
                            .count();

  telemetry_.corrections++;
  telemetry_.correction_us_sum += static_cast<std::uint64_t>(macro_us);
  if (static_cast<std::uint64_t>(macro_us) > telemetry_.correction_us_max) {
    telemetry_.correction_us_max = static_cast<std::uint64_t>(macro_us);
  }

  std::cerr << "[punto] Async-COMBINED-MACRO: task_id=" << meta.task_id
            << " macro_us=" << macro_us << "\n";
}

void EventLoop::action_invert_layout_selection() {
  is_processing_macro_ = true;

  const int restore_layout_for_undo = current_layout_;

  if (process_selection(
          [](std::string_view text) { return invert_layout(text); },
          /*restore_layout_for_undo=*/restore_layout_for_undo)) {
    // Переключаем раскладку после успешной инверсии
    wait_and_buffer(std::chrono::microseconds{100000});
    switch_layout(true);
  }

  drain_pending_events();
}

void EventLoop::action_invert_case_selection() {
  is_processing_macro_ = true;
  (void)process_selection([](std::string_view text) { return invert_case(text); },
                          /*restore_layout_for_undo=*/std::nullopt);
  drain_pending_events();
}

void EventLoop::action_transliterate_selection() {
  is_processing_macro_ = true;
  (void)process_selection(
      [](std::string_view text) { return transliterate(text); },
      /*restore_layout_for_undo=*/std::nullopt);
  drain_pending_events();
}

IpcResult EventLoop::reload_config(const std::string &config_path) {
  std::filesystem::path system_path{std::string{kConfigPath}};
  std::filesystem::path load_path = system_path;
  bool tried_user = false;
  bool user_exists = false;

  const bool explicit_path = !config_path.empty();

  if (explicit_path) {
    load_path = std::filesystem::path{config_path};
    tried_user = true;
    std::error_code ec;
    if (!std::filesystem::exists(load_path, ec) || ec) {
      std::string msg = "Config file not found: " + config_path;
      std::cerr << "[punto] " << msg << "\\n";
      return {false, std::move(msg)};
    }
    user_exists = true; // путь указан явно и существует
  } else {
    std::optional<std::filesystem::path> user_path;
    if (x11_session_ && x11_session_->is_valid()) {
      // Важно: reload_config() может выполняться из IPC-потока.
      // Не трогаем процессное окружение (setenv) здесь, берём снапшот данных.
      const X11SessionInfo info = x11_session_->info();
      if (!info.home_dir.empty()) {
        user_path = std::filesystem::path{info.home_dir} /
                    std::filesystem::path{std::string{kUserConfigRelPath}};
      }
    }

    tried_user = user_path.has_value();
    if (user_path) {
      std::error_code ec;
      user_exists = std::filesystem::exists(*user_path, ec) && !ec;
      if (user_exists) {
        load_path = *user_path;
      }
    }
  }

  ConfigLoadOutcome loaded = load_config_checked(load_path);
  if (loaded.result != ConfigResult::Ok) {
    std::cerr << "[punto] Config reload failed: " << loaded.error << "\\n";
    return {false,
            loaded.error.empty() ? "Config reload failed" : loaded.error};
  }

  auto new_cfg = std::make_shared<Config>(std::move(loaded.config));

  // Всегда синхронизируем runtime-статус автопереключения с конфигом при
  // RELOAD. Это делает файл конфигурации единым источником истины и устраняет
  // рассинхронизацию после SET_STATUS (runtime-only).
  ipc_enabled_.store(new_cfg->auto_switch.enabled, std::memory_order_relaxed);

  auto new_analyzer = std::make_shared<LayoutAnalyzer>(new_cfg->auto_switch);

  auto new_injector = std::make_shared<KeyInjector>();
  new_injector->set_wait_func(
      [this](std::chrono::microseconds us) { this->wait_and_buffer(us); });

  // Публикуем новые снапшоты (без блокировок, безопасно для main loop).
  std::shared_ptr<const Config> cfg_const = new_cfg;
  std::shared_ptr<const LayoutAnalyzer> analyzer_const = new_analyzer;
  std::shared_ptr<const KeyInjector> injector_const = new_injector;

  std::atomic_store(&config_, std::move(cfg_const));
  std::atomic_store(&analyzer_, std::move(analyzer_const));
  std::atomic_store(&injector_, std::move(injector_const));

  if (sound_manager_) {
    sound_manager_->set_enabled(new_cfg->sound.enabled);
  }

  std::cerr << "[punto] Configuration reloaded: " << loaded.used_path << "\n";
  std::cerr << "[punto] auto_switch: enabled=" << new_cfg->auto_switch.enabled
            << ", threshold=" << new_cfg->auto_switch.threshold
            << ", min_word_len=" << new_cfg->auto_switch.min_word_len
            << ", min_score=" << new_cfg->auto_switch.min_score
            << ", max_rollback_words="
            << new_cfg->auto_switch.max_rollback_words << '\n';

  std::string message = "Loaded " + loaded.used_path.string();
  if (!explicit_path && tried_user && !user_exists) {
    message += " (user config not found; using system config)";
  }

  return {true, std::move(message)};
}

} // namespace punto
