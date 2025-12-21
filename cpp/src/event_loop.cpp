/**
 * @file event_loop.cpp
 * @brief Реализация главного цикла обработки событий
 */

#include "punto/event_loop.hpp"
#include "punto/scancode_map.hpp"
#include "punto/text_processor.hpp"

#include <cstdio>
#include <iostream>
#include <unistd.h>

namespace punto {

EventLoop::EventLoop(Config config) : config_{std::move(config)} {}

EventLoop::~EventLoop() = default;

bool EventLoop::initialize() {
  if (initialized_)
    return true;

  // Создаём KeyInjector
  injector_ = std::make_unique<KeyInjector>(config_.delays);

  // Инициализируем X11 сессию
  x11_session_ = std::make_unique<X11Session>();
  if (!x11_session_->initialize()) {
    std::cerr << "[punto] Предупреждение: X11 сессия не инициализирована. "
                 "Операции с буфером обмена будут недоступны.\n";
    // Не фатальная ошибка — базовая функциональность работает
  } else {
    // Создаём ClipboardManager
    clipboard_ = std::make_unique<ClipboardManager>(*x11_session_);
  }

  initialized_ = true;
  return true;
}

int EventLoop::run() {
  if (!initialize()) {
    return 1;
  }

  // Отключаем буферизацию для минимальной латентности
  std::setbuf(stdin, nullptr);
  std::setbuf(stdout, nullptr);

  input_event ev{};

  while (std::fread(&ev, sizeof(ev), 1, stdin) == 1) {
    if (ev.type != EV_KEY) {
      // Пропускаем не-клавиатурные события без изменений
      KeyInjector::emit_event(ev);
      continue;
    }

    handle_key_event(ev);
  }

  return 0;
}

void EventLoop::handle_key_event(const input_event &ev) {
  const ScanCode code = ev.code;
  const bool pressed = ev.value != 0;
  const bool is_press = ev.value == 1; // Только первое нажатие, не repeat

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
    buffer_.reset_current();
    KeyInjector::emit_event(ev);
    return;
  }

  // =========================================================================
  // Разделители слов (пробел, таб)
  // =========================================================================

  if (code == KEY_SPACE || code == KEY_TAB) {
    buffer_.commit_word();
    buffer_.push_trailing(code);
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
  injector_->send_layout_hotkey(config_.hotkey.modifier, config_.hotkey.key);
}

void EventLoop::action_invert_layout_word() {
  auto word = buffer_.get_active_word();
  if (word.empty())
    return;

  injector_->release_all_modifiers();

  // Удаляем слово + trailing whitespace
  std::size_t total_len = word.size() + buffer_.trailing_length();
  injector_->send_backspace(total_len);

  // Переключаем раскладку
  switch_layout();

  // Перепечатываем с сохранением регистра
  injector_->retype_buffer(word);

  // Восстанавливаем trailing whitespace
  if (buffer_.trailing_length() > 0) {
    injector_->retype_trailing(buffer_.trailing());
  }

  // Если было текущее слово, сохраняем его как last и очищаем
  if (buffer_.current_length() > 0) {
    buffer_.commit_word();
  }
}

void EventLoop::action_invert_case_word() {
  auto word = buffer_.get_active_word();
  if (word.empty())
    return;

  injector_->release_all_modifiers();

  // Удаляем слово + trailing
  std::size_t total_len = word.size() + buffer_.trailing_length();
  injector_->send_backspace(total_len);

  // Перепечатываем с инвертированным регистром
  for (const auto &entry : word) {
    injector_->tap_key(entry.code, !entry.shifted);
  }

  // Восстанавливаем trailing
  if (buffer_.trailing_length() > 0) {
    injector_->retype_trailing(buffer_.trailing());
  }

  if (buffer_.current_length() > 0) {
    buffer_.commit_word();
  }
}

bool EventLoop::process_selection(
    std::function<std::string(std::string_view)> transform) {

  if (!clipboard_) {
    std::cerr << "[punto] Буфер обмена недоступен\n";
    return false;
  }

  injector_->release_all_modifiers();
  usleep(static_cast<useconds_t>(config_.delays.key_press.count()));

  bool is_terminal = clipboard_->is_active_window_terminal();

  std::optional<std::string> text;

  if (is_terminal) {
    // В терминале: читаем PRIMARY selection (автоматически заполняется)
    text = clipboard_->get_text(Selection::Primary);

    if (text && !text->empty()) {
      // Удаляем выделенный текст через Backspace
      std::size_t len = text->size(); // TODO: подсчёт UTF-8 символов
      for (std::size_t i = 0; i < len; ++i) {
        injector_->tap_key(KEY_BACKSPACE, false);
      }
      usleep(100000); // 100ms
    }
  } else {
    // В обычных приложениях: Ctrl+C для копирования
    injector_->send_key(KEY_LEFTCTRL, KeyState::Press);
    usleep(20000); // 20ms
    injector_->send_key(KEY_C, KeyState::Press);
    usleep(20000);
    injector_->send_key(KEY_C, KeyState::Release);
    usleep(20000);
    injector_->send_key(KEY_LEFTCTRL, KeyState::Release);
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
    injector_->send_key(KEY_LEFTCTRL, KeyState::Press);
    injector_->send_key(KEY_LEFTSHIFT, KeyState::Press);
    usleep(20000);
    injector_->send_key(KEY_V, KeyState::Press);
    usleep(20000);
    injector_->send_key(KEY_V, KeyState::Release);
    usleep(20000);
    injector_->send_key(KEY_LEFTSHIFT, KeyState::Release);
    injector_->send_key(KEY_LEFTCTRL, KeyState::Release);
  } else {
    // Ctrl+V для обычных приложений
    injector_->send_key(KEY_LEFTCTRL, KeyState::Press);
    usleep(20000);
    injector_->send_key(KEY_V, KeyState::Press);
    usleep(20000);
    injector_->send_key(KEY_V, KeyState::Release);
    usleep(20000);
    injector_->send_key(KEY_LEFTCTRL, KeyState::Release);
  }

  return true;
}

void EventLoop::action_invert_layout_selection() {
  if (process_selection(
          [](std::string_view text) { return invert_layout(text); })) {
    // Переключаем раскладку после успешной инверсии
    usleep(static_cast<useconds_t>(config_.delays.layout_switch.count()));
    switch_layout();
  }
}

void EventLoop::action_invert_case_selection() {
  process_selection([](std::string_view text) { return invert_case(text); });
}

void EventLoop::action_transliterate_selection() {
  process_selection([](std::string_view text) { return transliterate(text); });
}

} // namespace punto
