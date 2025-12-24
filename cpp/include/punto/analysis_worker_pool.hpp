/**
 * @file analysis_worker_pool.hpp
 * @brief Пул потоков для асинхронного анализа слов (dict + n-gram + typo fix)
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "punto/concurrent_queue.hpp"
#include "punto/config.hpp"
#include "punto/dictionary.hpp"
#include "punto/layout_analyzer.hpp"
#include "punto/scancode_map.hpp"
#include "punto/types.hpp"
#include "punto/typo_corrector.hpp"

namespace punto {

struct WordTask {
  std::uint64_t task_id = 0;

  // Слово (полное) в KeyEntry (включая пунктуацию, если она набиралась).
  std::vector<KeyEntry> word;

  // Длина части слова для анализа (после отрезания trailing пунктуации).
  std::size_t analysis_len = 0;

  // Раскладка ОС в момент завершения слова (0=EN, 1=RU)
  int layout_at_boundary = 0;

  // Конфиг auto_switch на момент постановки задачи.
  AutoSwitchConfig cfg;

  // Телеметрия
  std::chrono::steady_clock::time_point submitted_at{};
};

/// Тип коррекции, применённой к слову
enum class CorrectionType {
  NoCorrection,   // Коррекция не требуется
  LayoutSwitch,   // Переключение раскладки (EN <-> RU)
  TypoFix,        // Исправление опечатки (перестановка, замена, пропуск, дубль)
  StickyShiftFix, // Исправление залипшего Shift (ПРивет -> Привет)
  CombinedFix     // Комбинированное исправление (раскладка + регистр)
};

struct WordResult {
  std::uint64_t task_id = 0;
  bool need_switch = false;

  /// Тип применённой коррекции
  CorrectionType correction_type = CorrectionType::NoCorrection;

  /// Исправленное слово (если отличается от исходного).
  std::optional<std::vector<KeyEntry>> correction;

  // Телеметрия
  std::size_t word_len = 0;
  std::size_t analysis_len = 0;
  int layout_at_boundary = 0;

  std::uint64_t queue_us = 0;
  std::uint64_t analysis_us = 0;
};

class AnalysisWorkerPool {
public:
  explicit AnalysisWorkerPool(const Dictionary &dict) : dict_{&dict} {}

  AnalysisWorkerPool(const AnalysisWorkerPool &) = delete;
  AnalysisWorkerPool &operator=(const AnalysisWorkerPool &) = delete;

  ~AnalysisWorkerPool() { stop(); }

  void start(std::size_t threads) {
    if (!threads_.empty()) {
      return;
    }

    if (threads == 0) {
      threads = 1;
    }

    threads_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      threads_.emplace_back([this](std::stop_token st) { worker_main(st); });
    }
  }

  void stop() {
    for (auto &t : threads_) {
      t.request_stop();
    }
    tasks_.notify_all();
    threads_.clear();
  }

  void submit(WordTask task) { tasks_.push(std::move(task)); }

  [[nodiscard]] bool try_pop_result(WordResult &out) {
    return results_.try_pop(out);
  }

private:
  void worker_main(std::stop_token st) {
    while (!st.stop_requested()) {
      auto opt = tasks_.pop_wait(st);
      if (!opt.has_value()) {
        break;
      }

      WordTask task = std::move(*opt);

      WordResult res;
      res.task_id = task.task_id;
      res.need_switch = false;
      res.correction_type = CorrectionType::NoCorrection;
      res.word_len = task.word.size();
      res.analysis_len = task.analysis_len;
      res.layout_at_boundary = task.layout_at_boundary;

      const auto t_pop = std::chrono::steady_clock::now();
      if (task.submitted_at.time_since_epoch().count() != 0) {
        res.queue_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                t_pop - task.submitted_at)
                .count());
      }

      const auto t0 = std::chrono::steady_clock::now();

      // Проверяем что слово достаточно длинное
      if (task.analysis_len < task.cfg.min_word_len) {
        res.analysis_us = 0;
        results_.push(std::move(res));
        continue;
      }

      // Берём только анализируемую часть (без trailing пунктуации)
      std::span<const KeyEntry> analysis_span(task.word.data(),
                                              task.analysis_len);

      const bool is_en_layout = (task.layout_at_boundary == 0);

      // =========================================================================
      // Этап 1: Словарная проверка
      // =========================================================================
      DictResult dict_result = dict_->lookup(analysis_span);

      // Если слово найдено в словаре — проверяем sticky shift и layout switch
      if (dict_result == DictResult::English) {
        if (!is_en_layout) {
          // EN слово в RU раскладке -> переключаем
          res.need_switch = true;
          res.correction_type = CorrectionType::LayoutSwitch;
          finish_and_push(res, t0);
          continue;
        }
        // EN слово в EN раскладке -> проверяем sticky shift
        if (task.cfg.sticky_shift_correction_enabled) {
          if (try_sticky_shift_fix(analysis_span, res)) {
            finish_and_push(res, t0);
            continue;
          }
        }
      } else if (dict_result == DictResult::Russian) {
        if (is_en_layout) {
          // RU слово в EN раскладке -> проверяем combined fix или layout switch
          if (task.cfg.sticky_shift_correction_enabled) {
            CasePattern pattern = detect_case_pattern(analysis_span);
            if (pattern == CasePattern::StickyShiftUU ||
                pattern == CasePattern::StickyShiftLU) {
              // Combined: layout switch + case fix
              res.need_switch = true;
              res.correction_type = CorrectionType::CombinedFix;
              res.correction = make_title_case(analysis_span);
              finish_and_push(res, t0);
              continue;
            }
          }
          res.need_switch = true;
          res.correction_type = CorrectionType::LayoutSwitch;
          finish_and_push(res, t0);
          continue;
        }
        // RU слово в RU раскладке -> проверяем sticky shift
        if (task.cfg.sticky_shift_correction_enabled) {
          if (try_sticky_shift_fix(analysis_span, res)) {
            finish_and_push(res, t0);
            continue;
          }
        }
      }

      // =========================================================================
      // Этап 2: Слово Unknown — пробуем typo fix
      // =========================================================================
      if (dict_result == DictResult::Unknown &&
          task.cfg.typo_correction_enabled && dict_->is_hunspell_available()) {

        std::string word_str = keys_to_utf8(analysis_span, is_en_layout);

        if (!word_str.empty() &&
            analysis_span.size() >= task.cfg.min_word_len) {
          // КРИТИЧНО: проверяем что слово действительно неправильное через
          // spell()
          bool is_correct = dict_->spell(word_str, is_en_layout);

          if (is_correct) {
            // Слово правильное, не нужно исправлять (N-gram модель не знала
            // его) Переходим к N-gram анализу для layout switch
          } else {
            // Слово неправильное, запрашиваем предложения
            std::vector<std::string> suggestions =
                dict_->suggest(word_str, is_en_layout, 5);

            for (const auto &suggestion : suggestions) {
              // Проверяем что suggestion - это исправление оригинала
              if (suggestion == word_str) {
                // Слово уже правильное
                break;
              }

              std::size_t distance =
                  damerau_levenshtein_distance(word_str, suggestion);

              // distance=0 означает слово правильное
              if (distance == 0) {
                break;
              }

              if (distance > 0 && distance <= task.cfg.max_typo_diff) {
                // Нашли исправление!
                res.correction_type = CorrectionType::TypoFix;
                res.correction =
                    utf8_to_keys(suggestion, is_en_layout, true, analysis_span);
                finish_and_push(res, t0);
                continue; // Это continue для внешнего while, но мы в for...
              }
            }

            // Если нашли typo fix, уже сделали continue выше
            if (res.correction_type == CorrectionType::TypoFix) {
              continue;
            }
          }
        }
      }

      // =========================================================================
      // Этап 3: N-gram анализ для layout switch (если слово Unknown)
      // =========================================================================
      if (dict_result == DictResult::Unknown) {
        LayoutAnalyzer analyzer(task.cfg);
        AnalysisResult ar = analyzer.analyze(analysis_span);

        if (ar.should_switch) {
          bool ngram_suggests_en =
              ar.should_switch && ar.en_score > ar.ru_score;
          bool looks_like_valid_en = (ar.en_invalid_count == 0);
          bool invalid_suggests_en =
              (ar.ru_invalid_count > 0 && ar.en_invalid_count == 0);

          if ((ngram_suggests_en && looks_like_valid_en) ||
              invalid_suggests_en) {
            res.need_switch = true;
            res.correction_type = CorrectionType::LayoutSwitch;
          }
        }
      }

      finish_and_push(res, t0);
    }
  }

  // Helper: завершить анализ и положить результат в очередь
  void finish_and_push(WordResult &res,
                       std::chrono::steady_clock::time_point t0) {
    const auto t1 = std::chrono::steady_clock::now();
    res.analysis_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    results_.push(std::move(res));
  }

  // Helper: проверить и применить sticky shift fix
  bool try_sticky_shift_fix(std::span<const KeyEntry> span, WordResult &res) {
    CasePattern pattern = detect_case_pattern(span);
    if (pattern == CasePattern::StickyShiftUU ||
        pattern == CasePattern::StickyShiftLU) {
      res.need_switch = false;
      res.correction_type = CorrectionType::StickyShiftFix;
      res.correction = make_title_case(span);
      return true;
    }
    return false;
  }

  // Helper: создать Title Case версию слова
  std::vector<KeyEntry> make_title_case(std::span<const KeyEntry> span) {
    std::vector<KeyEntry> corrected;
    corrected.reserve(span.size());

    bool first_letter = true;
    for (const auto &entry : span) {
      if (is_typeable_letter(entry.code)) {
        corrected.emplace_back(entry.code, first_letter);
        first_letter = false;
      } else {
        corrected.push_back(entry);
      }
    }
    return corrected;
  }

  const Dictionary *dict_ = nullptr;

  ConcurrentQueue<WordTask> tasks_;
  ConcurrentQueue<WordResult> results_;

  std::vector<std::jthread> threads_;
};

} // namespace punto
