/**
 * @file clipboard_manager.cpp
 * @brief Bounded, fail-closed XCB clipboard implementation.
 */

#include "punto/clipboard_manager.hpp"
#include "punto/terminal_detection.hpp"
#include "punto/x11_session.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <utility>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>

namespace punto {

namespace {

constexpr std::size_t kPumpEventBudget = 64;
constexpr auto kPumpTimeBudget = std::chrono::milliseconds{2};
// Synchronous XCB transfers do not implement INCR. Keep one property plus the
// checked-request barrier below Linux's minimum Unix socket send buffer so a
// stopped X server cannot make libxcb enter its unbounded POLLOUT wait.
constexpr std::size_t kMaxClipboardBytes = 4096U;
constexpr std::size_t kMaxWmClassBytes = 4096;
constexpr std::string_view kSafeGuiWmClass = "gedit";

template <typename T> using XcbPtr = std::unique_ptr<T, decltype(&std::free)>;

template <typename T> XcbPtr<T> xcb_ptr(T *pointer) {
  return XcbPtr<T>{pointer, &std::free};
}

[[nodiscard]] std::uint64_t next_generation(std::uint64_t current) noexcept {
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    return 1;
  }
  return current + 1;
}

[[nodiscard]] bool
equals_ascii_case_insensitive(std::string_view lhs,
                              std::string_view rhs) noexcept {
  return lhs.size() == rhs.size() &&
         std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](char left, char right) {
                      const auto fold = [](char value) {
                        return value >= 'A' && value <= 'Z'
                                   ? static_cast<char>(value + ('a' - 'A'))
                                   : value;
                      };
                      return fold(left) == fold(right);
                    });
}

[[nodiscard]] bool is_safe_gui_wm_class(std::string_view instance,
                                        std::string_view klass) noexcept {
  return equals_ascii_case_insensitive(instance, kSafeGuiWmClass) ||
         equals_ascii_case_insensitive(klass, kSafeGuiWmClass);
}

[[nodiscard]] int
bounded_poll_timeout(std::chrono::steady_clock::time_point deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining >= std::chrono::milliseconds{5}) {
    return 5;
  }
  return std::max(static_cast<int>(remaining.count()) + 1, 1);
}

} // namespace

ClipboardManager::ClipboardManager(X11Session &session,
                                   std::chrono::milliseconds timeout)
    : session_{&session},
      timeout_{std::max(timeout, std::chrono::milliseconds{0})} {}

ClipboardManager::ClipboardManager(xcb_connection_t *connection,
                                   int screen_number,
                                   std::chrono::milliseconds timeout)
    : timeout_{std::max(timeout, std::chrono::milliseconds{0})} {
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  (void)initialize_connection(BoundedXcbConnection{connection, screen_number},
                              deadline);
}

ClipboardManager::~ClipboardManager() { close(); }

bool ClipboardManager::open() {
  if (is_open()) {
    return true;
  }
  close();
  if (session_ == nullptr) {
    return false;
  }

  auto lease = session_->acquire_write_lease();
  if (!lease) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  return initialize_connection(lease->open_bounded_connection(deadline),
                               deadline);
}

bool ClipboardManager::initialize_connection(
    BoundedXcbConnection connection,
    std::chrono::steady_clock::time_point deadline) {
  if (!connection.is_open() || connection.screen_number() < 0) {
    return false;
  }

  connection_owner_ = std::move(connection);
  connection_ = connection_owner_.get();
  const int screen_number = connection_owner_.screen_number();
  const xcb_setup_t *setup = xcb_get_setup(connection_);
  if (setup == nullptr) {
    fail_closed();
    return false;
  }

  xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
  for (int index = 0; index < screen_number && screens.rem > 0; ++index) {
    xcb_screen_next(&screens);
  }
  if (screens.rem <= 0 || screens.data == nullptr) {
    fail_closed();
    return false;
  }
  screen_ = screens.data;
  window_ = xcb_generate_id(connection_);
  const std::uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  const auto create_cookie = xcb_create_window_checked(
      connection_, XCB_COPY_FROM_PARENT, window_, screen_->root, 0, 0, 1, 1, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, XCB_CW_EVENT_MASK,
      &event_mask);
  if (!complete_checked_request(create_cookie, deadline)) {
    fail_closed();
    return false;
  }

  atom_clipboard_ = intern_atom("CLIPBOARD", deadline);
  atom_utf8_string_ = intern_atom("UTF8_STRING", deadline);
  atom_targets_ = intern_atom("TARGETS", deadline);
  atom_text_plain_ = intern_atom("text/plain", deadline);
  atom_text_plain_utf8_ = intern_atom("text/plain;charset=utf-8", deadline);
  atom_compound_text_ = intern_atom("COMPOUND_TEXT", deadline);
  atom_text_ = intern_atom("TEXT", deadline);
  atom_timestamp_ = intern_atom("TIMESTAMP", deadline);
  atom_multiple_ = intern_atom("MULTIPLE", deadline);
  atom_save_targets_ = intern_atom("SAVE_TARGETS", deadline);
  atom_incr_ = intern_atom("INCR", deadline);
  atom_net_active_window_ = intern_atom("_NET_ACTIVE_WINDOW", deadline);
  atom_read_property_ = intern_atom("PUNTO_SELECTION_READ", deadline);

  const xcb_query_extension_reply_t *xfixes =
      xcb_get_extension_data(connection_, &xcb_xfixes_id);
  if (xfixes == nullptr || xfixes->present == 0) {
    fail_closed();
    return false;
  }
  xfixes_event_base_ = xfixes->first_event;
  const auto version_cookie = xcb_xfixes_query_version(connection_, 5, 0);
  void *raw_version = nullptr;
  xcb_generic_error_t *raw_version_error = nullptr;
  if (!wait_for_reply(version_cookie.sequence, deadline, &raw_version,
                      &raw_version_error)) {
    fail_closed();
    return false;
  }
  auto version =
      xcb_ptr(static_cast<xcb_xfixes_query_version_reply_t *>(raw_version));
  auto version_error = xcb_ptr(raw_version_error);
  constexpr std::uint32_t owner_mask =
      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
  const auto clipboard_watch = xcb_xfixes_select_selection_input_checked(
      connection_, window_, atom_clipboard_, owner_mask);
  const auto primary_watch = xcb_xfixes_select_selection_input_checked(
      connection_, window_, atom_primary_, owner_mask);

  if (atom_clipboard_ == XCB_ATOM_NONE || atom_utf8_string_ == XCB_ATOM_NONE ||
      atom_targets_ == XCB_ATOM_NONE || atom_text_plain_ == XCB_ATOM_NONE ||
      atom_text_plain_utf8_ == XCB_ATOM_NONE || atom_incr_ == XCB_ATOM_NONE ||
      atom_compound_text_ == XCB_ATOM_NONE || atom_text_ == XCB_ATOM_NONE ||
      atom_timestamp_ == XCB_ATOM_NONE || atom_multiple_ == XCB_ATOM_NONE ||
      atom_save_targets_ == XCB_ATOM_NONE ||
      atom_read_property_ == XCB_ATOM_NONE || !version || version_error ||
      !complete_checked_request(clipboard_watch, deadline) ||
      !complete_checked_request(primary_watch, deadline) ||
      !connection_healthy()) {
    fail_closed();
    return false;
  }
  if (!initialize_selection_baseline(Selection::Clipboard, deadline) ||
      !initialize_selection_baseline(Selection::Primary, deadline)) {
    fail_closed();
    return false;
  }
  return true;
}

xcb_atom_t
ClipboardManager::intern_atom(std::string_view name,
                              std::chrono::steady_clock::time_point deadline,
                              bool only_if_exists) {
  if (!connection_healthy() ||
      name.size() > std::numeric_limits<std::uint16_t>::max()) {
    return XCB_ATOM_NONE;
  }
  const auto cookie =
      xcb_intern_atom(connection_, only_if_exists ? 1U : 0U,
                      static_cast<std::uint16_t>(name.size()), name.data());
  void *raw_reply = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw_reply, &raw_error)) {
    return XCB_ATOM_NONE;
  }
  auto reply = xcb_ptr(static_cast<xcb_intern_atom_reply_t *>(raw_reply));
  auto error = xcb_ptr(raw_error);
  if (error || !reply) {
    return XCB_ATOM_NONE;
  }
  return reply->atom;
}

void ClipboardManager::close() noexcept {
  if (clipboard_.armed_transition_id != 0) {
    cancel_owner_transition(OwnerTransitionToken{
        Selection::Clipboard, clipboard_.armed_transition_id});
  }
  if (primary_.armed_transition_id != 0) {
    cancel_owner_transition(
        OwnerTransitionToken{Selection::Primary, primary_.armed_transition_id});
  }
  clipboard_.owns = false;
  clipboard_.text.clear();
  clipboard_.generation = next_generation(clipboard_.generation);
  clipboard_.armed_receipt_id = 0;
  clipboard_.armed_generation = 0;
  clipboard_.expected_requestor_client = 0;
  clipboard_.observed_owner = XCB_WINDOW_NONE;
  clipboard_.observed_selection_timestamp = XCB_CURRENT_TIME;
  clipboard_.baseline_initialized = false;
  clipboard_.owner_generation = next_generation(clipboard_.owner_generation);
  clipboard_.owner_identity_generation =
      next_generation(clipboard_.owner_identity_generation);
  primary_.owns = false;
  primary_.text.clear();
  primary_.generation = next_generation(primary_.generation);
  primary_.armed_receipt_id = 0;
  primary_.armed_generation = 0;
  primary_.expected_requestor_client = 0;
  primary_.observed_owner = XCB_WINDOW_NONE;
  primary_.observed_selection_timestamp = XCB_CURRENT_TIME;
  primary_.baseline_initialized = false;
  primary_.owner_generation = next_generation(primary_.owner_generation);
  primary_.owner_identity_generation =
      next_generation(primary_.owner_identity_generation);

  // Closing the XCB connection destroys all owned resources. Do not send a
  // final request here: teardown must remain bounded when the X server is
  // stalled or the session write gate has already been revoked.
  connection_owner_.close();
  connection_ = nullptr;
  screen_ = nullptr;
  window_ = XCB_WINDOW_NONE;
  xfixes_event_base_ = 0;
}

bool ClipboardManager::is_open() const noexcept {
  return connection_healthy() && screen_ != nullptr &&
         window_ != XCB_WINDOW_NONE;
}

bool ClipboardManager::connection_healthy() const noexcept {
  return connection_owner_.is_open() && connection_ == connection_owner_.get();
}

bool ClipboardManager::wait_for_reply(
    unsigned int sequence, std::chrono::steady_clock::time_point deadline,
    void **reply, xcb_generic_error_t **error) {
  if (reply == nullptr || error == nullptr || !connection_healthy()) {
    return false;
  }
  *reply = nullptr;
  *error = nullptr;
  x11_detail::XcbOperationResult result{};
  *reply = connection_owner_.wait_for_reply(sequence, deadline, result);
  *error = nullptr;
  return result == x11_detail::XcbOperationResult::Success;
}

bool ClipboardManager::complete_checked_request(
    xcb_void_cookie_t cookie, std::chrono::steady_clock::time_point deadline) {
  x11_detail::XcbOperationResult result{};
  return connection_owner_.check_request(cookie, deadline, result);
}

bool ClipboardManager::initialize_selection_baseline(
    Selection sel, std::chrono::steady_clock::time_point deadline) {
  SelectionState &state = selection_state(sel);
  state.baseline_initialized = false;
  state.observed_owner = XCB_WINDOW_NONE;
  state.observed_selection_timestamp = XCB_CURRENT_TIME;

  while (connection_healthy() && std::chrono::steady_clock::now() < deadline) {
    const auto owner_cookie =
        xcb_get_selection_owner(connection_, selection_atom(sel));
    void *raw_owner = nullptr;
    xcb_generic_error_t *raw_error = nullptr;
    if (!wait_for_reply(owner_cookie.sequence, deadline, &raw_owner,
                        &raw_error)) {
      return false;
    }
    auto owner =
        xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
    auto error = xcb_ptr(raw_error);
    if (error || !owner || !connection_healthy()) {
      return false;
    }

    (void)pump_events();
    if (owner->owner == XCB_WINDOW_NONE) {
      if (!state.baseline_initialized) {
        state.observed_owner = XCB_WINDOW_NONE;
        state.observed_selection_timestamp = XCB_CURRENT_TIME;
        state.baseline_initialized = true;
      }
      if (state.observed_owner == XCB_WINDOW_NONE) {
        return true;
      }
    } else if (state.baseline_initialized &&
               state.observed_owner == owner->owner &&
               state.observed_selection_timestamp != XCB_CURRENT_TIME) {
      return true;
    } else {
      if (!complete_checked_request(
              xcb_delete_property_checked(connection_, window_,
                                          atom_read_property_),
              deadline) ||
          !complete_checked_request(xcb_convert_selection_checked(
                                        connection_, window_,
                                        selection_atom(sel), atom_timestamp_,
                                        atom_read_property_, XCB_CURRENT_TIME),
                                    deadline)) {
        return false;
      }
      const auto notify = wait_for_selection_notify(
          selection_atom(sel), atom_timestamp_, atom_read_property_, deadline);
      if (!notify || notify->property == XCB_ATOM_NONE) {
        return false;
      }

      const auto timestamp_cookie = xcb_get_property(
          connection_, 1, window_, atom_read_property_, XCB_ATOM_INTEGER, 0, 1);
      void *raw_timestamp = nullptr;
      xcb_generic_error_t *raw_timestamp_error = nullptr;
      if (!wait_for_reply(timestamp_cookie.sequence, deadline, &raw_timestamp,
                          &raw_timestamp_error)) {
        return false;
      }
      auto timestamp_reply =
          xcb_ptr(static_cast<xcb_get_property_reply_t *>(raw_timestamp));
      auto timestamp_error = xcb_ptr(raw_timestamp_error);
      if (timestamp_error || !timestamp_reply ||
          timestamp_reply->type != XCB_ATOM_INTEGER ||
          timestamp_reply->format != 32 || timestamp_reply->bytes_after != 0 ||
          xcb_get_property_value_length(timestamp_reply.get()) != 4) {
        return false;
      }
      xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
      std::memcpy(&timestamp, xcb_get_property_value(timestamp_reply.get()),
                  sizeof(timestamp));
      if (timestamp == XCB_CURRENT_TIME) {
        return false;
      }

      const auto confirm_cookie =
          xcb_get_selection_owner(connection_, selection_atom(sel));
      void *raw_confirm = nullptr;
      xcb_generic_error_t *raw_confirm_error = nullptr;
      if (!wait_for_reply(confirm_cookie.sequence, deadline, &raw_confirm,
                          &raw_confirm_error)) {
        return false;
      }
      auto confirmed =
          xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_confirm));
      auto confirm_error = xcb_ptr(raw_confirm_error);
      if (confirm_error || !confirmed || confirmed->owner != owner->owner) {
        continue;
      }

      if (!state.baseline_initialized ||
          state.observed_owner == XCB_WINDOW_NONE) {
        state.observed_owner = owner->owner;
        state.observed_selection_timestamp = timestamp;
        state.baseline_initialized = true;
        state.owner_generation = next_generation(state.owner_generation);
        state.owner_identity_generation =
            next_generation(state.owner_identity_generation);
      }
      (void)pump_events();
      if (state.observed_owner == owner->owner &&
          state.observed_selection_timestamp == timestamp) {
        return true;
      }
    }

    pollfd descriptor{xcb_get_file_descriptor(connection_), POLLIN, 0};
    const int result = ::poll(&descriptor, 1, bounded_poll_timeout(deadline));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 || (result > 0 && (descriptor.revents &
                                      (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
      return false;
    }
  }
  return false;
}

void ClipboardManager::fail_closed() noexcept { close(); }

xcb_atom_t ClipboardManager::selection_atom(Selection sel) const noexcept {
  return sel == Selection::Primary ? atom_primary_ : atom_clipboard_;
}

ClipboardManager::SelectionState &
ClipboardManager::selection_state(Selection sel) noexcept {
  return sel == Selection::Primary ? primary_ : clipboard_;
}

const ClipboardManager::SelectionState &
ClipboardManager::selection_state(Selection sel) const noexcept {
  return sel == Selection::Primary ? primary_ : clipboard_;
}

std::uint64_t
ClipboardManager::selection_request_seq(Selection sel) const noexcept {
  return selection_state(sel).request_sequence;
}

std::optional<PasteReceiptToken>
ClipboardManager::arm_paste_receipt(Selection sel) {
  if (!is_open()) {
    return std::nullopt;
  }
  SelectionState &state = selection_state(sel);
  if (!state.owns) {
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto active = active_window(deadline);
  if (!active) {
    return std::nullopt;
  }
  const std::uint32_t active_client = resource_client_id(*active);
  if (active_client == 0) {
    return std::nullopt;
  }
  next_receipt_id_ = next_generation(next_receipt_id_);
  state.armed_receipt_id = next_receipt_id_;
  state.armed_generation = state.generation;
  state.expected_requestor_client = active_client;
  return PasteReceiptToken{sel, next_receipt_id_};
}

bool ClipboardManager::paste_receipt_seen(
    const PasteReceiptToken &token) const noexcept {
  return token.id != 0 &&
         selection_state(token.selection).confirmed_receipt_id == token.id;
}

void ClipboardManager::cancel_paste_receipt(
    const PasteReceiptToken &token) noexcept {
  SelectionState &state = selection_state(token.selection);
  if (state.armed_receipt_id == token.id) {
    state.armed_receipt_id = 0;
    state.armed_generation = 0;
    state.expected_requestor_client = 0;
  }
}

std::optional<OwnerTransitionToken> ClipboardManager::arm_owner_transition(
    Selection sel, const SelectionRead &source, std::uint32_t expected_client) {
  if (!is_open() || source.owner == XCB_WINDOW_NONE ||
      source.selection_timestamp == XCB_CURRENT_TIME || expected_client == 0 ||
      resource_client_id(source.owner) != expected_client) {
    return std::nullopt;
  }
  (void)pump_events();
  SelectionState &state = selection_state(sel);
  if (state.observed_owner != source.owner ||
      state.observed_selection_timestamp != source.selection_timestamp ||
      state.owner_generation != source.owner_generation) {
    return std::nullopt;
  }

  next_transition_id_ = next_generation(next_transition_id_);
  state.armed_transition_id = next_transition_id_;
  state.transition_source_owner = source.owner;
  state.transition_expected_client = expected_client;
  state.transition_status = OwnerTransitionStatus::Pending;
  state.transition_owner = source.owner;
  state.transition_timestamp = source.selection_timestamp;
  state.transition_identity_generation = state.owner_identity_generation;
  return OwnerTransitionToken{sel, next_transition_id_};
}

OwnerTransitionStatus ClipboardManager::owner_transition_status(
    const OwnerTransitionToken &token) const noexcept {
  const SelectionState &state = selection_state(token.selection);
  if (token.id == 0 || state.armed_transition_id != token.id) {
    return OwnerTransitionStatus::Invalid;
  }
  return state.transition_status;
}

void ClipboardManager::cancel_owner_transition(
    const OwnerTransitionToken &token) noexcept {
  SelectionState &state = selection_state(token.selection);
  if (token.id != 0 && state.armed_transition_id == token.id) {
    state.armed_transition_id = 0;
    state.transition_source_owner = XCB_WINDOW_NONE;
    state.transition_expected_client = 0;
    state.transition_status = OwnerTransitionStatus::Invalid;
    state.transition_owner = XCB_WINDOW_NONE;
    state.transition_timestamp = XCB_CURRENT_TIME;
    state.transition_identity_generation = 0;
  }
}

bool ClipboardManager::restore_selection_after_owner_transition(
    const OwnerTransitionToken &token, const SelectionRead &previous) {
  if (!is_open() || previous.text.size() > maximum_payload_bytes()) {
    return false;
  }
  (void)pump_events();
  SelectionState &state = selection_state(token.selection);
  if (token.id == 0 || state.armed_transition_id != token.id ||
      state.transition_status == OwnerTransitionStatus::Pending ||
      state.transition_status == OwnerTransitionStatus::Ambiguous ||
      state.transition_status == OwnerTransitionStatus::Invalid ||
      state.observed_owner != state.transition_owner ||
      state.owner_identity_generation != state.transition_identity_generation) {
    return false;
  }

  bool restored = false;
  if (state.transition_status == OwnerTransitionStatus::SameClientOwner) {
    const SelectionRead current{"", state.observed_owner,
                                state.observed_selection_timestamp,
                                state.owner_generation};
    restored = restore_selection_if_owner_from_client(
        token.selection, current, previous, state.transition_expected_client);
  } else if (state.transition_status == OwnerTransitionStatus::Released &&
             state.transition_owner == XCB_WINDOW_NONE) {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    const auto owner_cookie =
        xcb_get_selection_owner(connection_, selection_atom(token.selection));
    void *raw_owner = nullptr;
    xcb_generic_error_t *raw_error = nullptr;
    if (!wait_for_reply(owner_cookie.sequence, deadline, &raw_owner,
                        &raw_error)) {
      fail_closed();
      return false;
    }
    auto owner =
        xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
    auto error = xcb_ptr(raw_error);
    if (error || !owner || owner->owner != XCB_WINDOW_NONE ||
        !connection_healthy()) {
      return false;
    }
    restored = previous.owner == XCB_WINDOW_NONE ||
               set_text_impl(token.selection, previous.text,
                             state.observed_selection_timestamp);
  }

  if (restored) {
    cancel_owner_transition(token);
  }
  return restored;
}

std::uint64_t
ClipboardManager::selection_generation(Selection sel) const noexcept {
  return selection_state(sel).generation;
}

std::uint64_t
ClipboardManager::selection_owner_generation(Selection sel) const noexcept {
  return selection_state(sel).owner_generation;
}

std::size_t ClipboardManager::maximum_payload_bytes() const noexcept {
  if (!connection_healthy()) {
    return 0;
  }
  const std::uint32_t words = xcb_get_maximum_request_length(connection_);
  constexpr std::size_t header_words =
      sizeof(xcb_change_property_request_t) / sizeof(std::uint32_t);
  if (words <= header_words) {
    return 0;
  }
  const std::size_t protocol_limit =
      (static_cast<std::size_t>(words) - header_words) * sizeof(std::uint32_t);
  return std::min(protocol_limit, kMaxClipboardBytes);
}

bool ClipboardManager::handle_selection_request(
    const xcb_selection_request_event_t &request,
    std::chrono::steady_clock::time_point deadline) {
  if (!connection_healthy()) {
    return false;
  }

  xcb_selection_notify_event_t notify{};
  notify.response_type = XCB_SELECTION_NOTIFY;
  notify.time = request.time;
  notify.requestor = request.requestor;
  notify.selection = request.selection;
  notify.target = request.target;
  notify.property = XCB_ATOM_NONE;

  auto send_notify = [&](xcb_atom_t property) {
    notify.property = property;
    const auto cookie = xcb_send_event_checked(
        connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
        reinterpret_cast<const char *>(&notify));
    return complete_checked_request(cookie, deadline);
  };

  SelectionState *state = nullptr;
  if (request.selection == atom_clipboard_) {
    state = &clipboard_;
  } else if (request.selection == atom_primary_) {
    state = &primary_;
  }

  const xcb_atom_t property =
      request.property == XCB_ATOM_NONE ? request.target : request.property;
  if (state == nullptr || !state->owns ||
      request.requestor == XCB_WINDOW_NONE || property == XCB_ATOM_NONE) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }

  if (request.target == atom_targets_) {
    const std::array<xcb_atom_t, 6> targets{
        atom_targets_,    atom_utf8_string_, atom_text_plain_utf8_,
        atom_text_plain_, XCB_ATOM_STRING,   atom_timestamp_};
    const auto property_cookie = xcb_change_property_checked(
        connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
        XCB_ATOM_ATOM, 32, static_cast<std::uint32_t>(targets.size()),
        targets.data());
    if (!complete_checked_request(property_cookie, deadline)) {
      fail_closed();
      return false;
    }
    (void)send_notify(property);
    return false;
  }

  if (request.target == atom_timestamp_ && state->observed_owner == window_ &&
      state->observed_selection_timestamp != XCB_CURRENT_TIME) {
    const xcb_timestamp_t timestamp = state->observed_selection_timestamp;
    const auto property_cookie = xcb_change_property_checked(
        connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
        XCB_ATOM_INTEGER, 32, 1, &timestamp);
    if (!complete_checked_request(property_cookie, deadline)) {
      fail_closed();
      return false;
    }
    (void)send_notify(property);
    return false;
  }

  const bool text_target = request.target == atom_utf8_string_ ||
                           request.target == atom_text_plain_utf8_ ||
                           request.target == atom_text_plain_ ||
                           request.target == XCB_ATOM_STRING;
  if (!text_target || state->text.size() > maximum_payload_bytes() ||
      state->text.size() > std::numeric_limits<std::uint32_t>::max()) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }

  const std::uint64_t generation = state->generation;
  const auto property_cookie = xcb_change_property_checked(
      connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
      request.target, 8, static_cast<std::uint32_t>(state->text.size()),
      state->text.data());
  notify.property = property;
  const auto notify_cookie = xcb_send_event_checked(
      connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
      reinterpret_cast<const char *>(&notify));
  if (!complete_payload_transfer(property_cookie, notify_cookie, deadline)) {
    return false;
  }

  if (state->owns && state->generation == generation) {
    state->request_sequence = next_generation(state->request_sequence);
    if (state->armed_receipt_id != 0 && state->armed_generation == generation) {
      if (resource_client_id(request.requestor) ==
          state->expected_requestor_client) {
        state->confirmed_receipt_id = state->armed_receipt_id;
        state->armed_receipt_id = 0;
        state->armed_generation = 0;
        state->expected_requestor_client = 0;
      }
    }
    return true;
  }
  return false;
}

bool ClipboardManager::complete_payload_transfer(
    xcb_void_cookie_t property_cookie, xcb_void_cookie_t notify_cookie,
    std::chrono::steady_clock::time_point deadline) {
  if (!complete_checked_request(property_cookie, deadline) ||
      !complete_checked_request(notify_cookie, deadline)) {
    fail_closed();
    return false;
  }
  return true;
}

void ClipboardManager::handle_selection_clear(
    const xcb_selection_clear_event_t &event) noexcept {
  SelectionState *state = nullptr;
  if (event.selection == atom_clipboard_) {
    state = &clipboard_;
  } else if (event.selection == atom_primary_) {
    state = &primary_;
  }
  if (state != nullptr) {
    state->owns = false;
    state->text.clear();
  }
}

void ClipboardManager::handle_owner_change(
    const xcb_generic_event_t &event) noexcept {
  const auto &change =
      reinterpret_cast<const xcb_xfixes_selection_notify_event_t &>(event);
  SelectionState *state = nullptr;
  if (change.selection == atom_clipboard_) {
    state = &clipboard_;
  } else if (change.selection == atom_primary_) {
    state = &primary_;
  }
  if (state == nullptr) {
    return;
  }
  const bool owner_changed = change.owner != state->observed_owner;
  if (owner_changed) {
    state->owner_identity_generation =
        next_generation(state->owner_identity_generation);
    if (state->armed_transition_id != 0) {
      if (state->transition_status == OwnerTransitionStatus::Pending) {
        state->transition_status = OwnerTransitionStatus::Ambiguous;
        if (state->observed_owner == state->transition_source_owner) {
          if (change.owner == XCB_WINDOW_NONE) {
            state->transition_status = OwnerTransitionStatus::Released;
          } else if (resource_client_id(change.owner) ==
                     state->transition_expected_client) {
            state->transition_status = OwnerTransitionStatus::SameClientOwner;
          }
        }
      } else if (state->transition_status ==
                 OwnerTransitionStatus::SameClientOwner) {
        if (change.owner == XCB_WINDOW_NONE) {
          state->transition_status = OwnerTransitionStatus::Released;
        } else if (resource_client_id(change.owner) !=
                   state->transition_expected_client) {
          state->transition_status = OwnerTransitionStatus::Ambiguous;
        }
      } else {
        // Once released, a later owner is a new transaction. Once ambiguous,
        // no later transition can make the history safe again.
        state->transition_status = OwnerTransitionStatus::Ambiguous;
      }
      state->transition_owner = change.owner;
      state->transition_timestamp = change.selection_timestamp;
      state->transition_identity_generation = state->owner_identity_generation;
    }
  }
  state->observed_owner = change.owner;
  state->observed_selection_timestamp = change.selection_timestamp;
  state->baseline_initialized = true;
  state->owner_generation = next_generation(state->owner_generation);
  if (change.owner != window_) {
    state->owns = false;
    state->text.clear();
  }
}

std::size_t ClipboardManager::pump_events() {
  if (!connection_healthy()) {
    if (connection_ != nullptr) {
      fail_closed();
    }
    return 0;
  }

  const auto deadline = std::chrono::steady_clock::now() + kPumpTimeBudget;
  std::size_t processed = 0;
  while (processed < kPumpEventBudget &&
         std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(connection_));
    if (!event) {
      break;
    }
    ++processed;
    const std::uint8_t type =
        static_cast<std::uint8_t>(event->response_type & 0x7fU);
    if (type == 0) {
      fail_closed();
      break;
    }
    if (type == XCB_SELECTION_REQUEST) {
      (void)handle_selection_request(
          *reinterpret_cast<const xcb_selection_request_event_t *>(event.get()),
          deadline);
    } else if (type == XCB_SELECTION_CLEAR) {
      handle_selection_clear(
          *reinterpret_cast<const xcb_selection_clear_event_t *>(event.get()));
    } else if (xfixes_event_base_ != 0 &&
               type == xfixes_event_base_ + XCB_XFIXES_SELECTION_NOTIFY) {
      handle_owner_change(*event);
    }
    if (!connection_healthy()) {
      fail_closed();
      break;
    }
  }
  return processed;
}

std::optional<xcb_selection_notify_event_t>
ClipboardManager::wait_for_selection_notify(
    xcb_atom_t selection, xcb_atom_t target, xcb_atom_t property,
    std::chrono::steady_clock::time_point deadline) {
  while (connection_healthy() && std::chrono::steady_clock::now() < deadline) {
    std::size_t batch = 0;
    while (batch < kPumpEventBudget &&
           std::chrono::steady_clock::now() < deadline) {
      auto event = xcb_ptr(xcb_poll_for_event(connection_));
      if (!event) {
        break;
      }
      ++batch;
      const std::uint8_t type =
          static_cast<std::uint8_t>(event->response_type & 0x7fU);
      if (type == 0) {
        fail_closed();
        return std::nullopt;
      }
      if (type == XCB_SELECTION_REQUEST) {
        (void)handle_selection_request(
            *reinterpret_cast<const xcb_selection_request_event_t *>(
                event.get()),
            deadline);
      } else if (type == XCB_SELECTION_CLEAR) {
        handle_selection_clear(
            *reinterpret_cast<const xcb_selection_clear_event_t *>(
                event.get()));
      } else if (xfixes_event_base_ != 0 &&
                 type == xfixes_event_base_ + XCB_XFIXES_SELECTION_NOTIFY) {
        handle_owner_change(*event);
      } else if (type == XCB_SELECTION_NOTIFY) {
        const auto &notify =
            *reinterpret_cast<const xcb_selection_notify_event_t *>(
                event.get());
        if (notify.requestor == window_ && notify.selection == selection &&
            notify.target == target &&
            (notify.property == property || notify.property == XCB_ATOM_NONE)) {
          return notify;
        }
      }
    }

    if (!connection_healthy()) {
      fail_closed();
      return std::nullopt;
    }
    pollfd descriptor{xcb_get_file_descriptor(connection_), POLLIN, 0};
    const int result = ::poll(&descriptor, 1, bounded_poll_timeout(deadline));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 || (result > 0 && (descriptor.revents &
                                      (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
      fail_closed();
      return std::nullopt;
    }
  }
  if (!connection_healthy() && connection_ != nullptr) {
    fail_closed();
  }
  return std::nullopt;
}

std::optional<std::string>
ClipboardManager::decode_text_property(const PropertyBytes &property,
                                       xcb_atom_t expected_type,
                                       xcb_atom_t incr_atom) {
  if (property.type == XCB_ATOM_NONE || property.type == incr_atom ||
      property.type != expected_type || property.format != 8 ||
      property.bytes_after != 0 || property.size > kMaxClipboardBytes ||
      (property.size > 0 && property.data == nullptr)) {
    return std::nullopt;
  }
  if (property.size == 0) {
    return std::string{};
  }
  return std::string{reinterpret_cast<const char *>(property.data),
                     property.size};
}

std::optional<SelectionRead>
ClipboardManager::get_text_with_owner(Selection sel) {
  if (!is_open() && !open()) {
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;

  const xcb_atom_t selection = selection_atom(sel);
  const auto owner_cookie = xcb_get_selection_owner(connection_, selection);
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_owner_error = nullptr;
  if (!wait_for_reply(owner_cookie.sequence, deadline, &raw_owner,
                      &raw_owner_error)) {
    fail_closed();
    return std::nullopt;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto owner_error = xcb_ptr(raw_owner_error);
  if (owner_error || !owner || !connection_healthy()) {
    fail_closed();
    return std::nullopt;
  }
  if (owner->owner == XCB_WINDOW_NONE) {
    // No owner is a known empty selection, not a transport/read failure.
    return SelectionRead{"", XCB_WINDOW_NONE, XCB_CURRENT_TIME,
                         selection_state(sel).owner_generation};
  }

  const auto delete_cookie =
      xcb_delete_property_checked(connection_, window_, atom_read_property_);
  if (!complete_checked_request(delete_cookie, deadline)) {
    fail_closed();
    return std::nullopt;
  }

  const auto convert_cookie = xcb_convert_selection_checked(
      connection_, window_, selection, atom_utf8_string_, atom_read_property_,
      XCB_CURRENT_TIME);
  if (!complete_checked_request(convert_cookie, deadline)) {
    fail_closed();
    return std::nullopt;
  }

  const auto notify = wait_for_selection_notify(selection, atom_utf8_string_,
                                                atom_read_property_, deadline);
  if (!notify || notify->property == XCB_ATOM_NONE) {
    return std::nullopt;
  }

  constexpr std::uint32_t read_words =
      static_cast<std::uint32_t>((kMaxClipboardBytes + 3U) / 4U);
  const auto property_cookie =
      xcb_get_property(connection_, 0, window_, atom_read_property_,
                       XCB_GET_PROPERTY_TYPE_ANY, 0, read_words);
  void *raw_reply = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(property_cookie.sequence, deadline, &raw_reply,
                      &raw_error)) {
    fail_closed();
    return std::nullopt;
  }
  auto reply = xcb_ptr(static_cast<xcb_get_property_reply_t *>(raw_reply));
  auto error = xcb_ptr(raw_error);
  if (error || !reply || !connection_healthy()) {
    fail_closed();
    return std::nullopt;
  }

  const int raw_size = xcb_get_property_value_length(reply.get());
  if (raw_size < 0 ||
      (reply->format == 8 &&
       reply->value_len != static_cast<std::uint32_t>(raw_size))) {
    return std::nullopt;
  }
  if (reply->type == atom_incr_) {
    // Deleting an INCR marker acknowledges the streaming handshake. Since we
    // intentionally do not implement streaming, destroy this private window
    // instead so the sender cannot feed an unconsumed transfer into it.
    fail_closed();
    return std::nullopt;
  }
  const PropertyBytes property{
      reply->type, reply->format, reply->bytes_after,
      static_cast<const std::uint8_t *>(xcb_get_property_value(reply.get())),
      static_cast<std::size_t>(raw_size)};
  auto decoded = decode_text_property(property, atom_utf8_string_, atom_incr_);
  const auto delete_after_read =
      xcb_delete_property_checked(connection_, window_, atom_read_property_);
  if (!complete_checked_request(delete_after_read, deadline)) {
    fail_closed();
    return std::nullopt;
  }
  if (!decoded) {
    return std::nullopt;
  }

  const auto confirm_cookie = xcb_get_selection_owner(connection_, selection);
  void *raw_confirm = nullptr;
  xcb_generic_error_t *raw_confirm_error = nullptr;
  if (!wait_for_reply(confirm_cookie.sequence, deadline, &raw_confirm,
                      &raw_confirm_error)) {
    fail_closed();
    return std::nullopt;
  }
  auto confirmed =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_confirm));
  auto confirm_error = xcb_ptr(raw_confirm_error);
  if (confirm_error || !confirmed || confirmed->owner != owner->owner ||
      !connection_healthy()) {
    return std::nullopt;
  }
  SelectionState &state = selection_state(sel);
  while (state.observed_owner != confirmed->owner && connection_healthy() &&
         std::chrono::steady_clock::now() < deadline) {
    (void)pump_events();
    if (state.observed_owner == confirmed->owner) {
      break;
    }
    pollfd descriptor{xcb_get_file_descriptor(connection_), POLLIN, 0};
    (void)::poll(&descriptor, 1, bounded_poll_timeout(deadline));
  }
  if (state.observed_owner != confirmed->owner ||
      state.observed_selection_timestamp == XCB_CURRENT_TIME) {
    return std::nullopt;
  }
  return SelectionRead{std::move(*decoded), confirmed->owner,
                       state.observed_selection_timestamp,
                       state.owner_generation};
}

std::optional<std::string> ClipboardManager::get_text(Selection sel) {
  auto read = get_text_with_owner(sel);
  if (!read) {
    return std::nullopt;
  }
  return std::move(read->text);
}

std::optional<bool>
ClipboardManager::has_only_text_targets(Selection sel,
                                        const SelectionRead &expected) {
  if (!is_open()) {
    return std::nullopt;
  }
  if (expected.owner == XCB_WINDOW_NONE) {
    return expected.text.empty();
  }
  if (expected.selection_timestamp == XCB_CURRENT_TIME) {
    return std::nullopt;
  }

  (void)pump_events();
  const SelectionState &state = selection_state(sel);
  if (state.observed_owner != expected.owner ||
      state.observed_selection_timestamp != expected.selection_timestamp) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  if (!complete_checked_request(xcb_delete_property_checked(
                                    connection_, window_, atom_read_property_),
                                deadline) ||
      !complete_checked_request(xcb_convert_selection_checked(
                                    connection_, window_, selection_atom(sel),
                                    atom_targets_, atom_read_property_,
                                    expected.selection_timestamp),
                                deadline)) {
    fail_closed();
    return std::nullopt;
  }
  const auto notify = wait_for_selection_notify(
      selection_atom(sel), atom_targets_, atom_read_property_, deadline);
  if (!notify || notify->property == XCB_ATOM_NONE) {
    return std::nullopt;
  }

  constexpr std::uint32_t kMaximumTargets = 256;
  const auto property_cookie =
      xcb_get_property(connection_, 0, window_, atom_read_property_,
                       XCB_ATOM_ATOM, 0, kMaximumTargets + 1U);
  void *raw_property = nullptr;
  xcb_generic_error_t *raw_property_error = nullptr;
  if (!wait_for_reply(property_cookie.sequence, deadline, &raw_property,
                      &raw_property_error)) {
    fail_closed();
    return std::nullopt;
  }
  auto property =
      xcb_ptr(static_cast<xcb_get_property_reply_t *>(raw_property));
  auto property_error = xcb_ptr(raw_property_error);
  const int raw_size =
      property ? xcb_get_property_value_length(property.get()) : -1;
  if (property_error || !property || property->type != XCB_ATOM_ATOM ||
      property->format != 32 || property->bytes_after != 0 || raw_size < 0 ||
      raw_size % static_cast<int>(sizeof(xcb_atom_t)) != 0 ||
      static_cast<std::size_t>(raw_size) >
          kMaximumTargets * sizeof(xcb_atom_t)) {
    return std::nullopt;
  }

  const auto *targets =
      static_cast<const xcb_atom_t *>(xcb_get_property_value(property.get()));
  const std::size_t target_count =
      static_cast<std::size_t>(raw_size) / sizeof(xcb_atom_t);
  const auto is_safe_target = [this](xcb_atom_t target) {
    return target == atom_targets_ || target == atom_utf8_string_ ||
           target == atom_text_plain_utf8_ || target == atom_text_plain_ ||
           target == atom_compound_text_ || target == atom_text_ ||
           target == XCB_ATOM_STRING || target == atom_timestamp_ ||
           target == atom_multiple_ || target == atom_save_targets_;
  };
  const bool text_only =
      target_count != 0 &&
      std::all_of(targets, targets + target_count, is_safe_target);

  if (!complete_checked_request(xcb_delete_property_checked(
                                    connection_, window_, atom_read_property_),
                                deadline)) {
    fail_closed();
    return std::nullopt;
  }
  const auto owner_cookie =
      xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_owner_error = nullptr;
  if (!wait_for_reply(owner_cookie.sequence, deadline, &raw_owner,
                      &raw_owner_error)) {
    fail_closed();
    return std::nullopt;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto owner_error = xcb_ptr(raw_owner_error);
  (void)pump_events();
  if (owner_error || !owner || owner->owner != expected.owner ||
      state.observed_owner != expected.owner ||
      state.observed_selection_timestamp != expected.selection_timestamp ||
      !connection_healthy()) {
    return false;
  }
  return text_only;
}

bool ClipboardManager::set_text_impl(Selection sel, std::string_view text,
                                     xcb_timestamp_t timestamp) {
  if (!is_open() || text.size() > maximum_payload_bytes() ||
      text.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  const xcb_atom_t selection = selection_atom(sel);
  SelectionState &state = selection_state(sel);
  const std::uint64_t previous_owner_generation = state.owner_generation;
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto owner_cookie = xcb_set_selection_owner_checked(
      connection_, window_, selection, timestamp);
  const auto query_cookie = xcb_get_selection_owner(connection_, selection);
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(query_cookie.sequence, deadline, &raw_owner,
                      &raw_error)) {
    state.owns = false;
    state.text.clear();
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto query_error = xcb_ptr(raw_error);
  if (!complete_checked_request(owner_cookie, deadline) || query_error ||
      !owner || owner->owner != window_ || !connection_healthy()) {
    state.owns = false;
    state.text.clear();
    if (!connection_healthy()) {
      fail_closed();
    }
    return false;
  }

  while (connection_healthy() &&
         (state.owner_generation == previous_owner_generation ||
          state.observed_owner != window_ ||
          state.observed_selection_timestamp == XCB_CURRENT_TIME) &&
         std::chrono::steady_clock::now() < deadline) {
    (void)pump_events();
    if (state.owner_generation != previous_owner_generation &&
        state.observed_owner == window_ &&
        state.observed_selection_timestamp != XCB_CURRENT_TIME) {
      break;
    }
    pollfd descriptor{xcb_get_file_descriptor(connection_), POLLIN, 0};
    const int result = ::poll(&descriptor, 1, bounded_poll_timeout(deadline));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 || (result > 0 && (descriptor.revents &
                                      (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
      break;
    }
  }
  if (!connection_healthy() ||
      state.owner_generation == previous_owner_generation ||
      state.observed_owner != window_ ||
      state.observed_selection_timestamp == XCB_CURRENT_TIME) {
    state.owns = false;
    state.text.clear();
    fail_closed();
    return false;
  }

  if (text.empty()) {
    state.text.clear();
  } else {
    state.text.assign(text.data(), text.size());
  }
  state.owns = true;
  state.generation = next_generation(state.generation);
  state.armed_receipt_id = 0;
  state.armed_generation = 0;
  state.expected_requestor_client = 0;
  return true;
}

ClipboardResult ClipboardManager::set_text(Selection sel,
                                           std::string_view text) {
  if (!is_open() && !open()) {
    return ClipboardResult::NoConnection;
  }
  if (text.size() > maximum_payload_bytes() ||
      text.size() > std::numeric_limits<std::uint32_t>::max()) {
    return ClipboardResult::ConversionFailed;
  }
  return set_text_impl(sel, text) ? ClipboardResult::Ok
                                  : ClipboardResult::ConversionFailed;
}

bool ClipboardManager::restore_text_if_generation(
    Selection sel, std::uint64_t expected_generation, std::string_view text) {
  if (!is_open()) {
    return false;
  }
  SelectionState &state = selection_state(sel);
  if (!state.owns || state.generation != expected_generation) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto cookie = xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error)) {
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto error = xcb_ptr(raw_error);
  if (error || !owner || owner->owner != window_ || !connection_healthy()) {
    state.owns = false;
    state.text.clear();
    if (!connection_healthy()) {
      fail_closed();
    }
    return false;
  }
  if (text.size() > maximum_payload_bytes() ||
      text.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  if (text.empty()) {
    state.text.clear();
  } else {
    state.text.assign(text.data(), text.size());
  }
  state.generation = next_generation(state.generation);
  return true;
}

bool ClipboardManager::set_text_if_owner(Selection sel,
                                         const SelectionRead &expected,
                                         std::string_view text) {
  if (!is_open() || expected.owner == XCB_WINDOW_NONE ||
      expected.selection_timestamp == XCB_CURRENT_TIME ||
      text.size() > maximum_payload_bytes()) {
    return false;
  }
  (void)pump_events();
  const SelectionState &state = selection_state(sel);
  if (state.observed_owner != expected.owner ||
      state.observed_selection_timestamp != expected.selection_timestamp) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto cookie = xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error)) {
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto error = xcb_ptr(raw_error);
  if (error || !owner || owner->owner != expected.owner ||
      !connection_healthy()) {
    return false;
  }
  return set_text_impl(sel, text, expected.selection_timestamp);
}

bool ClipboardManager::restore_selection_if_owner_from_client(
    Selection sel, const SelectionRead &expected_current,
    const SelectionRead &previous, std::uint32_t expected_client) {
  if (!is_open() || expected_client == 0 ||
      expected_current.owner == XCB_WINDOW_NONE ||
      expected_current.selection_timestamp == XCB_CURRENT_TIME ||
      resource_client_id(expected_current.owner) != expected_client ||
      previous.text.size() > maximum_payload_bytes()) {
    return false;
  }
  (void)pump_events();
  const SelectionState &state = selection_state(sel);
  if (state.observed_owner != expected_current.owner ||
      state.observed_selection_timestamp !=
          expected_current.selection_timestamp) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto owner_cookie =
      xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_owner_error = nullptr;
  if (!wait_for_reply(owner_cookie.sequence, deadline, &raw_owner,
                      &raw_owner_error)) {
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto owner_error = xcb_ptr(raw_owner_error);
  if (owner_error || !owner || owner->owner != expected_current.owner ||
      !connection_healthy()) {
    return false;
  }

  if (previous.owner != XCB_WINDOW_NONE) {
    return set_text_impl(sel, previous.text,
                         expected_current.selection_timestamp);
  }

  const auto clear_cookie = xcb_set_selection_owner_checked(
      connection_, XCB_WINDOW_NONE, selection_atom(sel),
      expected_current.selection_timestamp);
  if (!complete_checked_request(clear_cookie, deadline)) {
    fail_closed();
    return false;
  }
  const auto confirm_cookie =
      xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_confirm = nullptr;
  xcb_generic_error_t *raw_confirm_error = nullptr;
  if (!wait_for_reply(confirm_cookie.sequence, deadline, &raw_confirm,
                      &raw_confirm_error)) {
    fail_closed();
    return false;
  }
  auto confirmed =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_confirm));
  auto confirm_error = xcb_ptr(raw_confirm_error);
  if (confirm_error || !confirmed || confirmed->owner != XCB_WINDOW_NONE) {
    return false;
  }
  SelectionState &mutable_state = selection_state(sel);
  mutable_state.owns = false;
  mutable_state.text.clear();
  return true;
}

bool ClipboardManager::restore_selection_after_client_release(
    Selection sel, const SelectionRead &released, const SelectionRead &previous,
    std::uint32_t expected_client) {
  if (!is_open() || expected_client == 0 || released.owner == XCB_WINDOW_NONE ||
      released.selection_timestamp == XCB_CURRENT_TIME ||
      resource_client_id(released.owner) != expected_client ||
      previous.text.size() > maximum_payload_bytes()) {
    return false;
  }

  (void)pump_events();
  const SelectionState &state = selection_state(sel);
  if (state.observed_owner != XCB_WINDOW_NONE ||
      state.observed_selection_timestamp == XCB_CURRENT_TIME ||
      state.owner_generation != next_generation(released.owner_generation)) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto owner_cookie =
      xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_owner_error = nullptr;
  if (!wait_for_reply(owner_cookie.sequence, deadline, &raw_owner,
                      &raw_owner_error)) {
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto owner_error = xcb_ptr(raw_owner_error);
  if (owner_error || !owner || owner->owner != XCB_WINDOW_NONE ||
      !connection_healthy()) {
    return false;
  }

  if (previous.owner == XCB_WINDOW_NONE) {
    return true;
  }
  return set_text_impl(sel, previous.text, state.observed_selection_timestamp);
}

bool ClipboardManager::owns_generation(Selection sel,
                                       std::uint64_t generation) {
  if (!is_open()) {
    return false;
  }
  const SelectionState &state = selection_state(sel);
  if (!state.owns || state.generation != generation) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto cookie = xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error)) {
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto error = xcb_ptr(raw_error);
  const bool owns =
      !error && owner && owner->owner == window_ && connection_healthy();
  if (!owns) {
    selection_state(sel).owns = false;
  }
  return owns;
}

bool ClipboardManager::locally_owns_generation(
    Selection sel, std::uint64_t generation) const noexcept {
  const SelectionState &state = selection_state(sel);
  return state.owns && state.generation == generation;
}

bool ClipboardManager::verify_ownership() {
  if (!is_open()) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto owns = [&](xcb_atom_t selection, const SelectionState &state) {
    if (!state.owns) {
      return true;
    }
    const auto cookie = xcb_get_selection_owner(connection_, selection);
    void *raw_owner = nullptr;
    xcb_generic_error_t *raw_error = nullptr;
    if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error)) {
      return false;
    }
    auto owner =
        xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
    auto error = xcb_ptr(raw_error);
    return !error && owner && owner->owner == window_ && connection_healthy();
  };

  const bool owned =
      owns(atom_clipboard_, clipboard_) && owns(atom_primary_, primary_);
  if (!owned) {
    fail_closed();
  }
  return owned;
}

ActiveWindowKind ClipboardManager::active_window_kind() {
  if (!is_open() && !open()) {
    return ActiveWindowKind::Unknown;
  }
  if (atom_net_active_window_ == XCB_ATOM_NONE) {
    return ActiveWindowKind::Unknown;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;

  const auto active = active_window(deadline);
  if (!active) {
    return ActiveWindowKind::Unknown;
  }

  xcb_window_t window = *active;
  for (int depth = 0; depth < 8 && window != XCB_WINDOW_NONE; ++depth) {
    constexpr std::uint32_t wm_class_words =
        static_cast<std::uint32_t>((kMaxWmClassBytes + 3U) / 4U);
    const auto class_cookie =
        xcb_get_property(connection_, 0, window, atom_wm_class_,
                         XCB_ATOM_STRING, 0, wm_class_words);
    void *raw_class_reply = nullptr;
    xcb_generic_error_t *raw_class_error = nullptr;
    if (!wait_for_reply(class_cookie.sequence, deadline, &raw_class_reply,
                        &raw_class_error)) {
      fail_closed();
      return ActiveWindowKind::Unknown;
    }
    auto class_reply =
        xcb_ptr(static_cast<xcb_get_property_reply_t *>(raw_class_reply));
    auto class_error = xcb_ptr(raw_class_error);
    if (!class_error && class_reply && class_reply->type == XCB_ATOM_STRING &&
        class_reply->format == 8 && class_reply->bytes_after == 0) {
      const int raw_size = xcb_get_property_value_length(class_reply.get());
      if (raw_size >= 0 &&
          static_cast<std::size_t>(raw_size) <= kMaxWmClassBytes) {
        const auto *bytes = static_cast<const char *>(
            xcb_get_property_value(class_reply.get()));
        const std::string_view value{bytes, static_cast<std::size_t>(raw_size)};
        const std::size_t first_end = value.find('\0');
        const std::string_view instance = value.substr(0, first_end);
        std::string_view klass;
        if (first_end != std::string_view::npos &&
            first_end + 1 < value.size()) {
          const std::size_t second_end = value.find('\0', first_end + 1);
          klass = value.substr(first_end + 1, second_end - first_end - 1);
        }
        if (is_terminal_wm_class(instance, klass)) {
          return ActiveWindowKind::Terminal;
        }
        if (is_safe_gui_wm_class(instance, klass)) {
          return ActiveWindowKind::Gui;
        }
      }
    }

    const auto tree_cookie = xcb_query_tree(connection_, window);
    void *raw_tree = nullptr;
    xcb_generic_error_t *raw_tree_error = nullptr;
    if (!wait_for_reply(tree_cookie.sequence, deadline, &raw_tree,
                        &raw_tree_error)) {
      fail_closed();
      return ActiveWindowKind::Unknown;
    }
    auto tree = xcb_ptr(static_cast<xcb_query_tree_reply_t *>(raw_tree));
    auto tree_error = xcb_ptr(raw_tree_error);
    if (tree_error || !tree || tree->parent == XCB_WINDOW_NONE ||
        tree->parent == window) {
      break;
    }
    window = tree->parent;
  }

  if (!connection_healthy()) {
    fail_closed();
  }
  return ActiveWindowKind::Unknown;
}

std::optional<std::uint32_t> ClipboardManager::active_client_id() {
  if (!is_open()) {
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto active = active_window(deadline);
  if (!active) {
    return std::nullopt;
  }
  const std::uint32_t client = resource_client_id(*active);
  return client == 0 ? std::nullopt : std::optional<std::uint32_t>{client};
}

std::optional<xcb_window_t> ClipboardManager::active_window(
    std::chrono::steady_clock::time_point deadline) {
  if (!connection_healthy() || screen_ == nullptr ||
      atom_net_active_window_ == XCB_ATOM_NONE) {
    return std::nullopt;
  }
  const auto active_cookie =
      xcb_get_property(connection_, 0, screen_->root, atom_net_active_window_,
                       XCB_ATOM_WINDOW, 0, 1);
  void *raw_active_reply = nullptr;
  xcb_generic_error_t *raw_active_error = nullptr;
  if (!wait_for_reply(active_cookie.sequence, deadline, &raw_active_reply,
                      &raw_active_error)) {
    fail_closed();
    return std::nullopt;
  }
  auto active_reply =
      xcb_ptr(static_cast<xcb_get_property_reply_t *>(raw_active_reply));
  auto active_error = xcb_ptr(raw_active_error);
  if (active_error || !active_reply || active_reply->type != XCB_ATOM_WINDOW ||
      active_reply->format != 32 || active_reply->bytes_after != 0 ||
      xcb_get_property_value_length(active_reply.get()) !=
          static_cast<int>(sizeof(xcb_window_t))) {
    if (!connection_healthy()) {
      fail_closed();
    }
    return std::nullopt;
  }

  xcb_window_t window = XCB_WINDOW_NONE;
  std::memcpy(&window, xcb_get_property_value(active_reply.get()),
              sizeof(window));
  return window == XCB_WINDOW_NONE ? std::nullopt
                                   : std::optional<xcb_window_t>{window};
}

std::uint32_t
ClipboardManager::resource_client_id(xcb_window_t window) const noexcept {
  if (!connection_healthy() || window == XCB_WINDOW_NONE) {
    return 0;
  }
  const xcb_setup_t *setup = xcb_get_setup(connection_);
  if (setup == nullptr) {
    return 0;
  }
  // The X server assigns one resource-id base per client connection. GTK3
  // performs clipboard conversion through a GtkInvisible sibling window, so
  // top-level ancestry is not a valid application identity. Masking the
  // per-client resource bits correlates that helper window with the active
  // GDK connection while rejecting windows allocated by other clients.
  return window & ~setup->resource_id_mask;
}

} // namespace punto
