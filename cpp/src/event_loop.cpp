/**
 * @file event_loop.cpp
 * @brief Реализация главного цикла обработки событий
 */

#include "punto/event_loop.hpp"
#include "punto/sound_manager.hpp"
#include "punto/scancode_map.hpp"
#include "punto/text_processor.hpp"

#include <cstdio>
#include <iostream>
#include <poll.h>
#include <unistd.h>

namespace punto {

EventLoop::EventLoop(Config config)
    : config_{std::make_shared<Config>(std::move(config))},
      analyzer_{std::make_shared<LayoutAnalyzer>(config_->auto_switch)} {
  ipc_enabled_.store(config_->auto_switch.enabled, std::memory_order_relaxed);
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

  // После X11 инициализации можем вычислить реальный $HOME активного пользователя
  // и перечитать ~/.config/... (если существует).
  {
    IpcResult res = reload_config();
    if (!res.success) {
      std::cerr << "[punto] Warning: initial config reload failed: "
                << res.message << "\n";
    }
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
  ipc_server_ = std::make_unique<IpcServer>(ipc_enabled_,
                                           [this](const std::string &path) {
                                             return reload_config(path);
                                           });
  if (!ipc_server_->start()) {
    std::cerr << "[punto] Warning: IPC server failed to start. "
                 "Tray control will be unavailable.\n";
    // Не фатальная ошибка — базовая функциональность работает
  }

  initialized_ = true;
  return true;
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

  while (std::fread(&ev, sizeof(ev), 1, stdin) == 1) {
    handle_event(ev);
  }

  return 0;
}

void EventLoop::handle_event(const input_event &ev) {
  // If we are processing a macro, buffer ALL events (including EV_SYN, EV_MSC)
  if (is_processing_macro_) {
    if (pending_events_.size() < 200) {
      pending_events_.push_back(ev);
    }
    return;
  }

  if (ev.type != EV_KEY) {
    KeyInjector::emit_event(ev);
    return;
  }

  const ScanCode code = ev.code;
  const bool pressed = ev.value != 0;
  const bool is_press = ev.value == 1; // Только первое нажатие, не repeat

  auto cfg = std::atomic_load(&config_);

  // =========================================================================
  // Обновление состояния модификаторов
  // =========================================================================

  if (is_modifier(code)) {
    update_modifier_state(code, pressed);
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Обрабатываем только нажатия (не отпускания и не repeat)
  // =========================================================================

  if (!is_press) {
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Backspace
  // =========================================================================

  if (code == KEY_BACKSPACE) {
    buffer_.pop_char();
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Горячие клавиши (PAUSE + модификаторы)
  // =========================================================================

  if (code == KEY_PAUSE) {
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
    KeyInjector::emit_event(ev);
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

    bool is_en_layout = (current_layout_ == 0);

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

    // Проверяем, включено ли автопереключение через IPC
    bool ipc_enabled = ipc_enabled_.load(std::memory_order_relaxed);

    if (ipc_enabled && analysis_word.size() >= cfg->auto_switch.min_word_len) {
      bool need_switch = false;

      // === ГИБРИДНЫЙ АНАЛИЗ ===
      // 1. Сначала проверяем словарь (высокая точность)
      DictResult dict_result = dict_.lookup(analysis_word);

      if (dict_result == DictResult::English) {
        if (!is_en_layout) {
          std::cerr << "[punto] DICT: EN word in RU layout -> SWITCH\n";
          need_switch = true;
        }
      } else if (dict_result == DictResult::Russian) {
        if (is_en_layout) {
          std::cerr << "[punto] DICT: RU word in EN layout -> SWITCH\n";
          need_switch = true;
        }
      } else if (dict_result == DictResult::Unknown) {
        // 2. Слово не в словаре — используем N-граммный анализ как fallback
        auto analyzer = std::atomic_load(&analyzer_);
        auto result = analyzer->analyze(analysis_word);
        if (result.should_switch) {
          if (is_en_layout && result.ru_score > result.en_score) {
            std::cerr << "[punto] NGRAM: EN -> RU (en=" << result.en_score
                      << " ru=" << result.ru_score << ")\n";
            need_switch = true;
          } else if (!is_en_layout && result.en_score > result.ru_score) {
            std::cerr << "[punto] NGRAM: RU -> EN (en=" << result.en_score
                      << " ru=" << result.ru_score << ")\n";
            need_switch = true;
          }
        }
      }

      if (need_switch) {
        action_auto_invert_word(full_word, code);
        // Обязательно сбрасываем буфер после инверсии, иначе в следующей
        // итерации мы будем учитывать старые скан-коды и удалять лишнее
        buffer_.commit_word();
        buffer_.push_trailing(code);
        return;
      }
    }

    buffer_.commit_word();
    buffer_.push_trailing(code);
    KeyInjector::emit_event(ev);
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
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Enter — полный сброс
  // =========================================================================

  if (code == KEY_ENTER || code == KEY_KPENTER) {
    buffer_.reset_all();
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Буквенные клавиши
  // =========================================================================

  if (is_letter_key(code)) {
    buffer_.push_char(code, modifiers_.any_shift());
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Навигационные клавиши — полный сброс буфера
  // =========================================================================

  if (is_navigation_key(code)) {
    buffer_.reset_all();
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Функциональные клавиши — пропускаем без сброса
  // =========================================================================

  if (is_function_key(code)) {
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Неизвестные клавиши — сброс текущего слова
  // =========================================================================

  buffer_.reset_current();
  KeyInjector::emit_event(ev);
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

void EventLoop::switch_layout() {
  // Инвертируем внутреннее состояние
  current_layout_ = (current_layout_ == 0) ? 1 : 0;
  last_sync_time_ = std::chrono::steady_clock::now();

  if (sound_manager_) {
    sound_manager_->play_for_layout(current_layout_);
  }

  auto cfg = std::atomic_load(&config_);
  auto injector = std::atomic_load(&injector_);
  if (!injector) {
    return;
  }

  // Эмулируем нажатие горячей клавиши (единственный надежный способ)
  injector->send_layout_hotkey(cfg->hotkey.modifier, cfg->hotkey.key);
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
  switch_layout();
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
  wait_and_buffer(std::chrono::microseconds{5000});

  // 3. Переключаем раскладку
  switch_layout();

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
          if (pending_events_.size() < 1000) {
            pending_events_.push_back(ev);
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

void EventLoop::action_invert_layout_selection() {
  is_processing_macro_ = true;
  if (process_selection(
          [](std::string_view text) { return invert_layout(text); })) {
    // Переключаем раскладку после успешной инверсии
    auto cfg = std::atomic_load(&config_);
    wait_and_buffer(cfg->delays.layout_switch);
    switch_layout();
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
    return {false, loaded.error.empty() ? "Config reload failed" : loaded.error};
  }

  auto new_cfg = std::make_shared<Config>(std::move(loaded.config));

  // Всегда синхронизируем runtime-статус автопереключения с конфигом при RELOAD.
  // Это делает файл конфигурации единым источником истины и устраняет
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
            << ", min_score=" << new_cfg->auto_switch.min_score << '\n';

  std::string message = "Loaded "+loaded.used_path.string();
  if (!explicit_path && tried_user && !user_exists) {
    message += " (user config not found; using system config)";
  }

  return {true, std::move(message)};
}

} // namespace punto
