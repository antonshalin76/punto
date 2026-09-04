/**
 * @file analysis_worker_pool.hpp
 * @brief Пул потоков для асинхронного анализа слов (dict + n-gram + typo fix)
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "punto/concurrent_queue.hpp"
#include "punto/config.hpp"
#include "punto/dictionary.hpp"
#include "punto/layout_analyzer.hpp"
#include "punto/scancode_map.hpp"
#include "punto/smart_bypass.hpp"
#include "punto/types.hpp"
#include "punto/typo_corrector.hpp"

namespace punto {

struct WordTask {
  std::uint64_t task_id = 0;
  std::uint64_t epoch = 0;

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

struct AnalysisAdmission {
  bool accepted = false;
  std::uint64_t epoch = 0;
  std::chrono::steady_clock::time_point accepted_at{};

  [[nodiscard]] explicit operator bool() const noexcept { return accepted; }
};

/// Тип распознанной коррекции. EventLoop v2.8.6 учитывает её только в
/// телеметрии.
enum class CorrectionType {
  NoCorrection, // Коррекция не требуется
  LayoutSwitch, // Переключение раскладки (EN <-> RU)
  TypoFix, // Исправление опечатки (перестановка, замена, пропуск, дубль)
  StickyShiftFix, // Исправление залипшего Shift (ПРивет -> Привет)
  CombinedFix // Комбинированное исправление (раскладка + регистр)
};

enum class WordTerminalStatus { Completed, Failed, Cancelled };

struct WordResult {
  std::uint64_t task_id = 0;
  std::uint64_t epoch = 0;
  WordTerminalStatus terminal_status = WordTerminalStatus::Completed;
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
  static constexpr std::size_t kDefaultMaxOutstandingTasks = 1024;

  explicit AnalysisWorkerPool(
      const Dictionary &dict, std::function<void()> before_task = {},
      std::function<void()> on_stop_wait = {},
      std::size_t max_outstanding_tasks = kDefaultMaxOutstandingTasks)
      : dict_{&dict}, before_task_{std::move(before_task)},
        on_stop_wait_{std::move(on_stop_wait)},
        max_outstanding_tasks_{
            std::max<std::size_t>(max_outstanding_tasks, 1)} {}

  AnalysisWorkerPool(const AnalysisWorkerPool &) = delete;
  AnalysisWorkerPool &operator=(const AnalysisWorkerPool &) = delete;

  ~AnalysisWorkerPool() { stop(); }

  void start(std::size_t threads) {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    if (!threads_.empty()) {
      return;
    }

    if (lifecycle_ != Lifecycle::Running || fatal_) {
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

  void close_admission() noexcept {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    accepting_ = false;
  }

  void stop() {
    {
      std::unique_lock<std::mutex> state_lock(state_mu_);
      const auto caller = std::this_thread::get_id();
      for (const auto &thread : threads_) {
        if (thread.get_id() == caller) {
          throw std::logic_error(
              "AnalysisWorkerPool::stop cannot run on a worker thread");
        }
      }
      if (lifecycle_ == Lifecycle::Stopped) {
        return;
      }
      if (lifecycle_ == Lifecycle::Stopping) {
        const auto on_stop_wait = on_stop_wait_;
        state_lock.unlock();
        if (on_stop_wait) {
          on_stop_wait();
        }
        state_lock.lock();
        state_cv_.wait(state_lock,
                       [this] { return lifecycle_ == Lifecycle::Stopped; });
        return;
      }
      accepting_ = false;
      lifecycle_ = Lifecycle::Stopping;
      for (auto &t : threads_) {
        t.request_stop();
      }
    }

    WordTask queued;
    while (tasks_.try_pop(queued)) {
      WordResult cancelled;
      cancelled.task_id = queued.task_id;
      cancelled.epoch = queued.epoch;
      cancelled.terminal_status = WordTerminalStatus::Cancelled;
      cancelled.word_len = queued.word.size();
      cancelled.analysis_len = queued.analysis_len;
      cancelled.layout_at_boundary = queued.layout_at_boundary;
      publish_terminal(std::move(cancelled));
    }

    tasks_.notify_all();
    for (auto &t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      synthesize_outstanding_locked(fatal_ ? WordTerminalStatus::Failed
                                           : WordTerminalStatus::Cancelled);
      threads_.clear();
      lifecycle_ = Lifecycle::Stopped;
    }
    state_cv_.notify_all();
  }

  [[nodiscard]] AnalysisAdmission submit(WordTask task) {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    if (task.word.size() > kMaxWordLen ||
        task.analysis_len > task.word.size() ||
        (task.layout_at_boundary != 0 && task.layout_at_boundary != 1) ||
        !accepting_ || lifecycle_ != Lifecycle::Running || fatal_ ||
        task_states_.contains(task.task_id) ||
        task_states_.size() >= max_outstanding_tasks_) {
      return {};
    }

    const std::uint64_t task_id = task.task_id;
    task.epoch = epoch_;
    TaskState state;
    state.epoch = task.epoch;
    state.word_len = task.word.size();
    state.analysis_len = task.analysis_len;
    state.layout_at_boundary = task.layout_at_boundary;
    task_states_.emplace(task.task_id, state);
    try {
      tasks_.push(std::move(task), [this](WordTask &committed) {
        const auto accepted_at = std::chrono::steady_clock::now();
        committed.submitted_at = accepted_at;
        task_states_.at(committed.task_id).accepted_at = accepted_at;
      });
    } catch (...) {
      task_states_.erase(task_id);
      throw;
    }
    const TaskState &committed = task_states_.at(task_id);
    return {true, committed.epoch, committed.accepted_at};
  }

  [[nodiscard]] bool try_pop_result(WordResult &out) {
    if (results_.try_pop(out)) {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      auto it = task_states_.find(out.task_id);
      if (it != task_states_.end() && it->second.epoch == out.epoch &&
          it->second.terminal_status.has_value()) {
        task_states_.erase(it);
      }
      return true;
    }

    std::lock_guard<std::mutex> state_lock(state_mu_);
    for (auto it = task_states_.begin(); it != task_states_.end(); ++it) {
      const TaskState &state = it->second;
      if (!state.terminal_status.has_value() || state.result_queued) {
        continue;
      }
      out = WordResult{};
      out.task_id = it->first;
      out.epoch = state.epoch;
      out.terminal_status = *state.terminal_status;
      out.word_len = state.word_len;
      out.analysis_len = state.analysis_len;
      out.layout_at_boundary = state.layout_at_boundary;
      task_states_.erase(it);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool has_fatal_error() const {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    return fatal_;
  }

  void begin_new_epoch() {
    std::vector<WordResult> cancelled;
    {
      std::lock_guard<std::mutex> state_lock(state_mu_);
      if (fatal_ || lifecycle_ != Lifecycle::Running) {
        return;
      }
      ++epoch_;
      const auto retired_tasks =
          tasks_.extract_if([current_epoch = epoch_](const WordTask &task) {
            return task.epoch < current_epoch;
          });
      (void)retired_tasks;
      cancelled.reserve(task_states_.size());
      for (const auto &[task_id, state] : task_states_) {
        if (state.epoch < epoch_ && !state.terminal_status.has_value()) {
          WordResult result;
          result.task_id = task_id;
          result.epoch = state.epoch;
          result.terminal_status = WordTerminalStatus::Cancelled;
          cancelled.push_back(std::move(result));
        }
      }
    }
    for (auto &result : cancelled) {
      publish_terminal(std::move(result));
    }
  }

  [[nodiscard]] std::size_t pending_task_count() const { return tasks_.size(); }

  [[nodiscard]] std::size_t worker_count() const {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    return threads_.size();
  }

private:
  void worker_main(std::stop_token st) {
    try {
      while (!st.stop_requested()) {
        auto opt = tasks_.pop_wait(st);
        if (!opt.has_value()) {
          break;
        }

        WordTask task = std::move(*opt);
        if (before_task_) {
          before_task_();
        }

        WordResult res;
        res.task_id = task.task_id;
        res.epoch = task.epoch;
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

        try {
          // Проверяем что слово достаточно длинное
          if (task.analysis_len < task.cfg.min_word_len) {
            res.analysis_us = 0;
            publish_terminal(std::move(res));
            continue;
          }

          // Берём только анализируемую часть (без trailing пунктуации)
          std::span<const KeyEntry> analysis_span(task.word.data(),
                                                  task.analysis_len);

          // =========================================================================
          // Этап 0: Smart Bypass — определяем, нужно ли пропускать РЕГИСТРОВЫЕ
          // исправления (sticky shift). Layout-классификация остаётся
          // разрешённой; product EventLoop не применяет это решение к
          // документу.
          // =========================================================================
          BypassReason bypass_reason =
              should_bypass(analysis_span, task.cfg.min_word_len);

          // bypass_case_fix = true означает, что мы НЕ будем исправлять регистр
          // (sticky shift), но БУДЕМ переключать раскладку при необходимости
          const bool bypass_case_fix = (bypass_reason != BypassReason::None);

          const bool is_en_layout = (task.layout_at_boundary == 0);

          // =========================================================================
          // Этап 0.5: Проверка на цифры и спецсимволы
          // Слова с цифрами не должны вызывать переключение раскладки,
          // так как цифры на основной клавиатуре одинаковы для EN/RU.
          // =========================================================================
          if (LayoutAnalyzer::has_invalid_chars(analysis_span)) {
            // Слово содержит цифры или спецсимволы — пропускаем без
            // переключения
            finish_and_push(res, t0);
            continue;
          }

          // =========================================================================
          // Этап 1: Словарная проверка (строго по словарям)
          //
          // Требование:
          //  - если слова НЕТ в словаре текущей раскладки, но ЕСТЬ в
          //  противоположной
          //    → переключаем раскладку (word inversion);
          //  - если слова ЕСТЬ в текущей, но НЕТ в противоположной → НЕ
          //  переключаем;
          //  - если слова НЕТ в обеих → НЕ переключаем;
          //  - если слова ЕСТЬ в обеих → дополнительно решаем по N-граммам.
          // =========================================================================
          DictResult dict_result = dict_->lookup(analysis_span);

          const bool in_en_dict = (dict_result == DictResult::English ||
                                   dict_result == DictResult::Both);
          const bool in_ru_dict = (dict_result == DictResult::Russian ||
                                   dict_result == DictResult::Both);

          const bool in_current_dict = is_en_layout ? in_en_dict : in_ru_dict;
          const bool in_opposite_dict = is_en_layout ? in_ru_dict : in_en_dict;

          // Case A: !in_current && in_opposite -> переключаем
          if (!in_current_dict && in_opposite_dict) {
            // Combined fix (layout + case) блокируется bypass.
            if (!bypass_case_fix && task.cfg.sticky_shift_correction_enabled) {
              CasePattern pattern = detect_case_pattern(analysis_span);
              if (pattern == CasePattern::StickyShiftUU ||
                  pattern == CasePattern::StickyShiftLU) {
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

          // Case B: in_current && !in_opposite -> не переключаем
          if (in_current_dict && !in_opposite_dict) {
            // Но sticky shift fix в текущей раскладке по-прежнему возможен.
            if (!bypass_case_fix && task.cfg.sticky_shift_correction_enabled) {
              if (try_sticky_shift_fix(analysis_span, res)) {
                finish_and_push(res, t0);
                continue;
              }
            }

            finish_and_push(res, t0);
            continue;
          }

          // Case D: слово есть в обоих словарях -> решаем через N-граммы
          if (in_current_dict && in_opposite_dict) {
            LayoutAnalyzer analyzer(task.cfg);
            AnalysisResult ar = analyzer.analyze(analysis_span);

            bool switch_target_is_en = false;
            bool should_switch_by_ngram = false;

            if (ar.should_switch) {
              const bool ngram_suggests_en = (ar.en_score > ar.ru_score);
              const bool ngram_suggests_ru = (ar.ru_score > ar.en_score);

              const bool looks_like_valid_en = (ar.en_invalid_count == 0);
              const bool looks_like_valid_ru = (ar.ru_invalid_count == 0);

              // Дополнительные признаки на основе invalid биграмм
              const bool invalid_suggests_en =
                  (ar.ru_invalid_count > 0 && ar.en_invalid_count == 0);
              const bool invalid_suggests_ru =
                  (ar.en_invalid_count > 0 && ar.ru_invalid_count == 0);

              if ((ngram_suggests_en && looks_like_valid_en) ||
                  invalid_suggests_en) {
                should_switch_by_ngram = true;
                switch_target_is_en = true;
              } else if ((ngram_suggests_ru && looks_like_valid_ru) ||
                         invalid_suggests_ru) {
                should_switch_by_ngram = true;
                switch_target_is_en = false;
              }
            }

            if (should_switch_by_ngram) {
              // Переключаем только если текущая раскладка не соответствует
              // цели.
              if ((switch_target_is_en && !is_en_layout) ||
                  (!switch_target_is_en && is_en_layout)) {
                res.need_switch = true;

                // Если есть sticky shift — делаем CombinedFix.
                if (!bypass_case_fix &&
                    task.cfg.sticky_shift_correction_enabled) {
                  CasePattern pattern = detect_case_pattern(analysis_span);
                  if (pattern == CasePattern::StickyShiftUU ||
                      pattern == CasePattern::StickyShiftLU) {
                    res.correction_type = CorrectionType::CombinedFix;
                    res.correction = make_title_case(analysis_span);
                  } else {
                    res.correction_type = CorrectionType::LayoutSwitch;
                  }
                } else {
                  res.correction_type = CorrectionType::LayoutSwitch;
                }

                finish_and_push(res, t0);
                continue;
              }
            }

            // N-gram не требует переключения — можно сделать sticky shift fix.
            if (!bypass_case_fix && task.cfg.sticky_shift_correction_enabled) {
              if (try_sticky_shift_fix(analysis_span, res)) {
                finish_and_push(res, t0);
                continue;
              }
            }

            finish_and_push(res, t0);
            continue;
          }

          // Case C: слова нет в обоих словарях -> раскладку НЕ переключаем.
          // (можно продолжить на typo fix)

          // =========================================================================
          // Этап 2: Слово Unknown — пробуем typo fix
          // =========================================================================
          if (dict_result == DictResult::Unknown &&
              task.cfg.typo_correction_enabled &&
              dict_->is_hunspell_available()) {

            std::string word_str = keys_to_utf8(analysis_span, is_en_layout);

            if (!word_str.empty() &&
                analysis_span.size() >= task.cfg.min_word_len) {
              // КРИТИЧНО: проверяем что слово действительно неправильное через
              // spell()
              bool is_correct = dict_->spell(word_str, is_en_layout);

              if (is_correct) {
                // Слово корректное — не исправляем.
                // Важно: для Unknown слов раскладку больше НЕ переключаем
                // (требование).
              } else {
                // Слово неправильное, запрашиваем предложения
                std::vector<std::string> suggestions =
                    dict_->suggest(word_str, is_en_layout, 5);

                bool typo_fix_found = false;
                for (const auto &suggestion : suggestions) {
                  // Проверяем что suggestion - это исправление оригинала
                  if (suggestion == word_str) {
                    // Слово уже правильное
                    break;
                  }

                  std::vector<KeyEntry> corrected = utf8_to_keys(
                      suggestion, is_en_layout, true, analysis_span);
                  if (corrected.empty() || corrected.size() > kMaxWordLen) {
                    continue;
                  }

                  const std::size_t distance = damerau_levenshtein_distance(
                      analysis_span, std::span<const KeyEntry>{
                                         corrected.data(), corrected.size()});

                  // distance=0 означает слово правильное
                  if (distance == 0) {
                    break;
                  }

                  if (distance > 0 && distance <= task.cfg.max_typo_diff) {
                    // Нашли исправление!
                    res.correction_type = CorrectionType::TypoFix;
                    res.correction = std::move(corrected);
                    typo_fix_found = true;
                    break;
                  }
                }

                // Если нашли typo fix — отправляем результат и переходим к
                // следующей задаче
                if (typo_fix_found) {
                  finish_and_push(res, t0);
                  continue;
                }
              }
            }
          }

          finish_and_push(res, t0);
        } catch (const std::exception &ex) {
          std::cerr << "[punto] AnalysisWorkerPool: task " << res.task_id
                    << " failed: " << ex.what() << "\n";
          res.terminal_status = WordTerminalStatus::Failed;
          finish_and_push(res, t0);
        } catch (...) {
          std::cerr << "[punto] AnalysisWorkerPool: task " << res.task_id
                    << " failed with unknown exception\n";
          res.terminal_status = WordTerminalStatus::Failed;
          finish_and_push(res, t0);
        }
      }
    } catch (const std::exception &ex) {
      std::cerr << "[punto] AnalysisWorkerPool: worker terminated: "
                << ex.what() << "\n";
      latch_fatal();
    } catch (...) {
      std::cerr << "[punto] AnalysisWorkerPool: worker terminated by unknown "
                   "exception\n";
      latch_fatal();
    }
  }

  // Helper: завершить анализ и положить результат в очередь
  void finish_and_push(WordResult &res,
                       std::chrono::steady_clock::time_point t0) {
    const auto t1 = std::chrono::steady_clock::now();
    res.analysis_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    publish_terminal(std::move(res));
  }

  void publish_terminal(WordResult res) {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    auto it = task_states_.find(res.task_id);
    if (it == task_states_.end() || it->second.epoch != res.epoch ||
        it->second.terminal_status.has_value()) {
      return;
    }
    const WordTerminalStatus status = res.terminal_status;
    try {
      results_.push(std::move(res));
      it->second.terminal_status = status;
      it->second.result_queued = true;
    } catch (...) {
      mark_fatal_locked();
    }
  }

  void latch_fatal() {
    std::lock_guard<std::mutex> state_lock(state_mu_);
    mark_fatal_locked();
  }

  void mark_fatal_locked() {
    fatal_ = true;
    accepting_ = false;
    synthesize_outstanding_locked(WordTerminalStatus::Failed);
  }

  void synthesize_outstanding_locked(WordTerminalStatus status) {
    for (auto &[task_id, state] : task_states_) {
      if (state.terminal_status.has_value()) {
        continue;
      }
      WordResult result;
      result.task_id = task_id;
      result.epoch = state.epoch;
      result.word_len = state.word_len;
      result.analysis_len = state.analysis_len;
      result.layout_at_boundary = state.layout_at_boundary;
      result.terminal_status = fatal_ ? WordTerminalStatus::Failed : status;
      try {
        results_.push(std::move(result));
        state.terminal_status = fatal_ ? WordTerminalStatus::Failed : status;
        state.result_queued = true;
      } catch (...) {
        fatal_ = true;
        accepting_ = false;
        state.terminal_status = WordTerminalStatus::Failed;
        state.result_queued = false;
      }
    }
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
  std::function<void()> before_task_;
  std::function<void()> on_stop_wait_;
  const std::size_t max_outstanding_tasks_;

  struct TaskState {
    std::uint64_t epoch = 0;
    std::optional<WordTerminalStatus> terminal_status;
    bool result_queued = false;
    std::chrono::steady_clock::time_point accepted_at{};
    std::size_t word_len = 0;
    std::size_t analysis_len = 0;
    int layout_at_boundary = 0;
  };

  enum class Lifecycle { Running, Stopping, Stopped };

  ConcurrentQueue<WordTask> tasks_;
  ConcurrentQueue<WordResult> results_;

  std::vector<std::jthread> threads_;
  mutable std::mutex state_mu_;
  std::condition_variable state_cv_;
  std::unordered_map<std::uint64_t, TaskState> task_states_;
  std::uint64_t epoch_ = 0;
  bool accepting_ = true;
  Lifecycle lifecycle_ = Lifecycle::Running;
  bool fatal_ = false;
};

} // namespace punto
