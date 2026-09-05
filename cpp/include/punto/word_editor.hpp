#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>

namespace punto {

class X11Session;
class ClipboardManager;

enum class WordEditOperation {
  Word,
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
};

enum class WordEditStatus {
  Rejected,
  PreparedNotReplayed,
  Dispatched,
  PartialFailure,
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
  void reset();
  [[nodiscard]] bool busy() const noexcept;

private:
  struct PendingPaste;
  X11Session &session_;
  WaitFunction wait_;
  std::unique_ptr<ClipboardManager> clipboard_;
  std::unique_ptr<PendingPaste> pending_;
  std::uint64_t clipboard_session_ = 0;
};

} // namespace punto
