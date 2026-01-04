#include <cassert>
#include <vector>

#include <linux/input.h>

#include "punto/key_entry_text.hpp"
#include "punto/text_processor.hpp"
#include "punto/types.hpp"

namespace {

void test_text_processor_layout() {
  using namespace punto;

  assert(en_to_ru("ghbdtn") == "привет");
  assert(ru_to_en("привет") == "ghbdtn");

  assert(invert_layout("ghbdtn") == "привет");
  assert(invert_layout("привет") == "ghbdtn");
}

void test_text_processor_case() {
  using namespace punto;

  assert(invert_case("AbZ") == "aBz");
  assert(invert_case("Привет") == "пРИВЕТ");
}

void test_key_entry_text_checked() {
  using namespace punto;

  // Ghbdtn -> Привет (RU) при layout=1
  std::vector<KeyEntry> word;
  word.emplace_back(KEY_G, true);
  word.emplace_back(KEY_H, false);
  word.emplace_back(KEY_B, false);
  word.emplace_back(KEY_D, false);
  word.emplace_back(KEY_T, false);
  word.emplace_back(KEY_N, false);

  auto vis = key_entries_to_visible_text_checked(word, /*layout=*/1);
  assert(vis.has_value());
  assert(*vis == "Привет");

  // Shift mapping for digits
  std::vector<KeyEntry> punct;
  punct.emplace_back(KEY_1, true);
  punct.emplace_back(KEY_2, true);
  assert(key_entries_to_qwerty(punct) == "!@");

  auto punct_vis = key_entries_to_visible_text_checked(punct, /*layout=*/0);
  assert(punct_vis.has_value());
  assert(*punct_vis == "!@");

  // Fail-fast if a scancode cannot be mapped to QWERTY
  std::vector<KeyEntry> bad;
  bad.emplace_back(static_cast<ScanCode>(9999), false);
  auto bad_vis = key_entries_to_visible_text_checked(bad, /*layout=*/0);
  assert(!bad_vis.has_value());
}

} // namespace

int main() {
  test_text_processor_layout();
  test_text_processor_case();
  test_key_entry_text_checked();
  return 0;
}
