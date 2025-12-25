/**
 * @file history_manager.hpp
 * @brief История набранного текста (последние N слов) для безопасного
 * rollback/replay
 *
 * Хранит поток "токенов" на уровне KeyEntry (код клавиши + shifted),
 * а также метаданные последних слов (координаты в этом потоке).
 *
 * Важно:
 * - История отражает то, что было ПРОПУЩЕНО в stdout (т.е. применено к тексту).
 * - Инжектируемые макросы (backspace/retype) НЕ должны повторно пушить токены в
 * историю.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "punto/types.hpp"

namespace punto {

struct HistoryWord {
  std::uint64_t task_id = 0;

  // Позиции в потоке токенов [base_pos, cursor_pos)
  std::uint64_t start_pos = 0; // inclusive
  std::uint64_t end_pos = 0;   // exclusive (конец слова, перед разделителем)

  // Разделитель (space/tab) является отдельным токеном.
  std::uint64_t delim_pos = 0; // позиция разделителя
  ScanCode delimiter = 0;      // KEY_SPACE / KEY_TAB

  // Определённый язык слова для контекстного окна
  // 0 = Unknown, 1 = English, 2 = Russian
  std::uint8_t detected_language = 0;

  [[nodiscard]] constexpr std::size_t word_len() const noexcept {
    return (end_pos >= start_pos)
               ? static_cast<std::size_t>(end_pos - start_pos)
               : 0U;
  }

  [[nodiscard]] constexpr std::size_t total_len_with_delim() const noexcept {
    // +1 за разделитель
    return word_len() + 1U;
  }
};

class HistoryManager {
public:
  explicit HistoryManager(std::size_t max_words)
      : max_words_{max_words == 0 ? 1U : max_words} {
    resize_capacity();
  }

  void set_max_words(std::size_t max_words) {
    max_words_ = (max_words == 0 ? 1U : max_words);
    resize_capacity();
    trim_words_to_capacity();
  }

  [[nodiscard]] std::size_t max_words() const noexcept { return max_words_; }

  [[nodiscard]] std::uint64_t base_pos() const noexcept { return base_pos_; }
  [[nodiscard]] std::uint64_t cursor_pos() const noexcept {
    return cursor_pos_;
  }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  void reset() {
    head_ = 0;
    size_ = 0;
    base_pos_ = 0;
    cursor_pos_ = 0;
    words_.clear();
  }

  void push_token(KeyEntry entry) {
    if (capacity_ == 0) {
      return;
    }

    if (size_ < capacity_) {
      const std::size_t idx = (head_ + size_) % capacity_;
      tokens_[idx] = entry;
      ++size_;
      ++cursor_pos_;
    } else {
      // Переполнение: выкидываем самый старый токен, перезаписывая его.
      tokens_[head_] = entry;
      head_ = (head_ + 1) % capacity_;
      ++base_pos_;
      ++cursor_pos_;

      // Сбрасываем метаданные слов, которые выпали из окна.
      while (!words_.empty() && words_.front().start_pos < base_pos_) {
        words_.pop_front();
      }
    }
  }

  bool pop_token() {
    if (size_ == 0) {
      return false;
    }

    // Удаляем последний токен (backspace).
    --size_;
    --cursor_pos_;

    // Если backspace задел разделитель/слово из истории — удаляем последнее
    // слово.
    while (!words_.empty()) {
      const HistoryWord &w = words_.back();
      // delim_pos указывает на позицию разделителя. Если cursor_pos <=
      // delim_pos, значит разделитель был удалён (или мы откатились дальше).
      if (cursor_pos_ <= w.delim_pos) {
        words_.pop_back();
        continue;
      }
      // Если откатились внутрь слова, тоже снимаем его метаданные.
      if (cursor_pos_ <= w.end_pos) {
        words_.pop_back();
        continue;
      }
      break;
    }

    return true;
  }

  /// Коммитит слово при нажатии разделителя (space/tab).
  ///
  /// Требования к вызову:
  /// - разделитель уже должен быть добавлен в историю через push_token().
  /// - word_len — длина слова в KeyEntry (без разделителя), соответствующая
  ///   токенам, которые уже находятся в истории.
  [[nodiscard]] std::optional<HistoryWord>
  commit_word(std::uint64_t task_id, std::size_t word_len, ScanCode delimiter) {
    if (word_len == 0) {
      return std::nullopt;
    }
    if (cursor_pos_ == 0) {
      return std::nullopt;
    }

    // delimiter — последний добавленный токен.
    const std::uint64_t delim_pos = cursor_pos_ - 1;
    const std::uint64_t end_pos = delim_pos; // конец слова = перед разделителем

    if (end_pos < word_len) {
      return std::nullopt;
    }

    const std::uint64_t start_pos = end_pos - word_len;

    // Если слово вышло за окно — не коммитим (fail-fast).
    if (start_pos < base_pos_) {
      return std::nullopt;
    }

    HistoryWord w;
    w.task_id = task_id;
    w.start_pos = start_pos;
    w.end_pos = end_pos;
    w.delim_pos = delim_pos;
    w.delimiter = delimiter;

    words_.push_back(w);
    trim_words_to_capacity();

    return w;
  }

  /// Возвращает копию токенов в диапазоне [from_pos, to_pos).
  [[nodiscard]] bool get_range(std::uint64_t from_pos, std::uint64_t to_pos,
                               std::vector<KeyEntry> &out) const {
    out.clear();

    if (from_pos > to_pos) {
      return false;
    }
    if (from_pos < base_pos_ || to_pos > cursor_pos_) {
      return false;
    }

    const std::uint64_t len64 = to_pos - from_pos;
    const std::size_t len = static_cast<std::size_t>(len64);
    out.reserve(len);

    const std::uint64_t start_off = from_pos - base_pos_;
    for (std::size_t i = 0; i < len; ++i) {
      const std::size_t idx =
          (head_ + static_cast<std::size_t>(start_off) + i) % capacity_;
      out.push_back(tokens_[idx]);
    }

    return true;
  }

  /// Возвращает преобладающий язык в последних N словах (контекстное окно)
  /// @param window_size Размер окна (количество последних слов)
  /// @return 0 = Unknown/Mixed, 1 = English, 2 = Russian
  [[nodiscard]] std::uint8_t
  get_context_language(std::size_t window_size = 3) const {
    std::size_t en_count = 0;
    std::size_t ru_count = 0;

    const std::size_t words_count = words_.size();
    const std::size_t start_idx =
        (words_count > window_size) ? words_count - window_size : 0;

    for (std::size_t i = start_idx; i < words_count; ++i) {
      if (words_[i].detected_language == 1) {
        ++en_count;
      } else if (words_[i].detected_language == 2) {
        ++ru_count;
      }
    }

    // Возвращаем преобладающий язык если все слова одного языка
    if (en_count > 0 && ru_count == 0) {
      return 1; // English
    }
    if (ru_count > 0 && en_count == 0) {
      return 2; // Russian
    }

    return 0; // Mixed или Unknown
  }

  /// Обновляет detected_language для последнего слова
  void update_last_word_language(std::uint8_t lang) {
    if (!words_.empty()) {
      words_.back().detected_language = lang;
    }
  }

private:
  void resize_capacity() {
    // Храним max_words + текущий набираемый + запас.
    // kMaxWordLen = 256, поэтому даже max_words=50 остаётся умеренным.
    const std::size_t desired = (max_words_ + 2U) * (kMaxWordLen + 1U);

    capacity_ = desired;
    tokens_.assign(capacity_, KeyEntry{});

    // При изменении capacity проще сбросить, чтобы не получить рассинхрон.
    head_ = 0;
    size_ = 0;
    base_pos_ = 0;
    cursor_pos_ = 0;
    words_.clear();
  }

  void trim_words_to_capacity() {
    while (words_.size() > max_words_) {
      words_.pop_front();
    }
  }

  std::size_t max_words_ = 5;

  // Кольцевой буфер токенов.
  std::vector<KeyEntry> tokens_;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0;
  std::size_t size_ = 0;

  std::uint64_t base_pos_ = 0;
  std::uint64_t cursor_pos_ = 0;

  // Последние слова в порядке коммита.
  std::deque<HistoryWord> words_;
};

} // namespace punto
