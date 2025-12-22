#include "punto/history_manager.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

using punto::HistoryManager;
using punto::KeyEntry;
using punto::ScanCode;

namespace {

constexpr ScanCode kA = 30; // arbitrary, not used for mapping in tests
constexpr ScanCode kB = 48;
constexpr ScanCode kC = 46;
constexpr ScanCode kSpace = KEY_SPACE;

void test_push_pop_cursor() {
  HistoryManager hm{5};

  assert(hm.base_pos() == 0);
  assert(hm.cursor_pos() == 0);

  hm.push_token(KeyEntry{kA, false});
  hm.push_token(KeyEntry{kB, true});
  hm.push_token(KeyEntry{kC, false});

  assert(hm.base_pos() == 0);
  assert(hm.cursor_pos() == 3);

  bool ok = hm.pop_token();
  assert(ok);
  assert(hm.cursor_pos() == 2);

  ok = hm.pop_token();
  assert(ok);
  assert(hm.cursor_pos() == 1);

  ok = hm.pop_token();
  assert(ok);
  assert(hm.cursor_pos() == 0);

  ok = hm.pop_token();
  assert(!ok);
}

void test_get_range() {
  HistoryManager hm{5};

  hm.push_token(KeyEntry{kA, false}); // 0
  hm.push_token(KeyEntry{kB, false}); // 1
  hm.push_token(KeyEntry{kC, false}); // 2
  hm.push_token(KeyEntry{kSpace, false}); // 3

  std::vector<KeyEntry> out;
  bool ok = hm.get_range(0, 3, out);
  assert(ok);
  assert(out.size() == 3);
  assert(out[0].code == kA);
  assert(out[1].code == kB);
  assert(out[2].code == kC);

  ok = hm.get_range(3, 4, out);
  assert(ok);
  assert(out.size() == 1);
  assert(out[0].code == kSpace);
}

void test_length_invariant_math_like_apply_correction() {
  // Симулируем: word1(3) + space + word2(2) + space
  HistoryManager hm{5};

  // word1
  hm.push_token(KeyEntry{kA, false}); // pos 0
  hm.push_token(KeyEntry{kB, false}); // pos 1
  hm.push_token(KeyEntry{kC, false}); // pos 2

  const std::uint64_t start_pos = 0;

  hm.push_token(KeyEntry{kSpace, false}); // pos 3 (delimiter)
  const std::uint64_t end_pos = 3;        // word_end = delimiter pos

  // tail
  hm.push_token(KeyEntry{kA, false}); // 4
  hm.push_token(KeyEntry{kB, false}); // 5
  hm.push_token(KeyEntry{kSpace, false}); // 6

  const std::uint64_t cursor = hm.cursor_pos();
  assert(cursor == 7);

  std::vector<KeyEntry> tail;
  bool ok = hm.get_range(end_pos, cursor, tail);
  assert(ok);

  const std::size_t word_len = static_cast<std::size_t>(end_pos - start_pos);
  const std::size_t erase = static_cast<std::size_t>(cursor - start_pos);
  const std::size_t expected_retype = word_len + tail.size();

  assert(word_len == 3);
  assert(tail.size() == 4);
  assert(erase == 7);
  assert(expected_retype == erase);
}

} // namespace

int main() {
  test_push_pop_cursor();
  test_get_range();
  test_length_invariant_math_like_apply_correction();

  std::cout << "OK\n";
  return 0;
}
