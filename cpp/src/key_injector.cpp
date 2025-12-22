/**
 * @file key_injector.cpp
 * @brief Реализация генератора событий ввода
 */

#include "punto/key_injector.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <iostream>

namespace punto {

KeyInjector::KeyInjector(DelayConfig delays) noexcept
    : delays_{std::move(delays)} {}

void KeyInjector::update_delays(const DelayConfig &delays) noexcept {
  delays_ = delays;
}

void KeyInjector::write_all_or_die(int fd, const void *data,
                                  std::size_t bytes) {
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
    std::exit(1);
  }
}

void KeyInjector::emit_events(std::span<const input_event> events) {
  if (events.empty()) {
    return;
  }

  write_all_or_die(STDOUT_FILENO, events.data(),
                   events.size() * sizeof(input_event));
}

void KeyInjector::emit_event(const input_event &ev) {
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
  auto retype_delay = turbo ? delays_.turbo_retype : delays_.retype;

  if (with_shift) {
    send_key(KEY_LEFTSHIFT, KeyState::Press);
    delay(std::chrono::microseconds{10000}); // 10ms hold for modifier
  }

  send_key(code, KeyState::Press);
  delay(std::chrono::microseconds{15000}); // 15ms hold - safe for all apps
  send_key(code, KeyState::Release);

  if (with_shift) {
    delay(std::chrono::microseconds{5000});
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    delay(std::chrono::microseconds{5000});
  }

  delay(retype_delay);
}

void KeyInjector::send_backspace(std::size_t count, bool turbo) const {
  auto retype_delay = turbo ? delays_.turbo_retype : delays_.retype;

  for (std::size_t i = 0; i < count; ++i) {
    send_key(KEY_BACKSPACE, KeyState::Press);
    delay(std::chrono::microseconds{12000}); // 12ms hold for BS
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
  delay(delays_.key_press);

  send_key(modifier, KeyState::Press);
  delay(delays_.key_press);

  send_key(key, KeyState::Press);
  delay(delays_.key_press + std::chrono::microseconds{50000}); // Extended hold

  send_key(key, KeyState::Release);
  delay(delays_.key_press);

  send_key(modifier, KeyState::Release);
  delay(delays_.layout_switch);
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
  delay(delays_.key_press);
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

} // namespace punto
