/**
 * @file undo_detector.hpp
 * @brief Bounded undo-learning state with secure persistent storage.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace punto {

inline constexpr const char *kDefaultExclusionsPath =
    "/etc/punto/undo_exclusions.txt";

class UndoDetector {
public:
  explicit UndoDetector(std::string path = kDefaultExclusionsPath);

  void on_correction_applied(std::uint64_t task_id,
                             const std::string &original_word);
  bool on_backspace(std::chrono::steady_clock::time_point now);
  void on_undo();
  void on_key_typed() noexcept;

  [[nodiscard]] bool is_excluded(const std::string &word);
  [[nodiscard]] std::size_t exclusion_count() const noexcept;

  void clear_exclusions();
  void load_from_file();
  void add_exclusion(const std::string &word);

  static constexpr std::size_t maximum_entries() noexcept { return 128U; }
  static constexpr std::size_t maximum_word_bytes() noexcept { return 63U; }

private:
  enum class PersistenceMutation { Add, Clear };

  struct RecentCorrection {
    std::uint64_t task_id = 0;
    std::string original_word;
    std::chrono::steady_clock::time_point applied_at;
  };

  [[nodiscard]] static bool valid_word(const std::string &word) noexcept;
  [[nodiscard]] bool refresh_from_file();
  [[nodiscard]] bool persist(PersistenceMutation mutation,
                             std::string_view word = {});

  std::string file_path_;
  std::optional<RecentCorrection> last_correction_;
  std::unordered_set<std::string> exclusions_;
  std::size_t backspace_count_since_correction_ = 0;

  static constexpr auto kUndoWindow = std::chrono::milliseconds(2000);
  static constexpr std::size_t kMinBackspaceCount = 3U;
};

} // namespace punto
