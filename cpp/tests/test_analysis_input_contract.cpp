#include "punto/analysis_worker_pool.hpp"
#include "punto/input_buffer.hpp"
#include "punto/layout_analyzer.hpp"
#include "punto/typo_corrector.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace punto;

namespace {

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "analysis/input contract failure: " << message << '\n';
    std::abort();
  }
}

void test_input_word_limit_is_exact_and_overflow_invalidates_manual_undo() {
  InputBuffer buffer;

  expect(buffer.push_char(KEY_Z, false), "seed last word");
  buffer.commit_word();
  expect(buffer.last_length() == 1, "seed last word committed");

  for (std::size_t i = 0; i < kMaxWordLen; ++i) {
    expect(buffer.push_char(KEY_A, false),
           "all documented word slots remain usable");
  }
  expect(buffer.current_length() == kMaxWordLen,
         "word reaches documented maximum length");

  expect(!buffer.push_char(KEY_B, false), "overlong word is rejected");
  expect(buffer.current_overflowed(),
         "overlong word is latched until boundary");
  expect(buffer.current_word().empty(), "overlong current word is discarded");
  expect(buffer.last_word().empty(),
         "previous word cannot be reactivated after overlong input");
  expect(buffer.get_active_word().empty(),
         "manual hotkey has no stale word after overflow");

  buffer.commit_word();
  expect(!buffer.current_overflowed(),
         "word overflow latch clears at boundary");
  expect(buffer.get_active_word().empty(),
         "overflow boundary cannot restore a stale word");
}

void test_trailing_limit_is_exact_and_overflow_invalidates_manual_word() {
  InputBuffer buffer;
  expect(buffer.push_char(KEY_A, false), "seed word for trailing overflow");
  buffer.commit_word();

  for (std::size_t i = 0; i < kMaxWordLen; ++i) {
    expect(buffer.push_trailing(KEY_SPACE),
           "all documented trailing slots remain usable");
  }
  expect(buffer.trailing_length() == kMaxWordLen,
         "trailing reaches documented maximum length");

  expect(!buffer.push_trailing(KEY_TAB), "overlong trailing input is rejected");
  expect(buffer.trailing().empty(),
         "unsafe truncated trailing state is discarded");
  expect(buffer.get_active_word().empty(),
         "manual correction cannot omit an overlong trailing suffix");

  expect(buffer.push_char(KEY_B, false),
         "new word starts cleanly after trailing overflow");
  expect(buffer.current_length() == 1, "new word is tracked after overflow");
}

void test_layout_analyzer_accepts_full_documented_word() {
  AutoSwitchConfig config;
  config.enabled = true;
  config.min_word_len = 1;
  LayoutAnalyzer analyzer(config);

  std::vector<KeyEntry> word(kMaxWordLen, KeyEntry{KEY_A, false});
  const double score = analyzer.calculate_score(word, Language::English);
  expect(std::isfinite(score), "maximum-length score remains finite");

  std::size_t en_invalid = 0;
  std::size_t ru_invalid = 0;
  LayoutAnalyzer::count_invalid_bigrams(word, en_invalid, ru_invalid);
  expect(en_invalid <= word.size() - 1,
         "maximum-length invalid bigram count is bounded");
  expect(ru_invalid <= word.size() - 1,
         "maximum-length alternate bigram count is bounded");
}

void test_unknown_scancode_is_not_treated_as_a_word_character() {
  const std::vector<KeyEntry> word{{static_cast<ScanCode>(255), false},
                                   {KEY_A, false}};
  expect(LayoutAnalyzer::has_invalid_chars(word),
         "unknown scancode makes automatic correction ineligible");
}

void test_analysis_rejects_invalid_span_before_queue_commit() {
  Dictionary dictionary;
  AnalysisWorkerPool pool(dictionary);

  WordTask invalid;
  invalid.task_id = 1;
  invalid.analysis_len = 1;
  expect(!pool.submit(std::move(invalid)).accepted,
         "analysis_len beyond word storage is rejected");
  expect(pool.pending_task_count() == 0,
         "invalid task never reaches the worker queue");
  pool.stop();

  WordResult result;
  expect(!pool.try_pop_result(result),
         "rejected task has no synthetic accepted terminal");
}

void test_sticky_shift_is_not_hidden_by_generic_uppercase_run() {
  const std::vector<KeyEntry> word{{KEY_G, true},  {KEY_H, true},
                                   {KEY_B, true},  {KEY_D, false},
                                   {KEY_T, false}, {KEY_Y, false}};
  expect(detect_case_pattern(word) == CasePattern::StickyShiftUU,
         "three leading uppercase letters remain a sticky-shift pattern");
}

void test_utf8_typo_distance_and_conversion_are_character_safe() {
  expect(damerau_levenshtein_distance("я", "а") == 1,
         "one Cyrillic substitution has distance one");
  expect(utf8_to_keys("привет!", false).empty(),
         "unsupported suggestion is rejected instead of truncated");
  expect(ascii_to_keys("hello!").empty(),
         "unsupported ASCII suggestion is rejected instead of truncated");
}

} // namespace

int main() {
  test_input_word_limit_is_exact_and_overflow_invalidates_manual_undo();
  test_trailing_limit_is_exact_and_overflow_invalidates_manual_word();
  test_layout_analyzer_accepts_full_documented_word();
  test_unknown_scancode_is_not_treated_as_a_word_character();
  test_analysis_rejects_invalid_span_before_queue_commit();
  test_sticky_shift_is_not_hidden_by_generic_uppercase_run();
  test_utf8_typo_distance_and_conversion_are_character_safe();
  return 0;
}
