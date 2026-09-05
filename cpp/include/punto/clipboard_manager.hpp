/**
 * @file clipboard_manager.hpp
 * @brief Bounded, fail-closed XCB clipboard owner and reader.
 */

#pragma once

#include <xcb/xcb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "punto/types.hpp"
#include "punto/x11_session.hpp"

namespace punto {

enum class Selection {
  Primary,
  Clipboard,
};

enum class ActiveWindowKind {
  Gui,
  Terminal,
  Unknown,
};

struct SelectionRead {
  std::string text;
  std::uint32_t owner = XCB_WINDOW_NONE;
  std::uint32_t selection_timestamp = XCB_CURRENT_TIME;
  std::uint64_t owner_generation = 0;
};

struct PasteReceiptToken {
  Selection selection = Selection::Clipboard;
  std::uint64_t id = 0;
};

struct OwnerTransitionToken {
  Selection selection = Selection::Primary;
  std::uint64_t id = 0;
};

enum class OwnerTransitionStatus {
  Pending,
  Released,
  SameClientOwner,
  Ambiguous,
  Invalid,
};

struct ClipboardManagerTestAccess;

/**
 * Owns the X11 CLIPBOARD/PRIMARY selections and serves them through XCB.
 *
 * The manager is deliberately single-threaded. All ownership and delivery
 * state transitions happen while EventLoop calls this object, which makes a
 * generation a reliable restore guard.
 */
class ClipboardManager {
public:
  explicit ClipboardManager(
      X11Session &session,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{500});
  ~ClipboardManager();

  ClipboardManager(const ClipboardManager &) = delete;
  ClipboardManager &operator=(const ClipboardManager &) = delete;

  [[nodiscard]] bool open();
  /** Uses the caller's initialization deadline without changing request timeouts. */
  [[nodiscard]] bool open(std::chrono::steady_clock::time_point deadline);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Processes a bounded event batch and returns the number of consumed events.
   * A single call never intentionally consumes more than 64 events or spends
   * more than 2ms in the dispatch loop.
   */
  std::size_t pump_events();

  [[nodiscard]] std::optional<std::string> get_text(Selection sel);
  [[nodiscard]] std::optional<SelectionRead> get_text_with_owner(Selection sel);
  [[nodiscard]] std::optional<bool>
  has_only_text_targets(Selection sel, const SelectionRead &expected);
  ClipboardResult set_text(Selection sel, std::string_view text);
  [[nodiscard]] ActiveWindowKind active_window_kind();
  [[nodiscard]] std::optional<std::uint32_t> active_client_id();

  /**
   * Opaque delivery receipt used by EventLoop. It changes only after a text
   * payload for the current generation has been queued successfully; TARGETS
   * and unsupported targets never advance it.
   */
  [[nodiscard]] std::uint64_t
  selection_request_seq(Selection sel) const noexcept;

  /** Arms one paste receipt for the active X11 client connection. */
  [[nodiscard]] std::optional<PasteReceiptToken>
  arm_paste_receipt(Selection sel);
  [[nodiscard]] bool
  paste_receipt_seen(const PasteReceiptToken &token) const noexcept;
  void cancel_paste_receipt(const PasteReceiptToken &token) noexcept;

  [[nodiscard]] std::optional<OwnerTransitionToken>
  arm_owner_transition(Selection sel, const SelectionRead &source,
                       std::uint32_t expected_client);
  [[nodiscard]] OwnerTransitionStatus
  owner_transition_status(const OwnerTransitionToken &token) const noexcept;
  void cancel_owner_transition(const OwnerTransitionToken &token) noexcept;
  [[nodiscard]] bool
  restore_selection_after_owner_transition(const OwnerTransitionToken &token,
                                           const SelectionRead &previous);

  /** Current locally-owned generation, or the last generation after loss. */
  [[nodiscard]] std::uint64_t
  selection_generation(Selection sel) const noexcept;
  [[nodiscard]] std::uint64_t
  selection_owner_generation(Selection sel) const noexcept;

  /**
   * Restores text only if the selection is still owned at expected_generation.
   * This prevents a delayed restore from overwriting a newer local value.
   */
  [[nodiscard]] bool
  restore_text_if_generation(Selection sel, std::uint64_t expected_generation,
                             std::string_view text);

  /** Set text using the exact XFixes owner timestamp as a server-side fence. */
  [[nodiscard]] bool set_text_if_owner(Selection sel,
                                       const SelectionRead &expected,
                                       std::string_view text);
  [[nodiscard]] bool restore_selection_if_owner_from_client(
      Selection sel, const SelectionRead &expected_current,
      const SelectionRead &previous, std::uint32_t expected_client);
  [[nodiscard]] bool restore_selection_after_client_release(
      Selection sel, const SelectionRead &released,
      const SelectionRead &previous, std::uint32_t expected_client);

  /** Confirm that the selection is still owned at a local generation. */
  [[nodiscard]] bool owns_generation(Selection sel, std::uint64_t generation);
  [[nodiscard]] bool
  locally_owns_generation(Selection sel,
                          std::uint64_t generation) const noexcept;

  [[nodiscard]] bool verify_ownership();

private:
  friend struct ClipboardManagerTestAccess;

  struct SelectionState {
    std::string text;
    bool owns = false;
    std::uint64_t generation = 0;
    std::uint64_t request_sequence = 0;
    std::uint64_t armed_receipt_id = 0;
    std::uint64_t armed_generation = 0;
    std::uint32_t expected_requestor_client = 0;
    std::uint64_t confirmed_receipt_id = 0;
    xcb_window_t observed_owner = XCB_WINDOW_NONE;
    xcb_timestamp_t observed_selection_timestamp = XCB_CURRENT_TIME;
    bool baseline_initialized = false;
    std::uint64_t owner_generation = 0;
    std::uint64_t owner_identity_generation = 0;
    std::uint64_t armed_transition_id = 0;
    xcb_window_t transition_source_owner = XCB_WINDOW_NONE;
    std::uint32_t transition_expected_client = 0;
    OwnerTransitionStatus transition_status = OwnerTransitionStatus::Invalid;
    xcb_window_t transition_owner = XCB_WINDOW_NONE;
    xcb_timestamp_t transition_timestamp = XCB_CURRENT_TIME;
    std::uint64_t transition_identity_generation = 0;
  };

  struct PropertyBytes {
    xcb_atom_t type = XCB_ATOM_NONE;
    std::uint8_t format = 0;
    std::uint32_t bytes_after = 0;
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
  };

  ClipboardManager(xcb_connection_t *connection, int screen_number,
                   std::chrono::milliseconds timeout);

  [[nodiscard]] bool
  initialize_connection(BoundedXcbConnection connection,
                        std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] xcb_atom_t
  intern_atom(std::string_view name,
              std::chrono::steady_clock::time_point deadline,
              bool only_if_exists = false);
  [[nodiscard]] xcb_atom_t selection_atom(Selection sel) const noexcept;
  [[nodiscard]] SelectionState &selection_state(Selection sel) noexcept;
  [[nodiscard]] const SelectionState &
  selection_state(Selection sel) const noexcept;
  [[nodiscard]] bool connection_healthy() const noexcept;
  [[nodiscard]] std::optional<xcb_window_t>
  active_window(std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::uint32_t
  resource_client_id(xcb_window_t window) const noexcept;
  [[nodiscard]] bool
  wait_for_reply(unsigned int sequence,
                 std::chrono::steady_clock::time_point deadline, void **reply,
                 xcb_generic_error_t **error);
  [[nodiscard]] bool
  complete_checked_request(xcb_void_cookie_t cookie,
                           std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool
  initialize_selection_baseline(Selection sel,
                                std::chrono::steady_clock::time_point deadline);
  void fail_closed() noexcept;

  [[nodiscard]] bool
  handle_selection_request(const xcb_selection_request_event_t &request,
                           std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool
  complete_payload_transfer(xcb_void_cookie_t property_cookie,
                            xcb_void_cookie_t notify_cookie,
                            std::chrono::steady_clock::time_point deadline);
  void
  handle_selection_clear(const xcb_selection_clear_event_t &event) noexcept;
  void handle_owner_change(const xcb_generic_event_t &event) noexcept;
  [[nodiscard]] std::optional<xcb_selection_notify_event_t>
  wait_for_selection_notify(xcb_atom_t selection, xcb_atom_t target,
                            xcb_atom_t property,
                            std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] static std::optional<std::string>
  decode_text_property(const PropertyBytes &property, xcb_atom_t expected_type,
                       xcb_atom_t incr_atom);
  [[nodiscard]] std::size_t maximum_payload_bytes() const noexcept;
  [[nodiscard]] bool
  set_text_impl(Selection sel, std::string_view text,
                xcb_timestamp_t timestamp = XCB_CURRENT_TIME);

  X11Session *session_ = nullptr;
  std::chrono::milliseconds timeout_;

  BoundedXcbConnection connection_owner_;
  xcb_connection_t *connection_ = nullptr;
  xcb_screen_t *screen_ = nullptr;
  xcb_window_t window_ = XCB_WINDOW_NONE;

  xcb_atom_t atom_clipboard_ = XCB_ATOM_NONE;
  xcb_atom_t atom_primary_ = XCB_ATOM_PRIMARY;
  xcb_atom_t atom_utf8_string_ = XCB_ATOM_NONE;
  xcb_atom_t atom_targets_ = XCB_ATOM_NONE;
  xcb_atom_t atom_text_plain_ = XCB_ATOM_NONE;
  xcb_atom_t atom_text_plain_utf8_ = XCB_ATOM_NONE;
  xcb_atom_t atom_compound_text_ = XCB_ATOM_NONE;
  xcb_atom_t atom_text_ = XCB_ATOM_NONE;
  xcb_atom_t atom_timestamp_ = XCB_ATOM_NONE;
  xcb_atom_t atom_multiple_ = XCB_ATOM_NONE;
  xcb_atom_t atom_save_targets_ = XCB_ATOM_NONE;
  xcb_atom_t atom_incr_ = XCB_ATOM_NONE;
  xcb_atom_t atom_wm_class_ = XCB_ATOM_WM_CLASS;
  xcb_atom_t atom_net_active_window_ = XCB_ATOM_NONE;
  xcb_atom_t atom_read_property_ = XCB_ATOM_NONE;
  std::uint8_t xfixes_event_base_ = 0;
  std::uint64_t next_receipt_id_ = 0;
  std::uint64_t next_transition_id_ = 0;

  SelectionState clipboard_;
  SelectionState primary_;
};

} // namespace punto
