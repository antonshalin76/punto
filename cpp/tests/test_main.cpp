#include "punto/text_processor.hpp"

#include <cassert>
#include <iostream>

using namespace punto;

int main() {
  // UTF-8 codepoint count
  assert(utf8_codepoint_count("") == 0);
  assert(utf8_codepoint_count("a") == 1);
  assert(utf8_codepoint_count("привет") == 6);
  assert(utf8_codepoint_count("aпривет") == 7);

  // Layout inversion
  assert(invert_layout("ghbdtn") == "привет");
  assert(invert_layout("привет") == "ghbdtn");

  // Case inversion
  assert(invert_case("AbC") == "aBc");

  // Transliteration (cyr -> lat)
  assert(transliterate("привет") == "privet");

  std::cout << "punto-tests: OK\n";
  return 0;
}
