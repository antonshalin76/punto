/**
 * @file undo_detector.hpp
 * @brief Детектирование отмены коррекции пользователем
 *
 * Если пользователь отменяет коррекцию (Ctrl+Z или быстрый Backspace),
 * запоминаем слово в сессионный список исключений, чтобы не исправлять
 * его повторно.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace punto {

/// Детектор отмены коррекции
class UndoDetector {
public:
  UndoDetector() = default;

  /// Вызывается при применении коррекции
  /// @param task_id ID задачи
  /// @param original_word Исходное слово (ASCII lowercase)
  void on_correction_applied(std::uint64_t task_id,
                             const std::string &original_word) {
    last_correction_ = RecentCorrection{task_id, original_word,
                                        std::chrono::steady_clock::now()};
  }

  /// Вызывается при нажатии Backspace
  /// @param now Текущее время
  /// @return true если это похоже на отмену коррекции
  bool on_backspace(std::chrono::steady_clock::time_point now) {
    if (!last_correction_.has_value()) {
      return false;
    }

    // Проверяем, прошло ли достаточно мало времени с момента коррекции
    const auto elapsed = now - last_correction_->applied_at;
    if (elapsed > kUndoWindow) {
      // Прошло слишком много времени — не считаем это отменой
      last_correction_.reset();
      return false;
    }

    // Увеличиваем счётчик backspace
    ++backspace_count_since_correction_;

    // Если пользователь нажал Backspace несколько раз сразу после коррекции —
    // вероятно, он отменяет её
    if (backspace_count_since_correction_ >= kMinBackspaceCount) {
      // Добавляем слово в исключения сессии
      session_exclusions_.insert(last_correction_->original_word);
      last_correction_.reset();
      backspace_count_since_correction_ = 0;
      return true;
    }

    return false;
  }

  /// Вызывается при нажатии Ctrl+Z
  void on_undo() {
    if (last_correction_.has_value()) {
      // Ctrl+Z сразу после коррекции — добавляем в исключения
      session_exclusions_.insert(last_correction_->original_word);
      last_correction_.reset();
      backspace_count_since_correction_ = 0;
    }
  }

  /// Вызывается при наборе обычной буквы (сбрасывает счётчик backspace)
  void on_key_typed() { backspace_count_since_correction_ = 0; }

  /// Проверяет, находится ли слово в списке исключений сессии
  /// @param word Слово (ASCII lowercase)
  [[nodiscard]] bool is_excluded(const std::string &word) const {
    return session_exclusions_.count(word) > 0;
  }

  /// Количество исключений в сессии
  [[nodiscard]] std::size_t exclusion_count() const noexcept {
    return session_exclusions_.size();
  }

  /// Очистка сессионных исключений
  void clear_exclusions() { session_exclusions_.clear(); }

private:
  struct RecentCorrection {
    std::uint64_t task_id = 0;
    std::string original_word;
    std::chrono::steady_clock::time_point applied_at;
  };

  std::optional<RecentCorrection> last_correction_;
  std::unordered_set<std::string> session_exclusions_;
  std::size_t backspace_count_since_correction_ = 0;

  /// Окно для определения отмены (2 секунды)
  static constexpr auto kUndoWindow = std::chrono::milliseconds(2000);

  /// Минимальное количество Backspace для считывания отмены
  static constexpr std::size_t kMinBackspaceCount = 3;
};

} // namespace punto
