/**
 * @file key_injector.cpp
 * @brief Реализация генератора событий ввода
 */

#include "punto/key_injector.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <unistd.h>

#include <iostream>

namespace punto {

namespace {

[[nodiscard]] constexpr InjectionResult
output_failure(bool action_dispatched) noexcept {
  return action_dispatched ? InjectionResult::OutputFailedAfterAction
                           : InjectionResult::OutputFailedBeforeAction;
}

} // namespace

KeyInjector::KeyInjector() noexcept {
  const int flags = ::fcntl(STDOUT_FILENO, F_GETFL);
  if (flags < 0 || (!(flags & O_NONBLOCK) &&
                    ::fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK) != 0)) {
    fatal_io_errno_.store(errno, std::memory_order_relaxed);
    fatal_io_error_.store(true, std::memory_order_release);
  }
}

void KeyInjector::latch_io_error(int error, int fd, std::size_t bytes,
                                 std::size_t remaining) const {
  std::cerr << "[punto] KeyInjector: write failed (fd=" << fd
            << " bytes=" << bytes << " remaining=" << remaining
            << ") errno=" << error << " (" << std::strerror(error) << ")\n";
  fatal_io_errno_.store(error, std::memory_order_relaxed);
  fatal_io_error_.store(true, std::memory_order_release);
}

void KeyInjector::write_all(int fd, const void *data, std::size_t bytes) const {
  const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
  std::size_t remaining = bytes;
  const auto deadline = std::chrono::steady_clock::now() + kOutputWriteTimeout;
  constexpr std::size_t kAtomicPipeChunk = 512U;

  while (remaining > 0) {
    const std::size_t chunk = std::min(remaining, kAtomicPipeChunk);
    const ssize_t n = ::write(fd, p, chunk);
    if (n > 0) {
      p += static_cast<std::size_t>(n);
      remaining -= static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      const int error = n == 0 ? EIO : errno;
      latch_io_error(error, fd, bytes, remaining);
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      latch_io_error(ETIMEDOUT, fd, bytes, remaining);
      return;
    }

    const auto remaining_time = deadline - now;
    auto timeout =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining_time);
    if (timeout < remaining_time) {
      timeout += std::chrono::milliseconds{1};
    }
    const auto bounded_timeout = std::min<std::int64_t>(
        timeout.count(), std::numeric_limits<int>::max());
    pollfd descriptor{fd, POLLOUT, 0};
    const int poll_result =
        ::poll(&descriptor, 1, static_cast<int>(bounded_timeout));
    if (poll_result == 0) {
      latch_io_error(ETIMEDOUT, fd, bytes, remaining);
      return;
    }
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      latch_io_error(errno, fd, bytes, remaining);
      return;
    }
    if (descriptor.revents & POLLNVAL) {
      latch_io_error(EBADF, fd, bytes, remaining);
      return;
    }
    if (descriptor.revents & (POLLERR | POLLHUP)) {
      latch_io_error(EPIPE, fd, bytes, remaining);
      return;
    }
    if (!(descriptor.revents & POLLOUT)) {
      continue;
    }
  }
}

void KeyInjector::emit_events(std::span<const input_event> events) const {
  if (events.empty()) {
    return;
  }

  if (fatal_io_error_.load(std::memory_order_acquire)) {
    return;
  }

  if (events.size() >
      std::numeric_limits<std::size_t>::max() / sizeof(input_event)) {
    latch_io_error(EOVERFLOW, STDOUT_FILENO, 0, 0);
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

InjectionResult KeyInjector::tap_key(ScanCode code, bool with_shift,
                                     bool turbo) const {
  if (has_fatal_io_error()) {
    return InjectionResult::OutputFailedBeforeAction;
  }
  auto retype_delay = turbo ? kTurboRetype : kRetype;
  bool action_dispatched = false;

  if (with_shift) {
    send_key(KEY_LEFTSHIFT, KeyState::Press);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    if (!delay(kModifierHold)) {
      send_key(KEY_LEFTSHIFT, KeyState::Release);
      if (has_fatal_io_error()) {
        return output_failure(action_dispatched);
      }
      return InjectionResult::CancelledBeforeAction;
    }
  }

  send_key(code, KeyState::Press);
  if (has_fatal_io_error()) {
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    return output_failure(action_dispatched);
  }
  action_dispatched = true;
  if (!delay(kKeyHold)) {
    send_key(code, KeyState::Release);
    if (with_shift) {
      send_key(KEY_LEFTSHIFT, KeyState::Release);
    }
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }
  send_key(code, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }

  if (with_shift) {
    if (!delay(kModifierRelease)) {
      send_key(KEY_LEFTSHIFT, KeyState::Release);
      if (has_fatal_io_error()) {
        return output_failure(action_dispatched);
      }
      return InjectionResult::CancelledAfterAction;
    }
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    if (!delay(kModifierRelease)) {
      return InjectionResult::CancelledAfterAction;
    }
  }

  const bool completed = delay(retype_delay);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  return completed ? InjectionResult::Completed
                   : InjectionResult::CancelledAfterAction;
}

void KeyInjector::send_backspace(std::size_t count, bool turbo) const {
  auto retype_delay = turbo ? kTurboRetype : kRetype;

  for (std::size_t i = 0; i < count; ++i) {
    send_key(KEY_BACKSPACE, KeyState::Press);
    (void)delay(kBackspaceHold);
    send_key(KEY_BACKSPACE, KeyState::Release);

    if (i < count - 1) {
      (void)delay(retype_delay / 2);
    }
  }
}

void KeyInjector::retype_buffer(std::span<const KeyEntry> entries,
                                bool turbo) const {
  for (const auto &entry : entries) {
    (void)tap_key(entry.code, entry.shifted, turbo);
  }
}

void KeyInjector::retype_trailing(std::span<const ScanCode> codes,
                                  bool turbo) const {
  // Для некоторых приложений (в т.ч. терминалов) слишком быстрый press/release
  // может приводить к потере пробелов/таба. Используем tap_key с hold time.
  for (const auto code : codes) {
    (void)tap_key(code, false, turbo);
  }
}

InjectionResult KeyInjector::send_layout_hotkey(ScanCode modifier,
                                                ScanCode key) const {
  if (has_fatal_io_error()) {
    return InjectionResult::OutputFailedBeforeAction;
  }
  bool action_dispatched = false;
  if (!delay(kKeyPress)) {
    return InjectionResult::CancelledBeforeAction;
  }

  send_key(modifier, KeyState::Press);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(modifier, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledBeforeAction;
  }

  send_key(key, KeyState::Press);
  if (has_fatal_io_error()) {
    send_key(modifier, KeyState::Release);
    return output_failure(action_dispatched);
  }
  action_dispatched = true;
  if (!delay(kKeyPress + std::chrono::microseconds{50000})) {
    send_key(key, KeyState::Release);
    send_key(modifier, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }

  send_key(key, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(modifier, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }

  send_key(modifier, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  const bool completed = delay(kLayoutSwitch);
  return completed ? InjectionResult::Completed
                   : InjectionResult::CancelledAfterAction;
}

InjectionResult KeyInjector::send_paste(bool is_terminal) const {
  if (has_fatal_io_error()) {
    return InjectionResult::OutputFailedBeforeAction;
  }
  bool action_dispatched = false;
  // Важно: предполагаем, что вызывающий код уже отпустил "чужие" модификаторы.
  if (!delay(kKeyPress)) {
    return InjectionResult::CancelledBeforeAction;
  }

  if (is_terminal) {
    // Терминалы (включая некоторые встроенные/кастомные) часто используют
    // Ctrl+Shift+V для paste.
    send_key(KEY_LEFTCTRL, KeyState::Press);
    send_key(KEY_LEFTSHIFT, KeyState::Press);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    if (!delay(kKeyPress)) {
      send_key(KEY_LEFTSHIFT, KeyState::Release);
      send_key(KEY_LEFTCTRL, KeyState::Release);
      if (has_fatal_io_error()) {
        return output_failure(action_dispatched);
      }
      return InjectionResult::CancelledBeforeAction;
    }

    send_key(KEY_V, KeyState::Press);
    if (has_fatal_io_error()) {
      send_key(KEY_LEFTSHIFT, KeyState::Release);
      send_key(KEY_LEFTCTRL, KeyState::Release);
      return output_failure(action_dispatched);
    }
    action_dispatched = true;
    if (!delay(kKeyHold)) {
      send_key(KEY_V, KeyState::Release);
      send_key(KEY_LEFTSHIFT, KeyState::Release);
      send_key(KEY_LEFTCTRL, KeyState::Release);
      if (has_fatal_io_error()) {
        return output_failure(action_dispatched);
      }
      return InjectionResult::CancelledAfterAction;
    }
    send_key(KEY_V, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }

    if (!delay(kKeyPress)) {
      send_key(KEY_LEFTSHIFT, KeyState::Release);
      send_key(KEY_LEFTCTRL, KeyState::Release);
      if (has_fatal_io_error()) {
        return output_failure(action_dispatched);
      }
      return InjectionResult::CancelledAfterAction;
    }
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    const bool completed = delay(kKeyPress);
    return completed ? InjectionResult::Completed
                     : InjectionResult::CancelledAfterAction;
  }

  // Для обычных приложений и терминалов внутри IDE (которые не детектируются
  // как терминал по WM_CLASS) Shift+Insert — более универсальный paste hotkey.
  send_key(KEY_LEFTSHIFT, KeyState::Press);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledBeforeAction;
  }

  send_key(KEY_INSERT, KeyState::Press);
  if (has_fatal_io_error()) {
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    return output_failure(action_dispatched);
  }
  action_dispatched = true;
  if (!delay(kKeyHold)) {
    send_key(KEY_INSERT, KeyState::Release);
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }
  send_key(KEY_INSERT, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }

  if (!delay(kKeyPress)) {
    send_key(KEY_LEFTSHIFT, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }

  send_key(KEY_LEFTSHIFT, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  const bool completed = delay(kKeyPress);
  return completed ? InjectionResult::Completed
                   : InjectionResult::CancelledAfterAction;
}

InjectionResult KeyInjector::send_clipboard_paste() const {
  if (has_fatal_io_error()) {
    return InjectionResult::OutputFailedBeforeAction;
  }
  bool action_dispatched = false;
  if (!delay(kKeyPress)) {
    return InjectionResult::CancelledBeforeAction;
  }
  send_key(KEY_LEFTCTRL, KeyState::Press);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledBeforeAction;
  }
  send_key(KEY_V, KeyState::Press);
  if (has_fatal_io_error()) {
    send_key(KEY_LEFTCTRL, KeyState::Release);
    return output_failure(action_dispatched);
  }
  action_dispatched = true;
  if (!delay(kKeyHold)) {
    send_key(KEY_V, KeyState::Release);
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }
  send_key(KEY_V, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }
  send_key(KEY_LEFTCTRL, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  const bool completed = delay(kKeyPress);
  return completed ? InjectionResult::Completed
                   : InjectionResult::CancelledAfterAction;
}

InjectionResult KeyInjector::send_clipboard_copy() const {
  if (has_fatal_io_error()) {
    return InjectionResult::OutputFailedBeforeAction;
  }
  bool action_dispatched = false;
  if (!delay(kKeyPress)) {
    return InjectionResult::CancelledBeforeAction;
  }
  send_key(KEY_LEFTCTRL, KeyState::Press);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledBeforeAction;
  }
  send_key(KEY_C, KeyState::Press);
  if (has_fatal_io_error()) {
    send_key(KEY_LEFTCTRL, KeyState::Release);
    return output_failure(action_dispatched);
  }
  action_dispatched = true;
  if (!delay(kKeyHold)) {
    send_key(KEY_C, KeyState::Release);
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }
  send_key(KEY_C, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  if (!delay(kKeyPress)) {
    send_key(KEY_LEFTCTRL, KeyState::Release);
    if (has_fatal_io_error()) {
      return output_failure(action_dispatched);
    }
    return InjectionResult::CancelledAfterAction;
  }
  send_key(KEY_LEFTCTRL, KeyState::Release);
  if (has_fatal_io_error()) {
    return output_failure(action_dispatched);
  }
  const bool completed = delay(kKeyPress);
  return completed ? InjectionResult::Completed
                   : InjectionResult::CancelledAfterAction;
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
  (void)delay(kKeyPress);
}

bool KeyInjector::delay(std::chrono::microseconds us) const noexcept {
  if (us.count() <= 0) {
    return true;
  }

  if (wait_func_) {
    return wait_func_(us);
  } else {
    return usleep(static_cast<useconds_t>(us.count())) == 0;
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
