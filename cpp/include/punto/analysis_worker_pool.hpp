/**
 * @file analysis_worker_pool.hpp
 * @brief Пул потоков для асинхронного анализа слов (dict + n-gram)
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
#include "punto/types.hpp"

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

struct WordResult {
  std::uint64_t task_id = 0;
  bool need_switch = false;

  // Телеметрия
  std::size_t word_len = 0;
  std::size_t analysis_len = 0;
  int layout_at_boundary = 0;

  std::uint64_t queue_us = 0;   // ожидание в очереди до начала анализа
  std::uint64_t analysis_us = 0; // длительность анализа
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
    // jthread stop в деструкторе — но нам нужно разбудить ожидающих.
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
      res.word_len = task.word.size();
      res.analysis_len = task.analysis_len;
      res.layout_at_boundary = task.layout_at_boundary;

      const auto t_pop = std::chrono::steady_clock::now();
      if (task.submitted_at.time_since_epoch().count() != 0) {
        res.queue_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t_pop -
                                                                 task.submitted_at)
                .count());
      }

      const auto t0 = std::chrono::steady_clock::now();

      if (!dict_ || task.word.empty()) {
        results_.push(std::move(res));
        continue;
      }

      // Fail-fast: некорректный analysis_len.
      if (task.analysis_len > task.word.size()) {
        results_.push(std::move(res));
        continue;
      }

      // Базовые условия — как в синхронной реализации.
      if (!task.cfg.enabled || task.analysis_len < task.cfg.min_word_len) {
        results_.push(std::move(res));
        continue;
      }

      const std::span<const KeyEntry> analysis_span{task.word.data(),
                                                    task.analysis_len};

      const bool is_en_layout = (task.layout_at_boundary == 0);

      DictResult dict_result = dict_->lookup(analysis_span);

      if (dict_result == DictResult::English) {
        // Слово найдено в EN словаре → переключать только если мы в RU раскладке
        res.need_switch = !is_en_layout;
      } else if (dict_result == DictResult::Russian) {
        // Слово найдено в RU словаре → переключать только если мы в EN раскладке
        res.need_switch = is_en_layout;
      } else if (dict_result == DictResult::Unknown) {
        // Слово не найдено ни в одном словаре — используем N-граммы + анализ невалидных биграмм
        //
        // Логика принятия решения:
        // 1. Если слово содержит невалидные биграммы для одного языка, но не для другого —
        //    это сильный сигнал о неправильной раскладке.
        // 2. N-граммы используются как дополнительный сигнал.
        // 3. Для RU→EN требуем более строгие условия (нет невалидных EN биграмм).
        //
        LayoutAnalyzer analyzer(task.cfg);
        AnalysisResult ar = analyzer.analyze(analysis_span);

        if (is_en_layout) {
          // EN раскладка → возможно пользователь набирает русское слово
          // Переключаем на RU если:
          // - N-граммы показывают ru_score > en_score, ИЛИ
          // - Есть невалидные EN биграммы и нет невалидных RU биграмм
          bool ngram_suggests_ru = ar.should_switch && ar.ru_score > ar.en_score;
          bool invalid_suggests_ru = (ar.en_invalid_count > 0 && ar.ru_invalid_count == 0);
          bool invalid_balance_ru = (ar.en_invalid_count > ar.ru_invalid_count);

          if (ngram_suggests_ru || invalid_suggests_ru || 
              (invalid_balance_ru && ar.ru_score >= ar.en_score)) {
            res.need_switch = true;
          }
        } else {
          // RU раскладка → возможно пользователь набирает английское слово
          // Переключаем на EN если:
          // - N-граммы показывают en_score > ru_score, И
          // - НЕТ невалидных EN биграмм (слово выглядит как валидное английское)
          bool ngram_suggests_en = ar.should_switch && ar.en_score > ar.ru_score;
          bool looks_like_valid_en = (ar.en_invalid_count == 0);
          bool invalid_suggests_en = (ar.ru_invalid_count > 0 && ar.en_invalid_count == 0);

          if ((ngram_suggests_en && looks_like_valid_en) || invalid_suggests_en) {
            res.need_switch = true;
          }
        }
      }

      const auto t1 = std::chrono::steady_clock::now();
      res.analysis_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

      results_.push(std::move(res));
    }
  }

  const Dictionary *dict_ = nullptr;

  ConcurrentQueue<WordTask> tasks_;
  ConcurrentQueue<WordResult> results_;

  std::vector<std::jthread> threads_;
};

} // namespace punto
