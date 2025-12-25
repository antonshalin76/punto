/**
 * @file event_loop.cpp
 * @brief Реализация главного цикла обработки событий
 */

#include "punto/event_loop.hpp"
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
    auto cfg = std::atomic_load(&config_);
    auto injector = std::make_shared<KeyInjector>(cfg->delays);
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
    std::cerr << "[punto] Предупреждение: X11 сессия не инициализирована. "
                 "Операции с буфером обмена будут недоступны.\n";
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

  // Время последней попытки повторной инициализации X11
  auto last_x11_retry_time = std::chrono::steady_clock::now();
  constexpr auto kX11RetryInterval = std::chrono::seconds{3};

  // Главный цикл: проверяем флаг остановки на каждой итерации
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    // Ленивая повторная инициализация X11 если не удалось при старте (Bug #3)
    if (!x11_session_->is_valid()) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_x11_retry_time >= kX11RetryInterval) {
        last_x11_retry_time = now;
        std::cerr << "[punto] Attempting X11 re-initialization...\n";
        if (x11_session_->initialize()) {
          // X11 теперь доступен — создаём зависимые компоненты
          clipboard_ = std::make_unique<ClipboardManager>(*x11_session_);
          x11_session_->apply_environment();
          current_layout_ = x11_session_->get_current_keyboard_layout();
          if (current_layout_ < 0) {
            current_layout_ = 0;
          }
          std::cerr << "[punto] X11 re-initialized, layout: "
                    << (current_layout_ == 0 ? "EN" : "RU") << "\n";

          // Пересоздаём SoundManager с новыми данными X11Session
          auto cfg = std::atomic_load(&config_);
          sound_manager_ =
              std::make_unique<SoundManager>(*x11_session_, cfg->sound);
        }
      }
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

  // =========================================================================
  // Backspace
  // =========================================================================

  if (code == KEY_BACKSPACE) {
    buffer_.pop_char();
    (void)history_.pop_token();
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

void EventLoop::action_invert_layout_word() {
  auto word = buffer_.get_active_word();
  if (word.empty())
    return;

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return;
  }

  is_processing_macro_ = true;
  injector->release_all_modifiers();
  wait_and_buffer(cfg->delays.turbo_key_press);

  // Удаляем слово + trailing whitespace (используем turbo)
  std::size_t total_len = word.size() + buffer_.trailing_length();
  injector->send_backspace(total_len, true);

  // Переключаем раскладку
  switch_layout(true);
  wait_and_buffer(std::chrono::microseconds{60000}); // Wait for DE/OS

  // Перепечатываем с сохранением регистра (turbo)
  injector->retype_buffer(word, true);

  // Восстанавливаем trailing whitespace
  if (buffer_.trailing_length() > 0) {
    injector->retype_trailing(buffer_.trailing(), true);
  }

  // 5. Коммитим состояние
  if (buffer_.current_length() > 0) {
    buffer_.commit_word();
  }

  drain_pending_events();
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
  if (word.empty())
    return;

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return;
  }

  is_processing_macro_ = true;
  injector->release_all_modifiers();
  wait_and_buffer(cfg->delays.turbo_key_press);

  // Удаляем слово + trailing
  std::size_t total_len = word.size() + buffer_.trailing_length();
  injector->send_backspace(total_len, true);

  // Перепечатываем с инвертированным регистром
  for (const auto &entry : word) {
    injector->tap_key(entry.code, !entry.shifted, true);
  }

  // Восстанавливаем trailing
  if (buffer_.trailing_length() > 0) {
    injector->retype_trailing(buffer_.trailing(), true);
  }

  if (buffer_.current_length() > 0) {
    buffer_.commit_word();
  }

  drain_pending_events();
}

bool EventLoop::process_selection(
    std::function<std::string(std::string_view)> transform) {

  if (!clipboard_) {
    std::cerr << "[punto] Буфер обмена недоступен\n";
    return false;
  }

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return false;
  }

  injector->release_all_modifiers();
  usleep(static_cast<useconds_t>(cfg->delays.key_press.count()));

  bool is_terminal = clipboard_->is_active_window_terminal();

  std::optional<std::string> text;

  if (is_terminal) {
    // В терминале: читаем PRIMARY selection (автоматически заполняется)
    text = clipboard_->get_text(Selection::Primary);

    if (text && !text->empty()) {
      // Удаляем выделенный текст через Backspace
      std::size_t len = text->size();
      for (std::size_t i = 0; i < len; ++i) {
        injector->tap_key(KEY_BACKSPACE, false);
      }
      usleep(100000); // 100ms
    }
  } else {
    // В обычных приложениях: Ctrl+C для копирования
    injector->send_key(KEY_LEFTCTRL, KeyState::Press);
    usleep(20000); // 20ms
    injector->send_key(KEY_C, KeyState::Press);
    usleep(20000);
    injector->send_key(KEY_C, KeyState::Release);
    usleep(20000);
    injector->send_key(KEY_LEFTCTRL, KeyState::Release);
    usleep(200000); // 200ms для clipboard

    text = clipboard_->get_text(Selection::Clipboard);
  }

  if (!text || text->empty()) {
    return false;
  }

  // Трансформируем текст
  std::string transformed = transform(*text);

  // Записываем в clipboard
  clipboard_->set_text(Selection::Clipboard, transformed);
  usleep(100000);

  // Вставляем
  if (is_terminal) {
    // Ctrl+Shift+V для терминала
    injector->send_key(KEY_LEFTCTRL, KeyState::Press);
    injector->send_key(KEY_LEFTSHIFT, KeyState::Press);
    usleep(20000);
    injector->send_key(KEY_V, KeyState::Press);
    usleep(20000);
    injector->send_key(KEY_V, KeyState::Release);
    usleep(20000);
    injector->send_key(KEY_LEFTSHIFT, KeyState::Release);
    injector->send_key(KEY_LEFTCTRL, KeyState::Release);
  } else {
    // Ctrl+V для обычных приложений
    injector->send_key(KEY_LEFTCTRL, KeyState::Press);
    usleep(20000);
    injector->send_key(KEY_V, KeyState::Press);
    usleep(20000);
    injector->send_key(KEY_V, KeyState::Release);
    usleep(20000);
    injector->send_key(KEY_LEFTCTRL, KeyState::Release);
  }

  return true;
}

void EventLoop::wait_and_buffer(std::chrono::microseconds us) {
  if (us.count() <= 0)
    return;

  auto start = std::chrono::steady_clock::now();
  auto end = start + us;

  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= end)
      break;

    auto remaining_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - now);
    int timeout_ms = static_cast<int>(remaining_us.count() / 1000);

    // Ensure timeout is at least 1ms if there's time remaining,
    // or 0 if time is up/very little left.
    if (timeout_ms < 1 && remaining_us.count() > 0) {
      timeout_ms = 1;
    } else if (remaining_us.count() <= 0) {
      break; // Time is up
    }

    pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    int ret = poll(&pfd, 1, timeout_ms);

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
    } else if (ret < 0) {
      if (errno == EINTR)
        continue; // Interrupted by signal, retry poll
      break;      // Other error
    } else {
      // timeout - we waited 'timeout_ms' ms, so 'now' should be >= 'end'
      // or no events arrived within the timeout.
      break;
    }
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
        // Определяем тип коррекции и вызываем соответствующий метод
        switch (res.correction_type) {
        case CorrectionType::LayoutSwitch: {
          // Стандартное переключение раскладки (v2.6 логика)
          const int target_layout =
              (mit->second.layout_at_boundary == 0) ? 1 : 0;
          apply_correction(mit->second, target_layout);
          break;
        }

        case CorrectionType::StickyShiftFix: {
          // Исправление регистра БЕЗ смены раскладки (ПРивет -> Привет)
          if (res.correction.has_value()) {
            apply_case_correction(mit->second, res.correction.value());
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

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return;
  }

  // Ограничение по глубине: если мета уже была выкинута, сюда не попадём.

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
    std::cerr << "[punto] Async: failed to get tail for task_id="
              << meta.task_id << "\n";
    return;
  }

  const std::uint64_t erase64 = cursor - meta.start_pos;
  const std::size_t erase = static_cast<std::size_t>(erase64);

  const std::size_t expected_retype = meta.word.size() + tail_scratch_.size();
  if (expected_retype != erase) {
    // Инвариант: число удалённых символов должно совпасть с числом вставленных.
    std::cerr << "[punto] Async: length invariant violated for task_id="
              << meta.task_id << " erase=" << erase
              << " retype=" << expected_retype << " (skip)\n";
    return;
  }

  const bool has_delim =
      !tail_scratch_.empty() && (tail_scratch_.front().code == KEY_SPACE ||
                                 tail_scratch_.front().code == KEY_TAB);
  const ScanCode delim_code = has_delim ? tail_scratch_.front().code : 0;

  const std::span<const KeyEntry> tail_after_delim =
      has_delim ? std::span<const KeyEntry>{tail_scratch_.data() + 1,
                                            tail_scratch_.size() - 1}
                : std::span<const KeyEntry>{tail_scratch_.data(),
                                            tail_scratch_.size()};

  bool tail_layout_invariant = true;
  for (const KeyEntry &e : tail_after_delim) {
    if (e.code != KEY_SPACE && e.code != KEY_TAB) {
      tail_layout_invariant = false;
      break;
    }
  }

  std::cerr << "[punto] Async-CORRECT: task_id=" << meta.task_id
            << " word_len=" << meta.word.size()
            << " tail_len=" << tail_scratch_.size() << " delim="
            << (has_delim ? (delim_code == KEY_SPACE ? "SPACE" : "TAB")
                          : "<none>")
            << " tail_invariant=" << (tail_layout_invariant ? 1 : 0) << "\n";

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const auto macro_start = std::chrono::steady_clock::now();

  injector->release_all_modifiers();

  // Даем физическим key-release шанс прийти в stdin.
  // Иначе мы можем инжектировать нажатия, пока клавиша ещё "зажата" для
  // приложения (особенно последняя буква и SPACE), и тогда press будет
  // проигнорирован.
  wait_and_buffer(cfg->delays.key_press + std::chrono::microseconds{30000});
  flush_pending_release_frames();

  wait_and_buffer(cfg->delays.turbo_key_press);
  flush_pending_release_frames();

  const int original_layout = meta.layout_at_boundary;

  auto verify_layout = [&](int expected, const char *phase) -> bool {
    if (x11_session_ && x11_session_->is_valid()) {
      x11_session_->apply_environment();
      const int os_layout = x11_session_->get_current_keyboard_layout();
      if (os_layout == 0 || os_layout == 1) {
        if (os_layout != expected) {
          std::cerr << "[punto] Async: layout mismatch (" << phase
                    << ") expected=" << expected << " os_layout=" << os_layout
                    << " task_id=" << meta.task_id << "\n";
          return false;
        }
      }
    }
    return true;
  };

  // Важно: если мы не можем реально переключить раскладку, лучше не удалять
  // текст. Иначе получим «звук есть, а коррекция не произошла» или испортим
  // хвост.
  if (tail_layout_invariant) {
    if (!set_layout(target_layout, false)) {
      std::cerr << "[punto] Async: failed to set target layout="
                << target_layout << " for task_id=" << meta.task_id
                << " (skip)\n";
      return;
    }
    wait_and_buffer(cfg->delays.layout_switch);
    if (!verify_layout(target_layout, "preflight-target")) {
      return;
    }
  } else {
    // Preflight: проверяем оба направления переключения до удаления.
    if (!set_layout(target_layout, false)) {
      std::cerr << "[punto] Async: failed to set target layout="
                << target_layout << " for task_id=" << meta.task_id
                << " (skip)\n";
      return;
    }
    wait_and_buffer(cfg->delays.layout_switch);
    if (!verify_layout(target_layout, "preflight-target")) {
      return;
    }

    if (!set_layout(original_layout, false)) {
      std::cerr << "[punto] Async: failed to set original layout="
                << original_layout << " for task_id=" << meta.task_id
                << " (skip)\n";
      return;
    }
    wait_and_buffer(cfg->delays.layout_switch);
    if (!verify_layout(original_layout, "preflight-original")) {
      return;
    }

    if (!set_layout(target_layout, false)) {
      std::cerr << "[punto] Async: failed to re-set target layout="
                << target_layout << " for task_id=" << meta.task_id
                << " (skip)\n";
      return;
    }
    wait_and_buffer(cfg->delays.layout_switch);
    if (!verify_layout(target_layout, "preflight-target-2")) {
      return;
    }
  }

  // 1) Удаляем слово + хвост.
  injector->send_backspace(erase, true);

  // Если релизы пришли позже — выпускаем их до перепечатки.
  flush_pending_release_frames();

  // 2) Печатаем слово в целевой раскладке.
  injector->retype_buffer(
      std::span<const KeyEntry>{meta.word.data(), meta.word.size()}, true);

  // Разделитель (SPACE/TAB) печатаем явно и без turbo — это критично,
  // иначе в некоторых приложениях может "пропадать" пробел.
  if (has_delim && delim_code != 0) {
    // На случай, если release пробела пришёл позже, чем мы начали макрос.
    flush_pending_release_frames();
    wait_and_buffer(cfg->delays.key_press);
    injector->tap_key(delim_code, false, false);
  }

  // 3) Печатаем хвост ПОСЛЕ разделителя.
  // Если хвост содержит только пробелы/таб — он инвариантен к раскладке,
  // и мы избегаем лишних переключений.
  if (!tail_after_delim.empty()) {
    if (!tail_layout_invariant) {
      if (!set_layout(original_layout, false)) {
        std::cerr << "[punto] Async: failed to set original layout="
                  << original_layout << " for task_id=" << meta.task_id
                  << " (tail lost)\n";
        return;
      }
      wait_and_buffer(cfg->delays.layout_switch);
      if (!verify_layout(original_layout, "tail-original")) {
        return;
      }
    }

    injector->retype_buffer(tail_after_delim, true);
  }

  // 4) Финально оставляем целевую раскладку пользователю.
  if (!set_layout(target_layout, false)) {
    std::cerr << "[punto] Async: failed to finalize target layout="
              << target_layout << " for task_id=" << meta.task_id << "\n";
    return;
  }
  wait_and_buffer(cfg->delays.layout_switch);
  if (!verify_layout(target_layout, "final-target")) {
    return;
  }

  if (sound_manager_) {
    sound_manager_->play_for_layout(target_layout);
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

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
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

  // Для case correction длина может совпадать с исходной
  // (ПРивет -> Привет имеют одинаковую длину)

  std::cerr << "[punto] Async-CASE-FIX: task_id=" << meta.task_id
            << " word_len=" << meta.word.size()
            << " corrected_len=" << corrected_word.size()
            << " tail_len=" << tail_scratch_.size() << "\n";

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const auto macro_start = std::chrono::steady_clock::now();

  injector->release_all_modifiers();
  wait_and_buffer(cfg->delays.key_press + std::chrono::microseconds{30000});
  flush_pending_release_frames();
  wait_and_buffer(cfg->delays.turbo_key_press);
  flush_pending_release_frames();

  // 1) Удаляем слово + хвост
  injector->send_backspace(erase, true);
  flush_pending_release_frames();

  // 2) Печатаем исправленное слово (с правильным регистром)
  injector->retype_buffer(
      std::span<const KeyEntry>{corrected_word.data(), corrected_word.size()},
      true);

  // 3) Печатаем хвост
  if (!tail_scratch_.empty()) {
    // Разделитель
    const bool has_delim = (tail_scratch_.front().code == KEY_SPACE ||
                            tail_scratch_.front().code == KEY_TAB);
    const ScanCode delim_code = has_delim ? tail_scratch_.front().code : 0;

    if (has_delim && delim_code != 0) {
      flush_pending_release_frames();
      wait_and_buffer(cfg->delays.key_press);
      injector->tap_key(delim_code, false, false);
    }

    const std::span<const KeyEntry> tail_after_delim =
        has_delim ? std::span<const KeyEntry>{tail_scratch_.data() + 1,
                                              tail_scratch_.size() - 1}
                  : std::span<const KeyEntry>{tail_scratch_.data(),
                                              tail_scratch_.size()};

    if (!tail_after_delim.empty()) {
      injector->retype_buffer(tail_after_delim, true);
    }
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

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
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

  std::cerr << "[punto] Async-COMBINED-FIX: task_id=" << meta.task_id
            << " word_len=" << meta.word.size()
            << " corrected_len=" << corrected_word.size()
            << " tail_len=" << tail_scratch_.size()
            << " target_layout=" << target_layout << "\n";

  is_processing_macro_ = true;
  struct DrainGuard {
    EventLoop *self;
    ~DrainGuard() { self->drain_pending_events(); }
  } drain_guard{this};

  const auto macro_start = std::chrono::steady_clock::now();

  injector->release_all_modifiers();
  wait_and_buffer(cfg->delays.key_press + std::chrono::microseconds{30000});
  flush_pending_release_frames();
  wait_and_buffer(cfg->delays.turbo_key_press);
  flush_pending_release_frames();

  // Переключаем раскладку ПЕРЕД удалением
  if (!set_layout(target_layout, false)) {
    std::cerr << "[punto] Async: failed to set target layout=" << target_layout
              << " for combined correction task_id=" << meta.task_id
              << " (skip)\n";
    return;
  }
  wait_and_buffer(cfg->delays.layout_switch);

  // 1) Удаляем слово + хвост
  injector->send_backspace(erase, true);
  flush_pending_release_frames();

  // 2) Печатаем исправленное слово в целевой раскладке
  injector->retype_buffer(
      std::span<const KeyEntry>{corrected_word.data(), corrected_word.size()},
      true);

  // 3) Печатаем хвост
  if (!tail_scratch_.empty()) {
    const bool has_delim = (tail_scratch_.front().code == KEY_SPACE ||
                            tail_scratch_.front().code == KEY_TAB);
    const ScanCode delim_code = has_delim ? tail_scratch_.front().code : 0;

    if (has_delim && delim_code != 0) {
      flush_pending_release_frames();
      wait_and_buffer(cfg->delays.key_press);
      injector->tap_key(delim_code, false, false);
    }

    const std::span<const KeyEntry> tail_after_delim =
        has_delim ? std::span<const KeyEntry>{tail_scratch_.data() + 1,
                                              tail_scratch_.size() - 1}
                  : std::span<const KeyEntry>{tail_scratch_.data(),
                                              tail_scratch_.size()};

    if (!tail_after_delim.empty()) {
      // Хвост может быть в другой раскладке - нужно вернуться назад
      const int original_layout = meta.layout_at_boundary;

      // Проверяем, есть ли буквы в хвосте
      bool tail_has_letters = false;
      for (const auto &e : tail_after_delim) {
        if (e.code != KEY_SPACE && e.code != KEY_TAB) {
          tail_has_letters = true;
          break;
        }
      }

      if (tail_has_letters) {
        (void)set_layout(original_layout, false);
        wait_and_buffer(cfg->delays.layout_switch);
      }

      injector->retype_buffer(tail_after_delim, true);

      // Возвращаем целевую раскладку
      if (tail_has_letters) {
        (void)set_layout(target_layout, false);
        wait_and_buffer(cfg->delays.layout_switch);
      }
    }
  }

  if (sound_manager_) {
    sound_manager_->play_for_layout(target_layout);
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
  if (process_selection(
          [](std::string_view text) { return invert_layout(text); })) {
    // Переключаем раскладку после успешной инверсии
    auto cfg = std::atomic_load(&config_);
    wait_and_buffer(cfg->delays.layout_switch);
    switch_layout(true);
  }
  drain_pending_events();
}

void EventLoop::action_invert_case_selection() {
  is_processing_macro_ = true;
  process_selection([](std::string_view text) { return invert_case(text); });
  drain_pending_events();
}

void EventLoop::action_transliterate_selection() {
  is_processing_macro_ = true;
  process_selection([](std::string_view text) { return transliterate(text); });
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
      // Обновляем окружение для корректного чтения $HOME из активной сессии
      x11_session_->apply_environment();
      const auto &info = x11_session_->info();
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

  auto new_injector = std::make_shared<KeyInjector>(new_cfg->delays);
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
