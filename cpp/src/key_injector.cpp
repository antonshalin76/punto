/**
 * @file key_injector.cpp
 * @brief Реализация генератора событий ввода
 */

#include "punto/key_injector.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <unistd.h>

#include <iostream>

namespace punto {

KeyInjector::KeyInjector() noexcept = default;

void KeyInjector::write_all(int fd, const void *data, std::size_t bytes) const {
  const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
  std::size_t remaining = bytes;

  while (remaining > 0) {
    ssize_t n = ::write(fd, p, remaining);
    if (n > 0) {
      p += static_cast<std::size_t>(n);
      remaining -= static_cast<std::size_t>(n);
      continue;
    }

    if (n < 0 && errno == EINTR) {
      continue;
    }

    const int e = errno;
    std::cerr << "[punto] KeyInjector: write failed (fd=" << fd
              << " bytes=" << bytes << " remaining=" << remaining
              << ") errno=" << e << " (" << std::strerror(e) << ")\n";
    fatal_io_errno_.store(e, std::memory_order_relaxed);
    fatal_io_error_.store(true, std::memory_order_release);
    return;
  }
}

void KeyInjector::emit_events(std::span<const input_event> events) const {
  if (events.empty()) {
    return;
  }

  if (fatal_io_error_.load(std::memory_order_acquire)) {
    return;
  }

  write_all(STDOUT_FILENO, events.data(), events.size() * sizeof(input_event));
}

void KeyInjector::emit_event(const input_event &ev) const {
  emit_events(std::span<const input_event>{&ev, 1});
}

void KeyInjector::send_key(ScanCode code, KeyState state) const {
  input_event evs[2]{};

  evs[0].type = EV_KEY;
  evs[0].code = code;
  evs[0].value = static_cast<std::int32_t>(state);

  evs[1].type = EV_SYN;
  evs[1].code = SYN_REPORT;
  evs[1].value = 0;

  emit_events(std::span<const input_event>{evs, 2});
}

void KeyInjector::tap_key(ScanCode code, bool with_shift, bool turbo) const {
  auto retype_delay = turbo ? kTurboRetype : kRetype;

  if (with_shift) {
    send_key(KEY_LEFTSHIFT, KeyState::Press);
    delay(kModifierHold);
  }

  send_key(code, KeyState::Press);
  delay(kKeyHold);
  send_key(code, KeyState::Release);

  if (with_shift) {
    delay(kModifierRelease);
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    delay(kModifierRelease);
  }

  delay(retype_delay);
}

void KeyInjector::send_backspace(std::size_t count, bool turbo) const {
  auto retype_delay = turbo ? kTurboRetype : kRetype;

  for (std::size_t i = 0; i < count; ++i) {
    send_key(KEY_BACKSPACE, KeyState::Press);
    delay(kBackspaceHold);
    send_key(KEY_BACKSPACE, KeyState::Release);

    if (i < count - 1) {
      delay(retype_delay / 2);
    }
  }
}

void KeyInjector::retype_buffer(std::span<const KeyEntry> entries,
                                bool turbo) const {
  for (const auto &entry : entries) {
    tap_key(entry.code, entry.shifted, turbo);
  }
}

void KeyInjector::retype_trailing(std::span<const ScanCode> codes,
                                  bool turbo) const {
  // Для некоторых приложений (в т.ч. терминалов) слишком быстрый press/release
  // может приводить к потере пробелов/таба. Используем tap_key с hold time.
  for (const auto code : codes) {
    tap_key(code, false, turbo);
  }
}

void KeyInjector::send_layout_hotkey(ScanCode modifier, ScanCode key) const {
  delay(kKeyPress);

  send_key(modifier, KeyState::Press);
  delay(kKeyPress);

  send_key(key, KeyState::Press);
  delay(kKeyPress + std::chrono::microseconds{50000}); // Extended hold

  send_key(key, KeyState::Release);
  delay(kKeyPress);

  send_key(modifier, KeyState::Release);
  delay(kLayoutSwitch);
}

void KeyInjector::send_paste(bool is_terminal) const {
  // Важно: предполагаем, что вызывающий код уже отпустил "чужие" модификаторы.
  delay(kKeyPress);

  if (is_terminal) {
    // Терминалы (включая некоторые встроенные/кастомные) часто используют
    // Ctrl+Shift+V для paste.
    send_key(KEY_LEFTCTRL, KeyState::Press);
    send_key(KEY_LEFTSHIFT, KeyState::Press);
    delay(kKeyPress);

    send_key(KEY_V, KeyState::Press);
    delay(kKeyHold);
    send_key(KEY_V, KeyState::Release);

    delay(kKeyPress);
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    send_key(KEY_LEFTCTRL, KeyState::Release);
    delay(kKeyPress);
    return;
  }

  // Для обычных приложений и терминалов внутри IDE (которые не детектируются как
  // терминал по WM_CLASS) Shift+Insert — более универсальный paste hotkey.
  send_key(KEY_LEFTSHIFT, KeyState::Press);
  delay(kKeyPress);

  send_key(KEY_INSERT, KeyState::Press);
  delay(kKeyHold);
  send_key(KEY_INSERT, KeyState::Release);

  delay(kKeyPress);

  send_key(KEY_LEFTSHIFT, KeyState::Release);
  delay(kKeyPress);
}

void KeyInjector::release_all_modifiers() const {
  // Отпускаем все модификаторы для предотвращения interference
  send_key(KEY_LEFTSHIFT, KeyState::Release);
  send_key(KEY_RIGHTSHIFT, KeyState::Release);
  send_key(KEY_LEFTCTRL, KeyState::Release);
  send_key(KEY_RIGHTCTRL, KeyState::Release);
  send_key(KEY_LEFTALT, KeyState::Release);
  send_key(KEY_RIGHTALT, KeyState::Release);
  send_key(KEY_LEFTMETA, KeyState::Release);
  send_key(KEY_RIGHTMETA, KeyState::Release);
  delay(kKeyPress);
}

void KeyInjector::delay(std::chrono::microseconds us) const noexcept {
  if (us.count() <= 0)
    return;

  if (wait_func_) {
    wait_func_(us);
  } else {
    usleep(static_cast<useconds_t>(us.count()));
  }
}

bool KeyInjector::has_fatal_io_error() const noexcept {
  return fatal_io_error_.load(std::memory_order_acquire);
}

int KeyInjector::fatal_io_errno() const noexcept {
  return fatal_io_errno_.load(std::memory_order_relaxed);
}

void KeyInjector::clear_fatal_io_error() const noexcept {
  fatal_io_errno_.store(0, std::memory_order_relaxed);
  fatal_io_error_.store(false, std::memory_order_release);
}

} // namespace punto
