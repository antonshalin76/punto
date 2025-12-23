/**
 * @file layout_analyzer.cpp
 * @brief Реализация анализатора раскладки
 */

#include "punto/layout_analyzer.hpp"
#include "punto/asm_utils.hpp"
#include "punto/ngram_data.hpp"
#include "punto/scancode_map.hpp"

#include <algorithm>
#include <cstring>

namespace punto {

LayoutAnalyzer::LayoutAnalyzer(AutoSwitchConfig config)
    : config_(std::move(config)) {}

bool LayoutAnalyzer::should_switch(std::span<const KeyEntry> word) const {
  // Проверяем базовые условия
  if (!config_.enabled) {
    return false;
  }

  if (word.size() < config_.min_word_len) {
    return false;
  }

  // Проверяем на наличие цифр и спецсимволов
  if (has_invalid_chars(word)) {
    return false;
  }

  auto result = analyze(word);
  return result.should_switch;
}

AnalysisResult LayoutAnalyzer::analyze(std::span<const KeyEntry> word) const {
  AnalysisResult result;

  // Минимум 2 символа для биграммного анализа (1 биграмма)
  if (word.size() < 2) {
    return result;
  }

  result.en_score = calculate_score(word, Language::English);
  result.ru_score = calculate_score(word, Language::Russian);

  // Подсчитываем невалидные биграммы
  count_invalid_bigrams(word, result.en_invalid_count, result.ru_invalid_count);

  // Определяем вероятный язык
  if (result.ru_score > result.en_score) {
    result.likely_lang = Language::Russian;
  } else {
    result.likely_lang = Language::English;
  }

  // Определяем, нужно ли переключаться
  // Теперь работаем в обоих направлениях:
  // - Если ru_score >> en_score → слово русское в EN раскладке
  // - Если en_score >> ru_score → слово английское в RU раскладке

  double max_score = std::max(result.en_score, result.ru_score);
  double min_score = std::min(result.en_score, result.ru_score);

  // Проверяем минимальный скор — хотя бы один должен быть выше порога
  if (max_score < config_.min_score) {
    result.should_switch = false;
    return result;
  }

  // Проверяем порог — соотношение между максимальным и минимальным скорами
  if (min_score > 0.0) {
    double ratio = max_score / min_score;

    double effective_threshold = config_.threshold;
    result.should_switch = (ratio >= effective_threshold);
  } else {
    // Если min_score == 0, а max_score > min_score порога → переключаем
    result.should_switch = (max_score >= config_.min_score);
  }

  return result;
}

double LayoutAnalyzer::calculate_score(std::span<const KeyEntry> word,
                                       Language lang) const {
  if (word.size() < 2) {
    return 0.0;
  }

  // Prefetch N-gram tables to L1 cache for faster lookups
  asm_utils::prefetch_read(kEnBigrams.data());
  asm_utils::prefetch_read(kRuBigrams.data());
  asm_utils::prefetch_read(kEnTrigrams.data());
  asm_utils::prefetch_read(kRuTrigrams.data());

  // Конвертируем слово в ASCII
  char buffer[kMaxWordLen];
  std::size_t len = word_to_ascii(word, buffer);

  if (len < 2) {
    return 0.0;
  }

  double score = 0.0;
  std::size_t valid_ngrams = 0;

  // Проходим по всем биграммам
  for (std::size_t i = 0; i + 1 < len; ++i) {
    char first = buffer[i];
    char second = buffer[i + 1];

    std::uint8_t weight = 0;

    if (lang == Language::English) {
      weight = lookup_en_bigram(first, second);

      // Проверяем "невозможные" биграммы
      if (weight == 0 && is_invalid_en_bigram(first, second)) {
        score -= 15.0; // Штраф за невозможную биграмму
      }
    } else {
      weight = lookup_ru_bigram(first, second);

      // Проверяем "невозможные" биграммы
      if (weight == 0 && is_invalid_ru_bigram(first, second)) {
        score -= 15.0;
      }
    }

    if (weight > 0) {
      score += static_cast<double>(weight);
      ++valid_ngrams;
    }
  }

  // Проходим по всем триграммам (для слов от 3 символов)
  if (len >= 3) {
    for (std::size_t i = 0; i + 2 < len; ++i) {
      char first = buffer[i];
      char second = buffer[i + 1];
      char third = buffer[i + 2];

      std::uint8_t weight = 0;

      if (lang == Language::English) {
        weight = lookup_en_trigram(first, second, third);
      } else {
        weight = lookup_ru_trigram(first, second, third);
      }

      if (weight > 0) {
        // Триграммы имеют бóльший вес — они более надёжны
        score += static_cast<double>(weight) * 1.5;
        ++valid_ngrams;
      }
    }
  }

  // Нормализуем по количеству N-грамм
  if (valid_ngrams > 0) {
    // Нормализация: для слова из N букв есть (N-1) биграмм и (N-2) триграмм
    double expected_ngrams = static_cast<double>(len - 1);
    if (len >= 3) {
      expected_ngrams += static_cast<double>(len - 2);
    }
    score = score / expected_ngrams;
  }

  return std::max(0.0, score);
}

char LayoutAnalyzer::scancode_to_lowercase(ScanCode code) {
  if (code >= kScancodeToChar.size()) {
    return '\0';
  }

  char c = kScancodeToChar[code];

  // Приводим к нижнему регистру
  if (c >= 'A' && c <= 'Z') {
    return static_cast<char>(c + ('a' - 'A'));
  }

  return c;
}

bool LayoutAnalyzer::has_invalid_chars(std::span<const KeyEntry> word) {
  for (const auto &entry : word) {
    char c = scancode_to_lowercase(entry.code);

    // Проверяем на цифры
    if (c >= '0' && c <= '9') {
      return true;
    }

    // Проверяем на спецсимволы (кроме допустимых)
    // Допустимые: буквы, некоторые знаки препинания для русского (,.;'[])
    if (c == '\0') {
      // Неизвестный скан-код — возможно спецсимвол
      // Но не считаем это ошибкой, просто пропускаем
      continue;
    }

    // Проверяем ASCII спецсимволы
    if (!((c >= 'a' && c <= 'z') || c == ',' || c == '.' || c == ';' ||
          c == '\'' || c == '[' || c == ']' || c == '`' || c == '-')) {
      return true;
    }
  }

  return false;
}

std::size_t LayoutAnalyzer::word_to_ascii(std::span<const KeyEntry> word,
                                          char *buffer) {
  std::size_t len = 0;

  for (const auto &entry : word) {
    char c = scancode_to_lowercase(entry.code);
    if (c != '\0') {
      buffer[len++] = c;
    }
  }

  buffer[len] = '\0';
  return len;
}

void LayoutAnalyzer::count_invalid_bigrams(std::span<const KeyEntry> word,
                                           std::size_t &en_invalid,
                                           std::size_t &ru_invalid) {
  en_invalid = 0;
  ru_invalid = 0;

  if (word.size() < 2) {
    return;
  }

  char buffer[kMaxWordLen];
  std::size_t len = word_to_ascii(word, buffer);

  if (len < 2) {
    return;
  }

  for (std::size_t i = 0; i + 1 < len; ++i) {
    char first = buffer[i];
    char second = buffer[i + 1];

    if (is_invalid_en_bigram(first, second)) {
      ++en_invalid;
    }
    if (is_invalid_ru_bigram(first, second)) {
      ++ru_invalid;
    }
  }
}

} // namespace punto
