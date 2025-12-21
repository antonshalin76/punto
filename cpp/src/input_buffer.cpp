/**
 * @file input_buffer.cpp
 * @brief Реализация буфера ввода
 */

#include "punto/input_buffer.hpp"
#include "punto/asm_utils.hpp"

#include <algorithm>
#include <cstring>

namespace punto {

bool InputBuffer::push_char(ScanCode code, bool shifted) noexcept {
  if (current_len_ >= kMaxWordLen - 1) {
    return false;
  }

  // Новый символ сбрасывает trailing whitespace
  if (current_len_ == 0) {
    trailing_len_ = 0;
  }

  current_buf_[current_len_] = KeyEntry{code, shifted};
  ++current_len_;
  return true;
}

bool InputBuffer::pop_char() noexcept {
  if (current_len_ > 0) {
    --current_len_;
    return true;
  }
  return false;
}

void InputBuffer::commit_word() noexcept {
  if (current_len_ > 0) {
    // Копируем текущее слово в last_word
    std::copy_n(current_buf_.begin(), current_len_, last_buf_.begin());
    last_len_ = current_len_;
    current_len_ = 0;
    trailing_len_ = 0;
  }
}

void InputBuffer::reset_all() noexcept {
  current_len_ = 0;
  last_len_ = 0;
  trailing_len_ = 0;
}

void InputBuffer::reset_current() noexcept { current_len_ = 0; }

bool InputBuffer::push_trailing(ScanCode code) noexcept {
  if (trailing_len_ >= kMaxWordLen - 1) {
    return false;
  }
  trailing_buf_[trailing_len_] = code;
  ++trailing_len_;
  return true;
}

void InputBuffer::reset_trailing() noexcept { trailing_len_ = 0; }

std::span<const KeyEntry> InputBuffer::get_active_word() const noexcept {
  if (current_len_ > 0) {
    return current_word();
  }
  return last_word();
}

std::span<const KeyEntry> InputBuffer::current_word() const noexcept {
  return std::span{current_buf_.data(), current_len_};
}

std::span<const KeyEntry> InputBuffer::last_word() const noexcept {
  return std::span{last_buf_.data(), last_len_};
}

std::span<const ScanCode> InputBuffer::trailing() const noexcept {
  return std::span{trailing_buf_.data(), trailing_len_};
}

std::size_t InputBuffer::current_length() const noexcept {
  return current_len_;
}

std::size_t InputBuffer::last_length() const noexcept { return last_len_; }

std::size_t InputBuffer::trailing_length() const noexcept {
  return trailing_len_;
}

bool InputBuffer::has_data() const noexcept {
  return current_len_ > 0 || last_len_ > 0;
}

} // namespace punto
