/**
 * @file undo_detector.hpp
 * @brief Детектирование отмены коррекции пользователем
 *
 * Если пользователь отменяет коррекцию (Ctrl+Z или быстрый Backspace),
 * запоминаем слово в ПОСТОЯННЫЙ список исключений, который сохраняется
 * между сессиями для дообучения системы.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>

namespace punto {

/// Путь к файлу персистентных исключений по умолчанию
inline constexpr const char *kDefaultExclusionsPath =
    "/etc/punto/undo_exclusions.txt";

/// Детектор отмены коррекции с персистентным хранением
class UndoDetector {
public:
  explicit UndoDetector(const std::string &path = kDefaultExclusionsPath)
      : file_path_{path} {
    load_from_file();
  }

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
      // Добавляем слово в исключения и СОХРАНЯЕМ на диск
      add_exclusion(last_correction_->original_word);
      last_correction_.reset();
      backspace_count_since_correction_ = 0;
      return true;
    }

    return false;
  }

  /// Вызывается при нажатии Ctrl+Z
  void on_undo() {
    if (last_correction_.has_value()) {
      // Ctrl+Z сразу после коррекции — добавляем в исключения и сохраняем
      add_exclusion(last_correction_->original_word);
      last_correction_.reset();
      backspace_count_since_correction_ = 0;
    }
  }

  /// Вызывается при наборе обычной буквы (сбрасывает счётчик backspace)
  void on_key_typed() { backspace_count_since_correction_ = 0; }

  /// Проверяет, находится ли слово в списке исключений
  /// @param word Слово (ASCII lowercase)
  [[nodiscard]] bool is_excluded(const std::string &word) const {
    return exclusions_.count(word) > 0;
  }

  /// Количество исключений
  [[nodiscard]] std::size_t exclusion_count() const noexcept {
    return exclusions_.size();
  }

  /// Очистка всех исключений (включая файл)
  void clear_exclusions() {
    exclusions_.clear();
    save_to_file();
  }

  /// Загрузка исключений из файла
  void load_from_file() {
    std::ifstream file(file_path_);
    if (!file.is_open()) {
      // Файл не существует — это нормально при первом запуске
      return;
    }

    std::string word;
    std::size_t count = 0;
    while (std::getline(file, word)) {
      // Пропускаем пустые строки и комментарии
      if (word.empty() || word[0] == '#') {
        continue;
      }
      // Убираем пробелы
      while (!word.empty() && (word.back() == '\r' || word.back() == '\n' ||
                               word.back() == ' ')) {
        word.pop_back();
      }
      if (!word.empty()) {
        exclusions_.insert(word);
        ++count;
      }
    }

    if (count > 0) {
      std::cerr << "[punto] Loaded " << count
                << " persistent undo exclusions from " << file_path_ << "\n";
    }
  }

  /// Сохранение исключений в файл
  void save_to_file() const {
    std::ofstream file(file_path_);
    if (!file.is_open()) {
      std::cerr << "[punto] Warning: cannot save undo exclusions to "
                << file_path_ << "\n";
      return;
    }

    file << "# Punto Switcher Undo Exclusions\n";
    file << "# Слова, которые пользователь отменял после автокоррекции\n";
    file << "# Файл обновляется автоматически\n";
    file << "\n";

    for (const auto &word : exclusions_) {
      file << word << "\n";
    }

    std::cerr << "[punto] Saved " << exclusions_.size()
              << " undo exclusions to " << file_path_ << "\n";
  }

  /// Ручное добавление слова в исключения (и сохранение на диск)
  void add_exclusion(const std::string &word) {
    if (word.empty()) {
      return;
    }
    auto [it, inserted] = exclusions_.insert(word);
    if (inserted) {
      // Инкрементальное сохранение: append в конец файла
      std::ofstream file(file_path_, std::ios::app);
      if (file.is_open()) {
        file << word << "\n";
      }
      std::cerr << "[punto] Added undo exclusion: " << word << "\n";
    }
  }

private:
  struct RecentCorrection {
    std::uint64_t task_id = 0;
    std::string original_word;
    std::chrono::steady_clock::time_point applied_at;
  };

  std::string file_path_;
  std::optional<RecentCorrection> last_correction_;
  std::unordered_set<std::string> exclusions_;
  std::size_t backspace_count_since_correction_ = 0;

  /// Окно для определения отмены (2 секунды)
  static constexpr auto kUndoWindow = std::chrono::milliseconds(2000);

  /// Минимальное количество Backspace для считывания отмены
  static constexpr std::size_t kMinBackspaceCount = 3;
};

} // namespace punto
