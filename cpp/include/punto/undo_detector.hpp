/**
 * @file undo_detector.hpp
 * @brief Bounded undo-learning state with secure persistent storage.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#ifdef PUNTO_TESTING
#include <functional>
#endif

namespace punto {

inline constexpr const char *kDefaultExclusionsPath =
    "/etc/punto/undo_exclusions.txt";

class UndoDetector {
public:
  explicit UndoDetector(std::string path = kDefaultExclusionsPath);
  ~UndoDetector();
  UndoDetector(const UndoDetector &) = delete;
  UndoDetector &operator=(const UndoDetector &) = delete;
#ifdef PUNTO_TESTING
  UndoDetector(std::string path, std::function<void()> before_io,
               std::function<int(int)> directory_sync = {});
#endif

  void on_correction_applied(std::uint64_t task_id,
                             const std::string &original_word);
  bool on_backspace(std::chrono::steady_clock::time_point now);
  void on_undo();
  void on_key_typed() noexcept;

  [[nodiscard]] bool is_excluded(const std::string &word) const;
  [[nodiscard]] std::size_t exclusion_count() const noexcept;
  // Initial read has completed, including a reported storage failure.
  [[nodiscard]] bool ready() const noexcept;
  // Cached learning is immediate; only !pending() && !persistence_failed()
  // confirms that accepted changes have reached durable storage.
  [[nodiscard]] bool pending() const noexcept;
  [[nodiscard]] bool persistence_failed() const noexcept;

  void clear_exclusions();
  // Schedule a background refresh and retry retained failed mutations.
  void load_from_file();
  void add_exclusion(const std::string &word);

  static constexpr std::size_t maximum_entries() noexcept { return 1024U; }
  static constexpr std::size_t maximum_word_bytes() noexcept { return 63U; }
  static constexpr std::size_t maximum_file_bytes() noexcept {
    return maximum_entries() * (maximum_word_bytes() + 1U);
  }

private:
  enum class PersistenceMutation { Add, Clear, Refresh, SyncDirectory };
  struct Store;
  struct SharedState;

  struct RecentCorrection {
    std::uint64_t task_id = 0;
    std::string original_word;
    std::chrono::steady_clock::time_point applied_at;
  };

  [[nodiscard]] static bool valid_word(const std::string &word) noexcept;
  void start();
  static void worker(const std::shared_ptr<SharedState> &state) noexcept;

  std::shared_ptr<SharedState> state_;
  std::thread thread_;
  std::optional<RecentCorrection> last_correction_;
  std::size_t backspace_count_since_correction_ = 0;

  static constexpr auto kUndoWindow = std::chrono::milliseconds(2000);
  static constexpr std::size_t kMinBackspaceCount = 3U;
};

} // namespace punto
