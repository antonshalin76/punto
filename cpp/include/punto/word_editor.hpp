#pragma once

#include <linux/input.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace punto {

class X11Session;
class ClipboardManager;
class DesktopLayoutClient;
struct SelectionData;

enum class WordEditOperation {
  // Exact expected range, used by automatic correction and undo.
  Word,
  // GUI range comes from the application; terminals use admitted typing
  // history.
  CurrentWordLayout,
  CurrentWordCase,
  SelectionLayout,
  SelectionCase,
  SelectionTranslit,
  NativeUndo,
};

struct WordEditRequest {
  std::string expected;
  std::string replacement;
  int target_layout = -1;
  int source_layout = -1;
  std::uint64_t session_generation = 0;
  WordEditOperation operation = WordEditOperation::Word;
  std::uint32_t expected_focus = 0;
  int source_locked_mods = -1;
  bool allow_terminal = true;
  std::uint64_t text_input_sequence = 0;
  std::optional<std::chrono::steady_clock::time_point> held_tab_deadline;
  std::string expected_editor_token = {};
};

enum class WordEditStatus {
  Rejected,
  PreparedNotReplayed,
  Dispatched,
  PartialFailure,
};

enum class WordEditRetryPolicy {
  Never,
  AfterFreshInput,
};

enum class DesktopLayoutTransitionState {
  NotRequested,
  RequestedUnresolved,
  TargetObserved,
};

struct WordEditOutcome {
  WordEditStatus status = WordEditStatus::Rejected;
  std::string original;
  std::string replacement;
  int source_layout = -1;
  int target_layout = -1;
  std::uint64_t session_generation = 0;
  std::uint32_t focused_window = 0;
  bool terminal_insert = false;
  bool terminal_history = false;
  std::string editor_token;
  WordEditRetryPolicy retry_policy = WordEditRetryPolicy::Never;
  DesktopLayoutTransitionState layout_transition =
      DesktopLayoutTransitionState::NotRequested;
  // Last layout observed under the same session/focus/pointer/lock context.
  int observed_layout = -1;
  // Static check-group label only; never editor or clipboard content.
  std::string_view rejection_stage = "invalid_request";
};

// Bounded X11 executor. Dispatched means the server accepted the
// sequence, not that the application acknowledged the resulting editor text.
class WordEditor {
public:
  using WaitFunction =
      std::function<bool(std::chrono::steady_clock::time_point)>;
  explicit WordEditor(X11Session &session, WaitFunction wait = {});
  ~WordEditor();
  [[nodiscard]] WordEditOutcome execute(const WordEditRequest &request);
  void pump();
  // Cancel intent, retaining completed identity but never an old edit range.
  void reset();
  // Short admission fence after a queued paste. The retained payload may
  // outlive it until the application requests it or clipboard ownership ends.
  [[nodiscard]] bool busy() const noexcept;
  [[nodiscard]] bool has_pending_paste() const noexcept;
  [[nodiscard]] std::optional<std::string>
  editor_context(std::chrono::steady_clock::time_point deadline,
                 std::string_view expected_token = {}, WaitFunction wait = {},
                 bool preparation = false);

private:
  struct PendingPaste;
  struct RetainedWordSelection;
  struct RevokedWordContext;
  X11Session &session_;
  WaitFunction wait_;
  std::unique_ptr<ClipboardManager> clipboard_;
  std::unique_ptr<DesktopLayoutClient> desktop_layout_;
  std::unique_ptr<PendingPaste> pending_;
  // Retained bytes do not confer restoration authority after proof is lost.
  std::shared_ptr<const std::vector<SelectionData>> retired_clipboard_data_;
  std::unique_ptr<RetainedWordSelection> retained_word_selection_;
  // Negative provenance only: it cannot authorize an old selection or replay.
  std::unique_ptr<RevokedWordContext> revoked_word_context_;
  std::uint64_t clipboard_session_ = 0;
};

} // namespace punto
