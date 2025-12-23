#include "punto/history_manager.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

[[noreturn]] void test_fail(const char* expr, const char* file, int line) {
  std::cerr << "TEST FAIL: " << expr << " (" << file << ":" << line << ")\n";
  std::abort();
}

#define CHECK(expr) \
  do { \
    if (!(expr)) { \
      test_fail(#expr, __FILE__, __LINE__); \
    } \
  } while (0)

using punto::HistoryManager;
using punto::KeyEntry;
using punto::ScanCode;

constexpr ScanCode kA = 30; // arbitrary, not used for mapping in tests
constexpr ScanCode kB = 48;
constexpr ScanCode kC = 46;
constexpr ScanCode kSpace = KEY_SPACE;

void test_push_pop_cursor() {
  HistoryManager hm{5};

  CHECK(hm.base_pos() == 0);
  CHECK(hm.cursor_pos() == 0);

  hm.push_token(KeyEntry{kA, false});
  hm.push_token(KeyEntry{kB, true});
  hm.push_token(KeyEntry{kC, false});

  CHECK(hm.base_pos() == 0);
  CHECK(hm.cursor_pos() == 3);

  bool ok = hm.pop_token();
  CHECK(ok);
  CHECK(hm.cursor_pos() == 2);

  ok = hm.pop_token();
  CHECK(ok);
  CHECK(hm.cursor_pos() == 1);

  ok = hm.pop_token();
  CHECK(ok);
  CHECK(hm.cursor_pos() == 0);

  ok = hm.pop_token();
  CHECK(!ok);
}

void test_get_range() {
  HistoryManager hm{5};

  hm.push_token(KeyEntry{kA, false}); // 0
  hm.push_token(KeyEntry{kB, false}); // 1
  hm.push_token(KeyEntry{kC, false}); // 2
  hm.push_token(KeyEntry{kSpace, false}); // 3

  std::vector<KeyEntry> out;
  bool ok = hm.get_range(0, 3, out);
  CHECK(ok);
  CHECK(out.size() == 3);
  CHECK(out[0].code == kA);
  CHECK(out[1].code == kB);
  CHECK(out[2].code == kC);

  ok = hm.get_range(3, 4, out);
  CHECK(ok);
  CHECK(out.size() == 1);
  CHECK(out[0].code == kSpace);
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
  CHECK(cursor == 7);

  std::vector<KeyEntry> tail;
  bool ok = hm.get_range(end_pos, cursor, tail);
  CHECK(ok);

  const std::size_t word_len = static_cast<std::size_t>(end_pos - start_pos);
  const std::size_t erase = static_cast<std::size_t>(cursor - start_pos);
  const std::size_t expected_retype = word_len + tail.size();

  CHECK(word_len == 3);
  CHECK(tail.size() == 4);
  CHECK(erase == 7);
  CHECK(expected_retype == erase);
}

} // namespace

#undef CHECK

int main() {
  test_push_pop_cursor();
  test_get_range();
  test_length_invariant_math_like_apply_correction();

  std::cout << "OK\n";
  return 0;
}
