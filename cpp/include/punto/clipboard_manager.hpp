/**
 * @file clipboard_manager.hpp
 * @brief Bounded, fail-closed XCB clipboard owner and reader.
 */

#pragma once

#include <xcb/xcb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

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
  std::uint64_t connection_generation = 0;
};

struct SelectionData {
  xcb_atom_t target = XCB_ATOM_NONE;
  xcb_atom_t type = XCB_ATOM_NONE;
  std::uint8_t format = 0;
  std::vector<std::uint8_t> bytes{};
};
using SelectionPayload = std::shared_ptr<const std::vector<SelectionData>>;
struct SelectionSnapshot {
  SelectionRead origin;
  SelectionPayload payload; // Null only for a proven absent owner.
};
struct PayloadPublication {
  bool submitted = false;
  bool completed = false;
  std::uint64_t generation = 0;
  std::uint64_t witness_id = 0;
};

struct PasteReceiptToken {
  Selection selection = Selection::Clipboard;
  std::uint64_t id = 0;
};

enum class PasteReceiptStatus {
  Pending,
  Delivered,
  Expired,
  Invalid,
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
  /** Uses the caller's initialization deadline without changing request
   * timeouts. */
  [[nodiscard]] bool open(std::chrono::steady_clock::time_point deadline);
  void
  close(std::source_location where = std::source_location::current()) noexcept;
  [[nodiscard]] bool is_open() const noexcept;

  /**
   * Processes a bounded event batch and returns the number of consumed events.
   * Queue scanning is limited to 64 events and 2ms. At most one
   * SelectionRequest is handled per call with this manager's operation timeout.
   */
  std::size_t pump_events();

  [[nodiscard]] std::optional<std::string> get_text(Selection sel);
  /** Distinguishes an explicit owner rejection from an unanswered read. */
  [[nodiscard]] std::optional<SelectionRead>
  get_text_with_owner(Selection sel, ClipboardResult *result = nullptr);
  /** One absolute budget for initialization, conversion and identity fences. */
  [[nodiscard]] std::optional<SelectionRead>
  get_text_with_owner(Selection sel,
                      std::chrono::steady_clock::time_point deadline,
                      ClipboardResult *result = nullptr);
  /** Snapshot the exact XFixes owner identity without requesting its payload.
   */
  [[nodiscard]] std::optional<SelectionRead>
  get_selection_identity(Selection sel);
  [[nodiscard]] std::optional<SelectionRead>
  get_selection_identity(Selection sel,
                         std::chrono::steady_clock::time_point deadline);
  /** Verify an existing XFixes owner identity without requiring a text payload.
   */
  [[nodiscard]] bool selection_identity_matches(Selection sel,
                                                const SelectionRead &expected);
  [[nodiscard]] bool
  selection_identity_matches(Selection sel, const SelectionRead &expected,
                             std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::optional<bool>
  has_only_text_targets(Selection sel, const SelectionRead &expected);
  [[nodiscard]] std::optional<bool>
  has_only_text_targets(Selection sel, const SelectionRead &expected,
                        std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::optional<SelectionSnapshot>
  capture_payload(Selection sel,
                  std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] SelectionPayload text_payload(std::string_view text) const;
  [[nodiscard]] PayloadPublication
  set_payload_if_owner(Selection sel, const SelectionRead &expected,
                       SelectionPayload payload,
                       std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool
  restore_payload_if_generation(Selection sel, std::uint64_t generation,
                                const SelectionSnapshot &previous);
  /** The baseline must be retained by the caller BEFORE set_payload_if_owner.
   * Closed publishing connection or owner NONE alone never authorizes this. */
  [[nodiscard]] bool restore_payload_after_uncertain(
      Selection sel, const PayloadPublication &publication,
      const SelectionSnapshot &previous,
      std::chrono::steady_clock::time_point deadline);
  void
  cancel_publication_witness(const PayloadPublication &publication) noexcept;
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
  /** Arms a paste receipt bound to the exact PRIMARY context being edited. */
  [[nodiscard]] std::optional<PasteReceiptToken>
  arm_paste_receipt(Selection sel, const SelectionRead &source_context);
  void activate_paste_receipt(
      const PasteReceiptToken &token,
      std::chrono::steady_clock::time_point deadline) noexcept;
  [[nodiscard]] PasteReceiptStatus
  paste_receipt_status(const PasteReceiptToken &token) const noexcept;
  [[nodiscard]] bool
  paste_receipt_seen(const PasteReceiptToken &token) const noexcept;
  /** Successful text deliveries to the client bound to this receipt. */
  [[nodiscard]] std::uint64_t
  paste_receipt_request_seq(const PasteReceiptToken &token) const noexcept;
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

  /** Publish only under the same connection, owner and XFixes generation.
   * A lost acknowledgement can return false after submission. That connection
   * is closed; this never authorizes a blind retry or a claim of no
   * publication.
   */
  [[nodiscard]] bool set_text_if_owner(Selection sel,
                                       const SelectionRead &expected,
                                       std::string_view text);
  [[nodiscard]] bool
  set_text_if_owner(Selection sel, const SelectionRead &expected,
                    std::string_view text,
                    std::chrono::steady_clock::time_point deadline);
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
    SelectionPayload payload;
    bool owns = false;
    std::uint32_t publication_sequence = 0;
    std::uint64_t generation = 0;
    std::uint64_t request_sequence = 0;
    std::uint64_t armed_receipt_id = 0;
    std::uint64_t armed_generation = 0;
    std::uint32_t expected_requestor_client = 0;
    xcb_window_t receipt_context_owner = XCB_WINDOW_NONE;
    xcb_timestamp_t receipt_context_timestamp = XCB_CURRENT_TIME;
    std::uint64_t receipt_context_owner_generation = 0;
    std::uint64_t receipt_context_connection_generation = 0;
    std::uint64_t confirmed_receipt_id = 0;
    std::uint64_t receipt_request_sequence = 0;
    std::chrono::steady_clock::time_point receipt_deadline =
        std::chrono::steady_clock::time_point::max();
    xcb_window_t observed_owner = XCB_WINDOW_NONE;
    xcb_timestamp_t observed_selection_timestamp = XCB_CURRENT_TIME;
    bool baseline_initialized = false;
    std::uint8_t observed_subtype = 255;
    xcb_window_t previous_observed_owner = XCB_WINDOW_NONE;
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

  struct IncrementalTransfer {
    Selection selection;
    xcb_window_t requestor;
    xcb_atom_t property;
    SelectionPayload payload;
    std::size_t entry = 0;
    std::size_t offset = 0;
    std::uint64_t generation = 0;
    std::uint64_t receipt_id = 0;
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t id = 0;
  };
  struct DeferredPeerEvent {
    xcb_generic_event_t event;
    Selection selection;
    std::uint64_t connection_generation;
    std::uint64_t generation;
    std::uint64_t receipt_id;
    std::uint64_t transfer_id;
    std::chrono::steady_clock::time_point deadline;
  };
  struct PublicationProof {
    std::uint64_t id = 0;
    Selection selection;
    xcb_window_t publisher;
    xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
    std::uint64_t witness_generation = 0;
    std::uint64_t witness_connection_generation = 0;
    std::uint64_t session_generation = 0;
    std::chrono::steady_clock::time_point deadline;
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
                        std::chrono::steady_clock::time_point deadline,
                        bool initialize_baselines = true);
  [[nodiscard]] std::optional<PasteReceiptToken>
  arm_paste_receipt_impl(Selection sel, const SelectionRead *source_context);
  [[nodiscard]] xcb_atom_t
  intern_atom(std::string_view name,
              std::chrono::steady_clock::time_point deadline,
              bool only_if_exists = false);
  [[nodiscard]] xcb_atom_t selection_atom(Selection sel) const noexcept;
  [[nodiscard]] SelectionState &selection_state(Selection sel) noexcept;
  [[nodiscard]] const SelectionState &
  selection_state(Selection sel) const noexcept;
  [[nodiscard]] bool connection_healthy() const noexcept;
  [[nodiscard]] bool event_budget_available() const noexcept;
  void record_polled_event() noexcept;
  [[nodiscard]] bool
  current_connection(const SelectionRead &read) const noexcept;
  enum class RequestScan { Yield, FinishQueuedHistory };
  enum class ReplyIntent { Fence, Observation };
  [[nodiscard]] bool process_event(
      const xcb_generic_event_t &event,
      std::chrono::steady_clock::time_point selection_request_deadline,
      RequestScan requests = RequestScan::Yield);
  [[nodiscard]] std::size_t
  pump_events_until(std::chrono::steady_clock::time_point operation_deadline);
  void service_deferred_peer_event(
      std::chrono::steady_clock::time_point operation_deadline);
  [[nodiscard]] bool
  drain_queued_events_to_fence(std::chrono::steady_clock::time_point deadline,
                               RequestScan requests = RequestScan::Yield,
                               xcb_generic_event_t **guarded_request = nullptr);
  [[nodiscard]] std::optional<xcb_window_t>
  active_window(std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::uint32_t
  resource_client_id(xcb_window_t window) const noexcept;
  [[nodiscard]] bool
  wait_for_reply(unsigned int sequence,
                 std::chrono::steady_clock::time_point deadline, void **reply,
                 xcb_generic_error_t **error,
                 ReplyIntent intent = ReplyIntent::Fence);
  [[nodiscard]] bool
  complete_checked_request(xcb_void_cookie_t cookie,
                           std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool
  initialize_selection_baseline(Selection sel,
                                std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::optional<xcb_timestamp_t>
  server_timestamp(std::chrono::steady_clock::time_point deadline,
                   xcb_timestamp_t previous = XCB_CURRENT_TIME,
                   RequestScan requests = RequestScan::Yield);
  void fail_closed(
      std::source_location where = std::source_location::current()) noexcept;
  void close_impl(std::source_location where, bool failure) noexcept;

  [[nodiscard]] static bool
  event_precedes_publication(const SelectionState &state,
                             std::uint32_t sequence) noexcept;

  [[nodiscard]] bool handle_selection_request(
      const xcb_selection_request_event_t &request, std::uint32_t sequence,
      std::chrono::steady_clock::time_point deadline, bool current = true);
  struct PeerRequest {
    xcb_void_cookie_t cookie;
    std::uint8_t opcode;
  };
  enum class PeerCompletion { Delivered, Expired, Gone, Fatal };
  [[nodiscard]] PeerCompletion
  complete_peer_requests(xcb_window_t requestor,
                         std::initializer_list<PeerRequest> requests,
                         std::chrono::steady_clock::time_point deadline);
  void handle_selection_clear(const xcb_selection_clear_event_t &event,
                              std::uint32_t sequence) noexcept;
  void handle_owner_change(const xcb_generic_event_t &event) noexcept;
  [[nodiscard]] std::optional<xcb_window_t>
  request_selection_conversion(Selection sel, xcb_atom_t target,
                               xcb_timestamp_t timestamp,
                               std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::optional<xcb_selection_notify_event_t>
  wait_for_selection_notify(xcb_window_t requestor, xcb_atom_t selection,
                            xcb_atom_t target, xcb_atom_t property,
                            std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::optional<xcb_generic_event_t>
  wait_for_query_event(xcb_window_t requestor, xcb_atom_t selection,
                       xcb_atom_t target, xcb_atom_t property,
                       std::uint8_t event_type,
                       std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::optional<SelectionData>
  read_target(Selection sel, xcb_atom_t target, const SelectionRead &origin,
              std::chrono::steady_clock::time_point deadline,
              ClipboardResult *result);
  [[nodiscard]] bool
  portable_target(xcb_atom_t target,
                  std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool
  handle_incremental_property(const xcb_property_notify_event_t &event,
                              std::chrono::steady_clock::time_point deadline);
  void release_transfer_watch(xcb_window_t requestor,
                              std::chrono::steady_clock::time_point deadline);
  void finish_transfer(Selection sel, std::uint64_t generation,
                       std::uint64_t receipt_id,
                       xcb_window_t requestor) noexcept;
  [[nodiscard]] bool
  prepare_publication_witness(Selection sel, const SelectionRead &expected,
                              std::chrono::steady_clock::time_point deadline,
                              bool reuse = false);
  void adopt_publication_witness() noexcept;
  [[nodiscard]] static std::optional<std::string>
  decode_text_property(const PropertyBytes &property, xcb_atom_t expected_type,
                       xcb_atom_t incr_atom);
  [[nodiscard]] std::size_t maximum_payload_bytes() const noexcept;
  [[nodiscard]] bool
  publish_selection(Selection sel, std::optional<std::string_view> text,
                    xcb_timestamp_t timestamp,
                    std::chrono::steady_clock::time_point deadline,
                    const SelectionRead *expected = nullptr);
  [[nodiscard]] bool
  publish_payload(Selection sel, SelectionPayload payload,
                  xcb_timestamp_t timestamp,
                  std::chrono::steady_clock::time_point deadline,
                  const SelectionRead *expected = nullptr,
                  PayloadPublication *publication = nullptr);
  [[nodiscard]] bool restore_payload(Selection sel, std::uint64_t generation,
                                     SelectionPayload payload);

  X11Session *session_ = nullptr;
  std::chrono::milliseconds timeout_;

  BoundedXcbConnection connection_owner_;
  xcb_connection_t *connection_ = nullptr;
  xcb_screen_t *screen_ = nullptr;
  xcb_window_t window_ = XCB_WINDOW_NONE;
  std::uint64_t connection_generation_ = 0;
  std::uint64_t connection_session_generation_ = 0;

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
  xcb_atom_t atom_server_time_ = XCB_ATOM_NONE;
  std::uint8_t xfixes_event_base_ = 0;
  std::uint64_t next_receipt_id_ = 0;
  std::uint64_t next_transition_id_ = 0;
  bool handling_selection_request_ = false;
  std::optional<std::chrono::steady_clock::time_point>
      negative_response_deadline_;

  std::vector<IncrementalTransfer> outgoing_;
  std::uint64_t next_transfer_id_ = 0;
  std::deque<DeferredPeerEvent> deferred_peer_events_;
  std::optional<std::size_t> event_budget_;
  std::vector<xcb_window_t> retired_transfer_watches_;
  std::unique_ptr<ClipboardManager> publication_witness_;
  std::optional<PublicationProof> publication_proof_;
  std::uint64_t next_publication_id_ = 0;

  SelectionState clipboard_;
  SelectionState primary_;
};

} // namespace punto
