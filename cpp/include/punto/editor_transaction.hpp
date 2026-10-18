#pragma once

#include <xcb/xcb.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace punto {

enum class EditorTransactionMode { Word, Selection };

enum class EditorTransactionStatus {
  Unavailable,
  Prepared,
  Applied,
  Rejected,
  Unknown,
};

struct EditorTarget {
  std::uint32_t uid = 0;
  std::uint32_t pid = 0;
  xcb_window_t window = XCB_WINDOW_NONE;
  std::string runtime_directory;
};

struct PreparedEditorTransaction {
  EditorTarget target;
  std::string operation_id;
  std::string editor_instance;
  std::uint64_t widget_epoch = 0;
  std::uint64_t context_generation = 0;
  std::uint64_t revision = 0;
  std::uint32_t start = 0;
  std::uint32_t end = 0;
  std::string fragment;
  EditorTransactionMode mode = EditorTransactionMode::Word;
  std::int64_t not_after_monotonic_ms = 0;
};

enum class EditorTransactionExecution { NotAuthorized, Active, Quiescent, Unknown };
enum class EditorTransactionEffect { NotAttempted, Applied, Rejected, Unknown };

struct EditorTabTransaction {
  PreparedEditorTransaction prepared;
  std::string replacement;
  std::string frame_id;
  std::string frame_digest;
  std::uint32_t trigger_index = 0;
  std::string barrier_token;
  std::string command_identity;
};

struct EditorTransactionResult {
  EditorTransactionStatus status = EditorTransactionStatus::Unavailable;
  EditorTransactionExecution execution = EditorTransactionExecution::NotAuthorized;
  EditorTransactionEffect effect = EditorTransactionEffect::NotAttempted;
  std::optional<EditorTabTransaction> active_tab;
  std::string operation_id;
  std::string command_identity;
  std::uint64_t resulting_revision = 0;
  std::uint32_t resulting_start = 0;
  std::uint32_t resulting_end = 0;
  std::string reason;
};

class EditorTransactionClient {
public:
  static std::optional<EditorTarget>
  discover(xcb_connection_t *connection, int screen_number,
           xcb_window_t expected_focus, std::uint32_t session_uid,
           std::string_view runtime_directory,
           std::chrono::steady_clock::time_point deadline);

  explicit EditorTransactionClient(EditorTarget target);

  [[nodiscard]] std::optional<PreparedEditorTransaction>
  prepare(EditorTransactionMode mode, std::string_view expected_fragment,
          std::chrono::steady_clock::time_point request_deadline,
          std::chrono::steady_clock::time_point operation_deadline,
          std::string_view request_key = {});

  [[nodiscard]] EditorTransactionResult
  compare_apply(const PreparedEditorTransaction &prepared,
                std::string_view replacement,
                std::chrono::steady_clock::time_point deadline);

  [[nodiscard]] EditorTransactionResult compare_apply_before_tab(
      const PreparedEditorTransaction &prepared, std::string_view replacement,
      std::string_view frame_id, std::string_view frame_digest,
      std::uint32_t trigger_index,
      std::chrono::steady_clock::time_point deadline,
      const std::function<bool()> &before_decide,
      const std::function<void()> &release_original_frame,
      const std::function<bool(std::chrono::steady_clock::time_point)> &
          buffer_input);

  [[nodiscard]] EditorTransactionResult poll_tab_status(
      const EditorTabTransaction &transaction,
      std::chrono::steady_clock::time_point deadline);

#if defined(PUNTO_EDITOR_TRANSACTION_TESTING)
  [[nodiscard]] bool compare_apply_without_read_for_test(
      const PreparedEditorTransaction &prepared, std::string_view replacement,
      std::chrono::steady_clock::time_point deadline);
#endif

private:
  EditorTarget target_;
};

} // namespace punto
