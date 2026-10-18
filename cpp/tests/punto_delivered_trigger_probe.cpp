#include "punto/editor_transaction.hpp"
#include "punto/text_processor.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

bool parse_u32(std::string_view text, std::uint32_t &value) {
  const char *const end = text.data() + text.size();
  const auto [next, error] =
      std::from_chars(text.data(), end, value, 10);
  return error == std::errc{} && next == end;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 5) {
    std::cerr << "usage: punto-delivered-trigger-probe PID XID WORD BUDGET_MS\n";
    return 2;
  }
  std::uint32_t pid = 0;
  std::uint32_t window = 0;
  std::uint32_t budget_ms = 0;
  const std::string_view expected{argv[3]};
  if (!parse_u32(argv[1], pid) || pid == 0 ||
      !parse_u32(argv[2], window) || window == 0 ||
      !parse_u32(argv[4], budget_ms) || budget_ms == 0 ||
      budget_ms > 500 || expected.empty() || expected.size() > 4096) {
    std::cerr << "REJECTED invalid_input\n";
    return 2;
  }
  const std::string replacement = punto::invert_layout(expected);
  if (replacement.empty() || replacement == expected) {
    std::cerr << "REJECTED no_conversion\n";
    return 3;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = now + std::chrono::milliseconds{budget_ms};
  punto::EditorTarget target{
      static_cast<std::uint32_t>(::getuid()), pid, window,
      "/run/user/" + std::to_string(::getuid())};
  punto::EditorTransactionClient client{target};
  const auto prepared = client.prepare(
      punto::EditorTransactionMode::Word, expected,
      std::min(now + std::chrono::milliseconds{100}, deadline), deadline);
  if (!prepared) {
    std::cerr << "REJECTED prepare\n";
    return 3;
  }
  const auto result = client.compare_apply(*prepared, replacement, deadline);
  if (result.status == punto::EditorTransactionStatus::Applied &&
      result.execution == punto::EditorTransactionExecution::Quiescent &&
      result.effect == punto::EditorTransactionEffect::Applied) {
    std::cout << "APPLIED\n";
    return 0;
  }
  if (result.status == punto::EditorTransactionStatus::Rejected &&
      result.execution == punto::EditorTransactionExecution::Quiescent) {
    std::cerr << "REJECTED " << result.reason << '\n';
    return 3;
  }
  std::cerr << "UNKNOWN " << result.reason << '\n';
  return 4;
}
