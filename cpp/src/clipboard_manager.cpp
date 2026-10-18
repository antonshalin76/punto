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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <tuple>
#include <utility>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>

namespace punto {

#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
extern "C" void
punto_e2e_before_clipboard_payload_transfer(std::uint64_t receipt_id);
#endif

namespace {

constexpr std::size_t kPumpEventBudget = 64;
constexpr auto kPumpTimeBudget = std::chrono::milliseconds{2};
// Keep each direct or incremental chunk plus the
// checked-request barrier below Linux's minimum Unix socket send buffer so a
// stopped X server cannot make libxcb enter its unbounded POLLOUT wait.
constexpr std::size_t kMaxClipboardBytes = 4096U; // One bounded socket write.
constexpr std::size_t kRawPayloadBytes = 4U * 1024U * 1024U;
constexpr std::size_t kRawTargets = 64;
constexpr std::size_t kIncomingChunks = 4096;
constexpr std::size_t kOutgoingTransfers = 4;
constexpr auto kTransferBudget = std::chrono::seconds{2};

std::size_t payload_bytes(const SelectionPayload &payload) {
  std::size_t total = 0;
  if (payload)
    for (const auto &entry : *payload)
      total += entry.bytes.size();
  return total;
}
bool valid_data(const SelectionData &data) {
  return data.target != XCB_ATOM_NONE && data.type != XCB_ATOM_NONE &&
         (data.format == 8 || data.format == 16 || data.format == 32) &&
         data.bytes.size() % (data.format / 8U) == 0;
}
constexpr std::size_t kMaxWmClassBytes = 4096;

class ScopedSelectionQuery {
public:
  ScopedSelectionQuery(BoundedXcbConnection &connection,
                       const std::uint64_t &generation, xcb_window_t window)
      : connection_{connection}, current_generation_{generation},
        original_generation_{generation}, original_{connection.get()},
        window_{window} {}
  ScopedSelectionQuery(const ScopedSelectionQuery &) = delete;
  ScopedSelectionQuery &operator=(const ScopedSelectionQuery &) = delete;
  ~ScopedSelectionQuery() {
    if (current_generation_ == original_generation_ && connection_.is_open() &&
        connection_.get() == original_) {
      (void)xcb_destroy_window(original_, window_);
      (void)xcb_flush(original_);
    }
  }

private:
  BoundedXcbConnection &connection_;
  const std::uint64_t &current_generation_;
  std::uint64_t original_generation_;
  xcb_connection_t *original_;
  xcb_window_t window_;
};

void log_init_failure(std::string_view stage,
                      std::chrono::steady_clock::time_point deadline,
                      std::optional<Selection> selection = std::nullopt) {
  const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
  std::cerr << "[punto] Clipboard open failed stage=" << stage;
  if (selection) {
    std::cerr << " selection="
              << (*selection == Selection::Clipboard ? "clipboard" : "primary");
  }
  std::cerr << " remaining_us=" << remaining << '\n';
}

void log_read_failure(Selection selection, std::string_view stage) {
  std::cerr << "[punto] Clipboard read failed selection="
            << (selection == Selection::Clipboard ? "clipboard" : "primary")
            << " stage=" << stage << '\n';
}

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

[[nodiscard]] bool is_safe_gui_wm_class(std::string_view instance,
                                        std::string_view klass) noexcept {
  const auto ordinary = [](std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char byte) {
             const auto code = static_cast<unsigned char>(byte);
             return code >= 0x20U && code != 0x7fU;
           });
  };
  return ordinary(instance) && ordinary(klass);
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
  return open(std::chrono::steady_clock::now() + timeout_);
}

bool ClipboardManager::open(std::chrono::steady_clock::time_point deadline) {
  if (is_open()) {
    return true;
  }
  close();
  if (session_ == nullptr || std::chrono::steady_clock::now() >= deadline) {
    log_init_failure("deadline", deadline);
    return false;
  }

  auto lease = session_->acquire_write_lease();
  if (!lease) {
    log_init_failure("session_lease", deadline);
    return false;
  }
  const bool initialized =
      initialize_connection(lease->open_bounded_connection(deadline), deadline);
  if (initialized)
    connection_session_generation_ = lease->generation();
  return initialized;
}

bool ClipboardManager::initialize_connection(
    BoundedXcbConnection connection,
    std::chrono::steady_clock::time_point deadline, bool initialize_baselines) {
  if (!connection.is_open() || connection.screen_number() < 0) {
    log_init_failure("connection", deadline);
    return false;
  }

  const auto fail = [&](std::string_view stage) {
    log_init_failure(stage, deadline);
    fail_closed();
    return false;
  };

  connection_owner_ = std::move(connection);
  connection_ = connection_owner_.get();
  connection_generation_ = next_generation(connection_generation_);
  const int screen_number = connection_owner_.screen_number();
  const xcb_setup_t *setup = xcb_get_setup(connection_);
  if (setup == nullptr) {
    return fail("setup");
  }

  xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
  for (int index = 0; index < screen_number && screens.rem > 0; ++index) {
    xcb_screen_next(&screens);
  }
  if (screens.rem <= 0 || screens.data == nullptr) {
    return fail("screen");
  }
  screen_ = screens.data;
  window_ = xcb_generate_id(connection_);
  const std::uint32_t event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  const auto create_cookie = xcb_create_window_checked(
      connection_, XCB_COPY_FROM_PARENT, window_, screen_->root, 0, 0, 1, 1, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, XCB_CW_EVENT_MASK,
      &event_mask);
  if (!complete_checked_request(create_cookie, deadline)) {
    return fail("create_window");
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
  atom_server_time_ = intern_atom("PUNTO_SERVER_TIME", deadline);

  if (atom_clipboard_ == XCB_ATOM_NONE || atom_utf8_string_ == XCB_ATOM_NONE ||
      atom_targets_ == XCB_ATOM_NONE || atom_text_plain_ == XCB_ATOM_NONE ||
      atom_text_plain_utf8_ == XCB_ATOM_NONE || atom_incr_ == XCB_ATOM_NONE ||
      atom_compound_text_ == XCB_ATOM_NONE || atom_text_ == XCB_ATOM_NONE ||
      atom_timestamp_ == XCB_ATOM_NONE || atom_multiple_ == XCB_ATOM_NONE ||
      atom_save_targets_ == XCB_ATOM_NONE ||
      atom_read_property_ == XCB_ATOM_NONE ||
      atom_server_time_ == XCB_ATOM_NONE || !connection_healthy()) {
    return fail("atoms");
  }

  // Populate XCB's extension cache before generated XFixes requests can
  // perform an unbounded first lookup. The core reply fences the query.
  xcb_prefetch_extension_data(connection_, &xcb_xfixes_id);
  const auto extension_barrier = xcb_get_input_focus(connection_);
  void *raw_barrier = nullptr;
  xcb_generic_error_t *raw_barrier_error = nullptr;
  if (!wait_for_reply(extension_barrier.sequence, deadline, &raw_barrier,
                      &raw_barrier_error)) {
    return fail("xfixes_barrier");
  }
  auto barrier =
      xcb_ptr(static_cast<xcb_get_input_focus_reply_t *>(raw_barrier));
  auto barrier_error = xcb_ptr(raw_barrier_error);

  const xcb_query_extension_reply_t *xfixes =
      xcb_get_extension_data(connection_, &xcb_xfixes_id);
  if (xfixes == nullptr || xfixes->present == 0) {
    return fail("xfixes_presence");
  }
  xfixes_event_base_ = xfixes->first_event;
  const auto version_cookie = xcb_xfixes_query_version(connection_, 5, 0);
  void *raw_version = nullptr;
  xcb_generic_error_t *raw_version_error = nullptr;
  if (!wait_for_reply(version_cookie.sequence, deadline, &raw_version,
                      &raw_version_error)) {
    return fail("xfixes_version_reply");
  }
  auto version =
      xcb_ptr(static_cast<xcb_xfixes_query_version_reply_t *>(raw_version));
  auto version_error = xcb_ptr(raw_version_error);
  if (!version || version_error) {
    return fail("xfixes_version");
  }
  constexpr std::uint32_t owner_mask =
      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
      XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
  const auto clipboard_watch = xcb_xfixes_select_selection_input_checked(
      connection_, window_, atom_clipboard_, owner_mask);
  const auto primary_watch = xcb_xfixes_select_selection_input_checked(
      connection_, window_, atom_primary_, owner_mask);

  if (!complete_checked_request(clipboard_watch, deadline) ||
      !complete_checked_request(primary_watch, deadline) ||
      !connection_healthy()) {
    return fail("xfixes_watch");
  }
  if (!initialize_baselines)
    return true;
  if (!initialize_selection_baseline(Selection::Clipboard, deadline)) {
    fail_closed();
    return false;
  }
  if (!initialize_selection_baseline(Selection::Primary, deadline)) {
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

void ClipboardManager::close(std::source_location where) noexcept {
  close_impl(where, false);
}

void ClipboardManager::close_impl(std::source_location where,
                                  bool failure) noexcept {
  if (connection_ != nullptr) {
    const char *file = std::strrchr(where.file_name(), '/');
    file = file ? file + 1 : where.file_name();
    std::fprintf(
        stderr,
        "[punto] Clipboard close failure=%d site=%.96s:%u function=\"%.160s\" "
        "window=%u connection_generation=%llu "
        "clipboard_owner=%u clipboard_owns=%d clipboard_generation=%llu "
        "clipboard_receipt=%llu primary_owner=%u primary_owns=%d "
        "primary_generation=%llu primary_receipt=%llu\n",
        failure, file, static_cast<unsigned>(where.line()),
        where.function_name(), window_,
        static_cast<unsigned long long>(connection_generation_),
        clipboard_.observed_owner, clipboard_.owns,
        static_cast<unsigned long long>(clipboard_.generation),
        static_cast<unsigned long long>(clipboard_.armed_receipt_id),
        primary_.observed_owner, primary_.owns,
        static_cast<unsigned long long>(primary_.generation),
        static_cast<unsigned long long>(primary_.armed_receipt_id));
  }
  deferred_peer_events_.clear();
  outgoing_
      .clear(); // A witness, unlike publisher transfers, survives fail-close.
  retired_transfer_watches_.clear();
  if (clipboard_.armed_transition_id != 0) {
    cancel_owner_transition(OwnerTransitionToken{
        Selection::Clipboard, clipboard_.armed_transition_id});
  }
  if (primary_.armed_transition_id != 0) {
    cancel_owner_transition(
        OwnerTransitionToken{Selection::Primary, primary_.armed_transition_id});
  }
  clipboard_.owns = false;
  clipboard_.payload.reset();
  clipboard_.generation = next_generation(clipboard_.generation);
  clipboard_.armed_receipt_id = 0;
  clipboard_.armed_generation = 0;
  clipboard_.expected_requestor_client = 0;
  clipboard_.receipt_context_owner = XCB_WINDOW_NONE;
  clipboard_.receipt_context_timestamp = XCB_CURRENT_TIME;
  clipboard_.receipt_context_owner_generation = 0;
  clipboard_.receipt_context_connection_generation = 0;
  clipboard_.confirmed_receipt_id = 0;
  clipboard_.receipt_request_sequence = 0;
  clipboard_.receipt_deadline = std::chrono::steady_clock::time_point::max();
  clipboard_.observed_owner = XCB_WINDOW_NONE;
  clipboard_.observed_selection_timestamp = XCB_CURRENT_TIME;
  clipboard_.baseline_initialized = false;
  clipboard_.owner_generation = next_generation(clipboard_.owner_generation);
  clipboard_.owner_identity_generation =
      next_generation(clipboard_.owner_identity_generation);
  primary_.owns = false;
  primary_.payload.reset();
  primary_.generation = next_generation(primary_.generation);
  primary_.armed_receipt_id = 0;
  primary_.armed_generation = 0;
  primary_.expected_requestor_client = 0;
  primary_.receipt_context_owner = XCB_WINDOW_NONE;
  primary_.receipt_context_timestamp = XCB_CURRENT_TIME;
  primary_.receipt_context_owner_generation = 0;
  primary_.receipt_context_connection_generation = 0;
  primary_.confirmed_receipt_id = 0;
  primary_.receipt_request_sequence = 0;
  primary_.receipt_deadline = std::chrono::steady_clock::time_point::max();
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

bool ClipboardManager::current_connection(
    const SelectionRead &read) const noexcept {
  return read.connection_generation != 0 &&
         read.connection_generation == connection_generation_;
}

bool ClipboardManager::wait_for_reply(
    unsigned int sequence, std::chrono::steady_clock::time_point deadline,
    void **reply, xcb_generic_error_t **error, ReplyIntent intent) {
  if (reply == nullptr || error == nullptr || !connection_healthy()) {
    return false;
  }
  *reply = nullptr;
  *error = nullptr;
  x11_detail::XcbOperationResult result{};
  *reply =
      intent == ReplyIntent::Observation
          ? connection_owner_.wait_for_read_reply(sequence, deadline, result)
          : connection_owner_.wait_for_reply(sequence, deadline, result);
  *error = nullptr;
  return result == x11_detail::XcbOperationResult::Success;
}

bool ClipboardManager::complete_checked_request(
    xcb_void_cookie_t cookie, std::chrono::steady_clock::time_point deadline) {
  x11_detail::XcbOperationResult result{};
  return connection_owner_.check_request(cookie, deadline, result);
}

std::optional<xcb_timestamp_t> ClipboardManager::server_timestamp(
    std::chrono::steady_clock::time_point deadline, xcb_timestamp_t previous,
    RequestScan requests) {
  if (!drain_queued_events_to_fence(deadline, requests)) {
    return std::nullopt;
  }
  std::size_t processed = 0;
  while (connection_healthy() && event_budget_available() &&
         processed < kPumpEventBudget &&
         std::chrono::steady_clock::now() < deadline) {
    const std::uint8_t marker = 1;
    if (!complete_checked_request(
            xcb_change_property_checked(connection_, XCB_PROP_MODE_REPLACE,
                                        window_, atom_server_time_,
                                        XCB_ATOM_INTEGER, 8, 1, &marker),
            deadline)) {
      return std::nullopt;
    }
    while (connection_healthy() && event_budget_available() &&
           processed < kPumpEventBudget &&
           std::chrono::steady_clock::now() < deadline) {
      auto event = xcb_ptr(xcb_poll_for_event(connection_));
      if (!event) {
        pollfd descriptor{xcb_get_file_descriptor(connection_), POLLIN, 0};
        const int result =
            ::poll(&descriptor, 1, bounded_poll_timeout(deadline));
        if (result < 0 && errno == EINTR) {
          continue;
        }
        if (result < 0 ||
            (result > 0 &&
             (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
          return std::nullopt;
        }
        continue;
      }
      ++processed;
      record_polled_event();
      const auto type = event->response_type & 0x7fU;
      if (type == XCB_PROPERTY_NOTIFY) {
        const auto &property =
            *reinterpret_cast<xcb_property_notify_event_t *>(event.get());
        if (property.window == window_ && property.atom == atom_server_time_ &&
            property.state == XCB_PROPERTY_NEW_VALUE &&
            property.time != XCB_CURRENT_TIME) {
          if (property.time != previous) {
            return property.time;
          }
          // The server clock has millisecond resolution; yield before retry.
          (void)::poll(nullptr, 0, std::min(1, bounded_poll_timeout(deadline)));
          break;
        }
      }
      if (process_event(*event, deadline, requests) || !connection_healthy()) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

bool ClipboardManager::initialize_selection_baseline(
    Selection sel, std::chrono::steady_clock::time_point deadline) {
  const auto fail = [&](std::string_view stage) {
    log_init_failure(stage, deadline, sel);
    return false;
  };
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
      return fail("selection_owner_reply");
    }
    auto owner =
        xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
    auto error = xcb_ptr(raw_error);
    if (error || !owner || !connection_healthy()) {
      return fail("selection_owner");
    }

    (void)pump_events_until(deadline);
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
      const auto requestor = request_selection_conversion(
          sel, atom_timestamp_, XCB_CURRENT_TIME, deadline);
      if (!requestor) {
        return fail("selection_timestamp_request");
      }
      ScopedSelectionQuery query{connection_owner_, connection_generation_,
                                 *requestor};
      const auto notify = wait_for_selection_notify(
          *requestor, selection_atom(sel), atom_timestamp_, atom_read_property_,
          deadline);
      if (!notify) {
        return fail("selection_timestamp_notify");
      }

      if (notify->property == XCB_ATOM_NONE) {
        const auto local_fence = server_timestamp(deadline);
        if (!local_fence || !connection_healthy()) {
          return fail("selection_local_fence_event");
        }

        const auto confirm_cookie =
            xcb_get_selection_owner(connection_, selection_atom(sel));
        void *raw_confirm = nullptr;
        xcb_generic_error_t *raw_confirm_error = nullptr;
        if (!wait_for_reply(confirm_cookie.sequence, deadline, &raw_confirm,
                            &raw_confirm_error)) {
          return fail("selection_local_fence_confirm_reply");
        }
        auto confirmed = xcb_ptr(
            static_cast<xcb_get_selection_owner_reply_t *>(raw_confirm));
        auto confirm_error = xcb_ptr(raw_confirm_error);
        if (confirm_error || !confirmed || !connection_healthy()) {
          return fail("selection_local_fence_confirm");
        }
        if (!drain_queued_events_to_fence(deadline)) {
          return fail("selection_local_fence_history");
        }
        if (confirmed->owner != owner->owner) {
          continue;
        }

        if (state.baseline_initialized) {
          if (state.observed_owner == confirmed->owner &&
              state.observed_selection_timestamp != XCB_CURRENT_TIME) {
            return true;
          }
          continue;
        }
        state.observed_owner = confirmed->owner;
        state.observed_selection_timestamp = *local_fence;
        state.baseline_initialized = true;
        state.owner_generation = next_generation(state.owner_generation);
        state.owner_identity_generation =
            next_generation(state.owner_identity_generation);
        return true;
      }

      const auto timestamp_cookie =
          xcb_get_property(connection_, 1, *requestor, atom_read_property_,
                           XCB_ATOM_INTEGER, 0, 1);
      void *raw_timestamp = nullptr;
      xcb_generic_error_t *raw_timestamp_error = nullptr;
      if (!wait_for_reply(timestamp_cookie.sequence, deadline, &raw_timestamp,
                          &raw_timestamp_error)) {
        return fail("selection_timestamp_reply");
      }
      auto timestamp_reply =
          xcb_ptr(static_cast<xcb_get_property_reply_t *>(raw_timestamp));
      auto timestamp_error = xcb_ptr(raw_timestamp_error);
      if (timestamp_error || !timestamp_reply ||
          timestamp_reply->type != XCB_ATOM_INTEGER ||
          timestamp_reply->format != 32 || timestamp_reply->bytes_after != 0 ||
          xcb_get_property_value_length(timestamp_reply.get()) != 4) {
        return fail("selection_timestamp_format");
      }
      xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
      std::memcpy(&timestamp, xcb_get_property_value(timestamp_reply.get()),
                  sizeof(timestamp));
      if (timestamp == XCB_CURRENT_TIME) {
        return fail("selection_timestamp_zero");
      }

      const auto confirm_cookie =
          xcb_get_selection_owner(connection_, selection_atom(sel));
      void *raw_confirm = nullptr;
      xcb_generic_error_t *raw_confirm_error = nullptr;
      if (!wait_for_reply(confirm_cookie.sequence, deadline, &raw_confirm,
                          &raw_confirm_error)) {
        return fail("selection_confirm_reply");
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
      (void)pump_events_until(deadline);
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
      return fail("selection_poll");
    }
  }
  return fail("selection_deadline");
}

void ClipboardManager::fail_closed(std::source_location where) noexcept {
  close_impl(where, true);
}

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
  return arm_paste_receipt_impl(sel, nullptr);
}

std::optional<PasteReceiptToken>
ClipboardManager::arm_paste_receipt(Selection sel,
                                    const SelectionRead &source_context) {
  return arm_paste_receipt_impl(sel, &source_context);
}

std::optional<PasteReceiptToken>
ClipboardManager::arm_paste_receipt_impl(Selection sel,
                                         const SelectionRead *source_context) {
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
  if (source_context != nullptr &&
      !selection_identity_matches(Selection::Primary, *source_context,
                                  deadline)) {
    return std::nullopt;
  }
  next_receipt_id_ = next_generation(next_receipt_id_);
  state.armed_receipt_id = next_receipt_id_;
  state.armed_generation = state.generation;
  state.expected_requestor_client = active_client;
  state.receipt_context_owner = source_context == nullptr
                                    ? xcb_window_t{XCB_WINDOW_NONE}
                                    : source_context->owner;
  state.receipt_context_timestamp = source_context == nullptr
                                        ? XCB_CURRENT_TIME
                                        : source_context->selection_timestamp;
  state.receipt_context_owner_generation =
      source_context == nullptr ? 0 : source_context->owner_generation;
  state.receipt_context_connection_generation =
      source_context == nullptr ? 0 : source_context->connection_generation;
  state.confirmed_receipt_id = 0;
  state.receipt_request_sequence = 0;
  state.receipt_deadline = std::chrono::steady_clock::time_point::max();
  return PasteReceiptToken{sel, next_receipt_id_};
}

void ClipboardManager::activate_paste_receipt(
    const PasteReceiptToken &token,
    std::chrono::steady_clock::time_point deadline) noexcept {
  SelectionState &state = selection_state(token.selection);
  if (token.id != 0 && state.armed_receipt_id == token.id) {
    state.receipt_deadline = deadline;
  }
}

PasteReceiptStatus ClipboardManager::paste_receipt_status(
    const PasteReceiptToken &token) const noexcept {
  const SelectionState &state = selection_state(token.selection);
  if (token.id == 0 || state.armed_receipt_id != token.id) {
    return PasteReceiptStatus::Invalid;
  }
  if (state.confirmed_receipt_id == token.id) {
    return PasteReceiptStatus::Delivered;
  }
  if (std::chrono::steady_clock::now() >= state.receipt_deadline) {
    return PasteReceiptStatus::Expired;
  }
  return PasteReceiptStatus::Pending;
}

bool ClipboardManager::paste_receipt_seen(
    const PasteReceiptToken &token) const noexcept {
  return paste_receipt_status(token) == PasteReceiptStatus::Delivered;
}

std::uint64_t ClipboardManager::paste_receipt_request_seq(
    const PasteReceiptToken &token) const noexcept {
  const SelectionState &state = selection_state(token.selection);
  if (token.id == 0 || state.armed_receipt_id != token.id ||
      state.confirmed_receipt_id != token.id) {
    return 0;
  }
  return state.receipt_request_sequence;
}

void ClipboardManager::cancel_paste_receipt(
    const PasteReceiptToken &token) noexcept {
  SelectionState &state = selection_state(token.selection);
  if (state.armed_receipt_id == token.id) {
    state.armed_receipt_id = 0;
    state.armed_generation = 0;
    state.expected_requestor_client = 0;
    state.receipt_context_owner = XCB_WINDOW_NONE;
    state.receipt_context_timestamp = XCB_CURRENT_TIME;
    state.receipt_context_owner_generation = 0;
    state.receipt_context_connection_generation = 0;
    state.receipt_request_sequence = 0;
    state.receipt_deadline = std::chrono::steady_clock::time_point::max();
  }
  if (state.confirmed_receipt_id == token.id) {
    state.confirmed_receipt_id = 0;
  }
}

std::optional<OwnerTransitionToken> ClipboardManager::arm_owner_transition(
    Selection sel, const SelectionRead &source, std::uint32_t expected_client) {
  if (!is_open() || source.owner == XCB_WINDOW_NONE ||
      source.selection_timestamp == XCB_CURRENT_TIME || expected_client == 0 ||
      !current_connection(source) ||
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
  if (!is_open() || !current_connection(previous) ||
      previous.text.size() > maximum_payload_bytes()) {
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

  if (state.transition_status == OwnerTransitionStatus::SameClientOwner &&
      resource_client_id(state.observed_owner) !=
          state.transition_expected_client) {
    return false;
  }
  const SelectionRead current{"", state.observed_owner,
                              state.observed_selection_timestamp,
                              state.owner_generation, connection_generation_};
  const bool restored =
      publish_selection(token.selection,
                        previous.owner == XCB_WINDOW_NONE
                            ? std::nullopt
                            : std::optional<std::string_view>{previous.text},
                        current.selection_timestamp,
                        std::chrono::steady_clock::now() + timeout_, &current);
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

bool ClipboardManager::event_precedes_publication(
    const SelectionState &state, std::uint32_t sequence) noexcept {
  return state.owns && sequence - state.publication_sequence >
                           std::numeric_limits<std::uint32_t>::max() / 2;
}

bool ClipboardManager::handle_selection_request(
    const xcb_selection_request_event_t &request, std::uint32_t sequence,
    std::chrono::steady_clock::time_point deadline, bool current) {
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
    if (!connection_healthy()) {
      return false;
    }
    notify.property = property;
    // A refused payload has no remaining write authority. Deliver its refusal
    // without closing a healthy owner merely because preparation expired.
    auto response_deadline = deadline;
    if (property == XCB_ATOM_NONE) {
      if (!negative_response_deadline_) {
        negative_response_deadline_ =
            std::chrono::steady_clock::now() + timeout_;
      }
      response_deadline = *negative_response_deadline_;
      if (std::chrono::steady_clock::now() >= response_deadline) {
        return false;
      }
    }
    const auto cookie = xcb_send_event_checked(
        connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
        reinterpret_cast<const char *>(&notify));
    return complete_peer_requests(request.requestor, {{cookie, XCB_SEND_EVENT}},
                                  response_deadline) ==
           PeerCompletion::Delivered;
  };

  if (handling_selection_request_) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }
  handling_selection_request_ = true;
  struct RequestScope {
    bool &handling;
    std::optional<std::chrono::steady_clock::time_point> &negative_deadline;
    ~RequestScope() {
      negative_deadline.reset();
      handling = false;
    }
  } scope{handling_selection_request_, negative_response_deadline_};
  if (!current || std::chrono::steady_clock::now() >= deadline) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }

  SelectionState *state = nullptr;
  if (request.selection == atom_clipboard_) {
    state = &clipboard_;
  } else if (request.selection == atom_primary_) {
    state = &primary_;
  }

  const xcb_atom_t property =
      request.property == XCB_ATOM_NONE ? request.target : request.property;
  const auto send_property = [&](xcb_void_cookie_t cookie) {
    notify.property = property;
    const auto notify_cookie = xcb_send_event_checked(
        connection_, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
        reinterpret_cast<const char *>(&notify));
    return complete_peer_requests(
        request.requestor,
        {{cookie, XCB_CHANGE_PROPERTY}, {notify_cookie, XCB_SEND_EVENT}},
        deadline);
  };
  if (state == nullptr || !state->owns ||
      event_precedes_publication(*state, sequence) ||
      request.requestor == XCB_WINDOW_NONE || property == XCB_ATOM_NONE) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }

  if (request.target == atom_targets_) {
    std::vector<xcb_atom_t> targets{atom_targets_, atom_timestamp_};
    if (state->payload)
      for (const auto &data : *state->payload)
        targets.push_back(data.target);
    const auto property_cookie = xcb_change_property_checked(
        connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
        XCB_ATOM_ATOM, 32, static_cast<std::uint32_t>(targets.size()),
        targets.data());
    (void)send_property(property_cookie);
    return false;
  }

  if (request.target == atom_timestamp_ && state->observed_owner == window_ &&
      state->observed_selection_timestamp != XCB_CURRENT_TIME) {
    const xcb_timestamp_t timestamp = state->observed_selection_timestamp;
    const auto property_cookie = xcb_change_property_checked(
        connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
        XCB_ATOM_INTEGER, 32, 1, &timestamp);
    (void)send_property(property_cookie);
    return false;
  }

  const bool text_target = request.target == atom_utf8_string_ ||
                           request.target == atom_text_plain_utf8_ ||
                           request.target == atom_text_plain_ ||
                           request.target == XCB_ATOM_STRING;
  const auto payload = state->payload;
  const auto found = payload
                         ? std::find_if(payload->begin(), payload->end(),
                                        [&](const SelectionData &data) {
                                          return data.target == request.target;
                                        })
                         : std::vector<SelectionData>::const_iterator{};
  if (!payload || found == payload->end() || !valid_data(*found)) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }
  const auto &data = *found;

  if (state->armed_receipt_id != 0) {
    std::cerr << "[punto] Paste request receipt=" << state->armed_receipt_id
              << " generation=" << state->generation
              << " requestor=" << request.requestor
              << " requestor_client=" << resource_client_id(request.requestor)
              << " time=" << request.time << " property=" << property
              << " target=" << request.target << '\n';
  }

#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  punto_e2e_before_clipboard_payload_transfer(state->armed_receipt_id);
#endif

  if (state->armed_receipt_id != 0 &&
      resource_client_id(request.requestor) ==
          state->expected_requestor_client &&
      state->receipt_context_owner != XCB_WINDOW_NONE) {
    const SelectionRead expected_context{
        "", state->receipt_context_owner, state->receipt_context_timestamp,
        state->receipt_context_owner_generation,
        state->receipt_context_connection_generation};
    if (!selection_identity_matches(Selection::Primary, expected_context,
                                    deadline)) {
      (void)send_notify(XCB_ATOM_NONE);
      return false;
    }
  }

  if (state->armed_receipt_id != 0 &&
      state->confirmed_receipt_id != state->armed_receipt_id &&
      state->armed_generation == state->generation &&
      resource_client_id(request.requestor) ==
          state->expected_requestor_client &&
      std::chrono::steady_clock::now() >= state->receipt_deadline) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }

  const std::uint64_t generation = state->generation;
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  if (request.selection == atom_clipboard_) {
    std::cerr << "[punto-e2e] clipboard_transfer generation=" << generation
              << " tick_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count()
              << " bytes=" << data.bytes.size()
              << " receipt=" << state->armed_receipt_id
              << " requestor=" << request.requestor
              << " target=" << request.target << " property=" << property
              << '\n';
  }
#endif
  if (!state->owns || state->generation != generation ||
      state->payload != payload) {
    (void)send_notify(XCB_ATOM_NONE);
    return false;
  }
  if (data.bytes.size() > maximum_payload_bytes()) {
    // Reserve retirement capacity before installing a native watch. An active
    // transfer can turn into one cleanup duty, never an unbounded subscription.
    if (outgoing_.size() + retired_transfer_watches_.size() >=
            kOutgoingTransfers ||
        std::any_of(outgoing_.begin(), outgoing_.end(), [&](const auto &t) {
          return t.requestor == request.requestor && t.property == property;
        })) {
      (void)send_notify(XCB_ATOM_NONE);
      return false;
    }
    const std::uint32_t mask =
        XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    const auto watch_cookie = xcb_change_window_attributes_checked(
        connection_, request.requestor, XCB_CW_EVENT_MASK, &mask);
    const auto watch = complete_peer_requests(
        request.requestor, {{watch_cookie, XCB_CHANGE_WINDOW_ATTRIBUTES}},
        deadline);
    if (watch != PeerCompletion::Delivered) {
      if (watch == PeerCompletion::Expired)
        release_transfer_watch(request.requestor, deadline);
      return false;
    }
    const auto count = static_cast<std::uint32_t>(data.bytes.size());
    const auto property_cookie = xcb_change_property_checked(
        connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
        atom_incr_, 32, 1, &count);
    const auto header = send_property(property_cookie);
    if (header != PeerCompletion::Delivered) {
      if (header == PeerCompletion::Expired)
        release_transfer_watch(request.requestor, deadline);
      return false;
    }
    const Selection sel = request.selection == atom_primary_
                              ? Selection::Primary
                              : Selection::Clipboard;
    outgoing_.push_back(IncrementalTransfer{
        sel, request.requestor, property, payload,
        static_cast<std::size_t>(found - payload->begin()), 0, generation,
        text_target ? state->armed_receipt_id : 0,
        std::min(std::chrono::steady_clock::now() + kTransferBudget,
                 state->armed_receipt_id
                     ? state->receipt_deadline
                     : std::chrono::steady_clock::time_point::max()),
        next_transfer_id_ = next_generation(next_transfer_id_)});
    return false;
  }
  const auto property_cookie = xcb_change_property_checked(
      connection_, XCB_PROP_MODE_REPLACE, request.requestor, property,
      data.type, data.format,
      static_cast<std::uint32_t>(data.bytes.size() / (data.format / 8U)),
      data.bytes.data());
  if (send_property(property_cookie) != PeerCompletion::Delivered) {
    return false;
  }

  if (text_target)
    finish_transfer(request.selection == atom_primary_ ? Selection::Primary
                                                       : Selection::Clipboard,
                    generation, state->armed_receipt_id, request.requestor);
  return state->owns && state->generation == generation;
}

void ClipboardManager::finish_transfer(Selection sel, std::uint64_t generation,
                                       std::uint64_t receipt_id,
                                       xcb_window_t requestor) noexcept {
  auto &state = selection_state(sel);
  if (!state.owns || state.generation != generation)
    return;
  state.request_sequence = next_generation(state.request_sequence);
  if (receipt_id && state.armed_receipt_id == receipt_id &&
      state.armed_generation == generation &&
      resource_client_id(requestor) == state.expected_requestor_client) {
    state.confirmed_receipt_id = receipt_id;
    state.receipt_request_sequence =
        next_generation(state.receipt_request_sequence);
  }
}

void ClipboardManager::release_transfer_watch(
    xcb_window_t requestor, std::chrono::steady_clock::time_point deadline) {
  if (!connection_healthy())
    return;
  if (std::any_of(outgoing_.begin(), outgoing_.end(),
                  [&](const auto &t) { return t.requestor == requestor; })) {
    std::erase(retired_transfer_watches_, requestor);
    return;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    if (std::find(retired_transfer_watches_.begin(),
                  retired_transfer_watches_.end(),
                  requestor) == retired_transfer_watches_.end()) {
      if (retired_transfer_watches_.size() >= kOutgoingTransfers) {
        fail_closed(); // Admission must have reserved this resource's slot.
        return;
      }
      retired_transfer_watches_.push_back(requestor);
    }
    return;
  }
  const std::uint32_t mask = 0;
  const auto cookie = xcb_change_window_attributes_checked(
      connection_, requestor, XCB_CW_EVENT_MASK, &mask);
  (void)complete_peer_requests(
      requestor, {{cookie, XCB_CHANGE_WINDOW_ATTRIBUTES}}, deadline);
  // Even Expired means the checked mask=0 completed. Never submit it again.
  std::erase(retired_transfer_watches_, requestor);
}

bool ClipboardManager::handle_incremental_property(
    const xcb_property_notify_event_t &event,
    std::chrono::steady_clock::time_point deadline) {
  if (event.state != XCB_PROPERTY_DELETE)
    return false;
  auto it =
      std::find_if(outgoing_.begin(), outgoing_.end(), [&](const auto &t) {
        return t.requestor == event.window && t.property == event.atom;
      });
  if (it == outgoing_.end())
    return false;
  auto transfer = std::move(*it);
  outgoing_.erase(it);
  const bool previous_handling = handling_selection_request_;
  handling_selection_request_ = true;
  const auto reset = [&](bool *) {
    if (!previous_handling)
      negative_response_deadline_.reset();
    handling_selection_request_ = previous_handling;
  };
  const std::unique_ptr<bool, decltype(reset)> scope{
      &handling_selection_request_, reset};
  deadline = std::min({deadline, transfer.deadline,
                       std::chrono::steady_clock::now() + timeout_});
  auto &state = selection_state(transfer.selection);
  const auto abort = [&] {
    release_transfer_watch(transfer.requestor, deadline);
    return true;
  };
  if (!connection_healthy() || std::chrono::steady_clock::now() >= deadline ||
      (transfer.receipt_id &&
       (!state.owns || state.generation != transfer.generation ||
        state.payload != transfer.payload)))
    return abort();
  if (transfer.receipt_id) {
    if (state.armed_receipt_id != transfer.receipt_id ||
        std::chrono::steady_clock::now() >= state.receipt_deadline)
      return abort();
    if (state.receipt_context_owner != XCB_WINDOW_NONE &&
        !selection_identity_matches(
            Selection::Primary,
            SelectionRead{"", state.receipt_context_owner,
                          state.receipt_context_timestamp,
                          state.receipt_context_owner_generation,
                          state.receipt_context_connection_generation},
            deadline))
      return abort();
  }
  if (transfer.receipt_id &&
      (!state.owns || state.generation != transfer.generation ||
       state.payload != transfer.payload))
    return abort();
  const auto &data = transfer.payload->at(transfer.entry);
  const auto remaining = data.bytes.size() - transfer.offset;
  const auto unit = static_cast<std::size_t>(data.format / 8U);
  const auto count = std::min(remaining, maximum_payload_bytes()) / unit * unit;
  if (remaining && count == 0)
    return abort();
  const auto cookie = xcb_change_property_checked(
      connection_, XCB_PROP_MODE_REPLACE, transfer.requestor, transfer.property,
      data.type, data.format,
      static_cast<std::uint32_t>(count / (data.format / 8U)),
      count ? data.bytes.data() + transfer.offset : nullptr);
  const auto chunk = complete_peer_requests(
      transfer.requestor, {{cookie, XCB_CHANGE_PROPERTY}}, deadline);
  if (chunk != PeerCompletion::Delivered) {
    if (chunk == PeerCompletion::Expired)
      release_transfer_watch(transfer.requestor, deadline);
    return true;
  }
  if (remaining == 0) {
    const bool text =
        data.target == atom_utf8_string_ || data.target == XCB_ATOM_STRING ||
        data.target == atom_text_plain_ || data.target == atom_text_plain_utf8_;
    if (text)
      finish_transfer(transfer.selection, transfer.generation,
                      transfer.receipt_id, transfer.requestor);
    release_transfer_watch(transfer.requestor, deadline);
  } else {
    transfer.offset += count;
    outgoing_.push_back(std::move(transfer));
  }
  return true;
}

ClipboardManager::PeerCompletion ClipboardManager::complete_peer_requests(
    xcb_window_t requestor, std::initializer_list<PeerRequest> requests,
    std::chrono::steady_clock::time_point deadline) {
  if (!connection_healthy())
    return PeerCompletion::Fatal;
  const auto barrier = xcb_get_input_focus(connection_);
  x11_detail::XcbOperationResult result{};
  std::free(connection_owner_.wait_for_read_reply(barrier.sequence, deadline,
                                                  result));
  if (result != x11_detail::XcbOperationResult::Success &&
      result != x11_detail::XcbOperationResult::TimedOut) {
    fail_closed();
    return PeerCompletion::Fatal;
  }
  bool delivered = result == x11_detail::XcbOperationResult::Success;
  bool gone = false;
  bool fatal = false;
  // Read every actual checked result, even if scheduling exhausted the ACK
  // budget. Completed peer writes do not revoke publisher ownership; a late
  // result still cannot acknowledge delivery. Unknown results remain fatal.
  for (const auto &request : requests) {
    void *reply = nullptr;
    xcb_generic_error_t *error = nullptr;
    const int ready = xcb_poll_for_reply(connection_, request.cookie.sequence,
                                         &reply, &error);
    if (!ready || reply != nullptr) {
      fatal = true;
    } else if (error != nullptr) {
      const bool requestor_gone =
          requestor != XCB_WINDOW_NONE && requestor != window_ &&
          error->error_code == XCB_WINDOW && error->resource_id == requestor &&
          error->major_code == request.opcode &&
          error->full_sequence == request.cookie.sequence &&
          error->sequence ==
              static_cast<std::uint16_t>(request.cookie.sequence);
      fatal = fatal || !requestor_gone;
      gone = gone || requestor_gone;
      delivered = false;
    }
    std::free(reply);
    std::free(error);
  }
  fatal = fatal || !connection_healthy();
  delivered = delivered && std::chrono::steady_clock::now() < deadline;
  if (fatal) {
    fail_closed();
    return PeerCompletion::Fatal;
  }
  if (gone) {
    std::erase_if(outgoing_, [&](const auto &transfer) {
      return transfer.requestor == requestor;
    });
    std::erase(retired_transfer_watches_, requestor);
    return PeerCompletion::Gone;
  }
  return delivered ? PeerCompletion::Delivered : PeerCompletion::Expired;
}

void ClipboardManager::handle_selection_clear(
    const xcb_selection_clear_event_t &event, std::uint32_t sequence) noexcept {
  SelectionState *state = nullptr;
  if (event.selection == atom_clipboard_) {
    state = &clipboard_;
  } else if (event.selection == atom_primary_) {
    state = &primary_;
  }
  if (state != nullptr && !event_precedes_publication(*state, sequence)) {
    state->owns = false;
    state->payload.reset();
    state->armed_receipt_id = 0;
    state->armed_generation = 0;
    state->expected_requestor_client = 0;
    state->receipt_context_owner = XCB_WINDOW_NONE;
    state->receipt_context_timestamp = XCB_CURRENT_TIME;
    state->receipt_context_owner_generation = 0;
    state->receipt_context_connection_generation = 0;
    state->confirmed_receipt_id = 0;
    state->receipt_request_sequence = 0;
    state->receipt_deadline = std::chrono::steady_clock::time_point::max();
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
  if (event_precedes_publication(*state, event.full_sequence)) {
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
  state->previous_observed_owner = state->observed_owner;
  state->observed_subtype = change.subtype;
  state->observed_owner = change.owner;
  state->observed_selection_timestamp = change.selection_timestamp;
  state->baseline_initialized = true;
  state->owner_generation = next_generation(state->owner_generation);
  if (change.owner != window_) {
    state->owns = false;
    state->payload.reset();
  }
}

std::size_t ClipboardManager::pump_events() {
  if (event_budget_)
    return pump_events_until(std::chrono::steady_clock::time_point::max());
  event_budget_ = kPumpEventBudget;
  const auto reset = [&](std::optional<std::size_t> *) {
    event_budget_.reset();
  };
  const std::unique_ptr<std::optional<std::size_t>, decltype(reset)> scope{
      &event_budget_, reset};
  return pump_events_until(std::chrono::steady_clock::time_point::max());
}

bool ClipboardManager::event_budget_available() const noexcept {
  return !event_budget_ || *event_budget_ != 0;
}

void ClipboardManager::record_polled_event() noexcept {
  if (event_budget_)
    --*event_budget_;
}

std::size_t ClipboardManager::pump_events_until(
    std::chrono::steady_clock::time_point operation_deadline) {
  if (!connection_healthy()) {
    if (connection_ != nullptr) {
      fail_closed();
    }
    return 0;
  }

  if (!retired_transfer_watches_.empty() &&
      std::chrono::steady_clock::now() < operation_deadline) {
    release_transfer_watch(
        retired_transfer_watches_.front(),
        std::min(operation_deadline,
                 std::chrono::steady_clock::now() + timeout_));
    if (!connection_healthy())
      return 0;
  }

  for (auto it = outgoing_.begin(); it != outgoing_.end();) {
    if (std::chrono::steady_clock::now() < it->deadline &&
        (!it->receipt_id ||
         (selection_state(it->selection).owns &&
          selection_state(it->selection).generation == it->generation))) {
      ++it;
      continue;
    }
    const auto requestor = it->requestor;
    const auto transfer_deadline = it->deadline;
    it = outgoing_.erase(it);
    release_transfer_watch(
        requestor, std::min({operation_deadline, transfer_deadline,
                             std::chrono::steady_clock::now() + timeout_}));
    if (!connection_healthy()) {
      fail_closed();
      return 0;
    }
  }
  if (!handling_selection_request_ && !deferred_peer_events_.empty() &&
      event_budget_available() &&
      std::chrono::steady_clock::now() < operation_deadline) {
    service_deferred_peer_event(operation_deadline);
    return 1;
  }
  const auto deadline = std::min(
      operation_deadline, std::chrono::steady_clock::now() + kPumpTimeBudget);
  std::size_t processed = 0;
  while (event_budget_available() && processed < kPumpEventBudget &&
         std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(connection_));
    if (!event) {
      break;
    }
    ++processed;
    record_polled_event();
    if (process_event(*event,
                      std::min(operation_deadline,
                               std::chrono::steady_clock::now() + timeout_))) {
      break;
    }
    if (!connection_healthy()) {
      fail_closed();
      break;
    }
  }
  return processed;
}

void ClipboardManager::service_deferred_peer_event(
    std::chrono::steady_clock::time_point operation_deadline) {
  const auto pending = deferred_peer_events_.front();
  deferred_peer_events_.pop_front();
  record_polled_event();
  const auto deadline = std::min(operation_deadline, pending.deadline);
  const auto type = pending.event.response_type & 0x7fU;
  const bool publication_bound =
      type == XCB_SELECTION_REQUEST || pending.receipt_id != 0;
  auto &state = selection_state(pending.selection);
  const auto current_authority = [&] {
    return pending.connection_generation == connection_generation_ &&
           std::chrono::steady_clock::now() < deadline &&
           (!publication_bound ||
            (state.owns && state.generation == pending.generation &&
             state.armed_receipt_id == pending.receipt_id &&
             (!pending.receipt_id ||
              std::chrono::steady_clock::now() < state.receipt_deadline)));
  };
  bool current = current_authority();
  if (current) {
    // Fence peer history without recursively serving another peer.
    handling_selection_request_ = true;
    const auto reset = [&](bool *) {
      handling_selection_request_ = false;
      negative_response_deadline_.reset();
    };
    const std::unique_ptr<bool, decltype(reset)> scope{
        &handling_selection_request_, reset};
    if (publication_bound) {
      current = selection_identity_matches(
          pending.selection,
          SelectionRead{"", state.observed_owner,
                        state.observed_selection_timestamp,
                        state.owner_generation, connection_generation_},
          deadline);
    } else {
      // Accepted raw snapshots survive clipboard ownership changes, but not
      // requestor destruction. The reply fences history, not focus authority.
      const auto fence = xcb_get_input_focus(connection_);
      void *raw_reply = nullptr;
      xcb_generic_error_t *raw_error = nullptr;
      const bool replied =
          wait_for_reply(fence.sequence, deadline, &raw_reply, &raw_error);
      auto reply =
          xcb_ptr(static_cast<xcb_get_input_focus_reply_t *>(raw_reply));
      auto error = xcb_ptr(raw_error);
      current = replied && reply && !error &&
                drain_queued_events_to_fence(deadline,
                                             RequestScan::FinishQueuedHistory);
    }
  }
  current = current && current_authority();
  if (type == XCB_SELECTION_REQUEST) {
    xcb_selection_request_event_t request{};
    std::memcpy(&request, &pending.event, sizeof(request));
    (void)handle_selection_request(request, pending.event.full_sequence,
                                   deadline, current);
  } else {
    xcb_property_notify_event_t event{};
    std::memcpy(&event, &pending.event, sizeof(event));
    const auto transfer =
        std::find_if(outgoing_.begin(), outgoing_.end(), [&](const auto &item) {
          return item.id == pending.transfer_id &&
                 item.requestor == event.window && item.property == event.atom;
        });
    if (transfer != outgoing_.end())
      (void)handle_incremental_property(
          event, current ? deadline : std::chrono::steady_clock::now());
  }
}

bool ClipboardManager::process_event(
    const xcb_generic_event_t &event,
    std::chrono::steady_clock::time_point selection_request_deadline,
    RequestScan requests) {
  const std::uint8_t type =
      static_cast<std::uint8_t>(event.response_type & 0x7fU);
  if (type == 0) {
    fail_closed();
    return true;
  }
  if ((handling_selection_request_ || !deferred_peer_events_.empty()) &&
      (type == XCB_SELECTION_REQUEST || type == XCB_PROPERTY_NOTIFY)) {
    const auto *request =
        reinterpret_cast<const xcb_selection_request_event_t *>(&event);
    const auto *property =
        reinterpret_cast<const xcb_property_notify_event_t *>(&event);
    auto transfer = outgoing_.end();
    if (type == XCB_PROPERTY_NOTIFY && property->state == XCB_PROPERTY_DELETE)
      transfer = std::find_if(outgoing_.begin(), outgoing_.end(),
                              [&](const auto &item) {
                                return item.requestor == property->window &&
                                       item.property == property->atom;
                              });
    if (type == XCB_SELECTION_REQUEST || transfer != outgoing_.end()) {
      if (deferred_peer_events_.size() >= kPumpEventBudget) {
        if (type == XCB_SELECTION_REQUEST)
          (void)handle_selection_request(*request, event.full_sequence,
                                         selection_request_deadline, false);
        return true;
      }
      const auto selection =
          type == XCB_SELECTION_REQUEST
              ? (request->selection == atom_primary_ ? Selection::Primary
                                                     : Selection::Clipboard)
              : transfer->selection;
      const auto &state = selection_state(selection);
      const bool request_event = type == XCB_SELECTION_REQUEST;
      deferred_peer_events_.push_back(DeferredPeerEvent{
          event, selection, connection_generation_,
          request_event ? state.generation : transfer->generation,
          request_event ? state.armed_receipt_id : transfer->receipt_id,
          request_event ? 0 : transfer->id,
          std::min({selection_request_deadline,
                    request_event ? state.receipt_deadline : transfer->deadline,
                    std::chrono::steady_clock::now() + timeout_})});
      return type == XCB_SELECTION_REQUEST && requests == RequestScan::Yield;
    }
  }
  if (type == XCB_SELECTION_REQUEST) {
    // Ordinary scans yield after one request. Restoration finishes only the
    // bounded queued history within its original timestamp/fence deadline.
    (void)handle_selection_request(
        *reinterpret_cast<const xcb_selection_request_event_t *>(&event),
        event.full_sequence,
        std::min(selection_request_deadline,
                 std::chrono::steady_clock::now() + timeout_));
    return requests == RequestScan::Yield;
  }
  if (type == XCB_DESTROY_NOTIFY) {
    const auto &destroy =
        *reinterpret_cast<const xcb_destroy_notify_event_t *>(&event);
    std::erase_if(outgoing_,
                  [&](const auto &t) { return t.requestor == destroy.window; });
    std::erase(retired_transfer_watches_, destroy.window);
    std::erase_if(deferred_peer_events_, [&](const auto &pending) {
      if ((pending.event.response_type & 0x7fU) == XCB_SELECTION_REQUEST) {
        xcb_selection_request_event_t request{};
        std::memcpy(&request, &pending.event, sizeof(request));
        return request.requestor == destroy.window;
      }
      xcb_property_notify_event_t property{};
      std::memcpy(&property, &pending.event, sizeof(property));
      return property.window == destroy.window;
    });
  } else if (type == XCB_PROPERTY_NOTIFY) {
    (void)handle_incremental_property(
        *reinterpret_cast<const xcb_property_notify_event_t *>(&event),
        selection_request_deadline);
  } else if (type == XCB_SELECTION_CLEAR) {
    handle_selection_clear(
        *reinterpret_cast<const xcb_selection_clear_event_t *>(&event),
        event.full_sequence);
  } else if (xfixes_event_base_ != 0 &&
             type == xfixes_event_base_ + XCB_XFIXES_SELECTION_NOTIFY) {
    handle_owner_change(event);
  }
  return false;
}

bool ClipboardManager::drain_queued_events_to_fence(
    std::chrono::steady_clock::time_point deadline, RequestScan requests,
    xcb_generic_event_t **guarded_request) {
  std::size_t processed = 0;
  while (event_budget_available() && processed < kPumpEventBudget &&
         std::chrono::steady_clock::now() < deadline) {
    // wait_for_reply() has already read through the fence reply. Only consume
    // events XCB queued before that boundary; newer socket traffic belongs to
    // a later observation.
    auto event = xcb_ptr(xcb_poll_for_queued_event(connection_));
    if (!event) {
      return connection_healthy();
    }
    ++processed;
    record_polled_event();
    if (guarded_request != nullptr &&
        (event->response_type & 0x7fU) == XCB_SELECTION_REQUEST) {
      // The caller releases its server grab before serving this exact event.
      *guarded_request = event.release();
      return false;
    }
    if (process_event(*event, deadline,
                      handling_selection_request_
                          ? RequestScan::FinishQueuedHistory
                          : requests) ||
        !connection_healthy()) {
      return false;
    }
  }
  // Hitting either bound leaves the pre-fence history incomplete. Exactly a
  // full batch is conservatively unknown because proving the queue empty would
  // consume another event.
  return false;
}

std::optional<xcb_window_t> ClipboardManager::request_selection_conversion(
    Selection sel, xcb_atom_t target, xcb_timestamp_t timestamp,
    std::chrono::steady_clock::time_point deadline) {
  if (!is_open() || std::chrono::steady_clock::now() >= deadline) {
    return std::nullopt;
  }
  const auto requestor = xcb_generate_id(connection_);
  const auto create = xcb_create_window_checked(
      connection_, XCB_COPY_FROM_PARENT, requestor, screen_->root, 0, 0, 1, 1,
      0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, XCB_CW_EVENT_MASK,
      std::array<std::uint32_t, 1>{XCB_EVENT_MASK_PROPERTY_CHANGE}.data());
  if (!complete_checked_request(create, deadline)) {
    log_read_failure(sel, "query_window");
    fail_closed();
    return std::nullopt;
  }
  const auto convert =
      xcb_convert_selection_checked(connection_, requestor, selection_atom(sel),
                                    target, atom_read_property_, timestamp);
  if (!complete_checked_request(convert, deadline)) {
    log_read_failure(sel, "convert_request");
    fail_closed();
    return std::nullopt;
  }
  return requestor;
}

std::optional<xcb_selection_notify_event_t>
ClipboardManager::wait_for_selection_notify(
    xcb_window_t requestor, xcb_atom_t selection, xcb_atom_t target,
    xcb_atom_t property, std::chrono::steady_clock::time_point deadline) {
  const auto event = wait_for_query_event(
      requestor, selection, target, property, XCB_SELECTION_NOTIFY, deadline);
  if (!event)
    return std::nullopt;
  xcb_selection_notify_event_t notify{};
  std::memcpy(&notify, &*event, sizeof(notify));
  return notify;
}

std::optional<xcb_generic_event_t> ClipboardManager::wait_for_query_event(
    xcb_window_t requestor, xcb_atom_t selection, xcb_atom_t target,
    xcb_atom_t property, std::uint8_t event_type,
    std::chrono::steady_clock::time_point deadline) {
  while (connection_healthy() && event_budget_available() &&
         std::chrono::steady_clock::now() < deadline) {
    std::size_t batch = 0;
    while (event_budget_available() && batch < kPumpEventBudget &&
           std::chrono::steady_clock::now() < deadline) {
      auto event = xcb_ptr(xcb_poll_for_event(connection_));
      if (!event) {
        break;
      }
      ++batch;
      record_polled_event();
      const std::uint8_t type =
          static_cast<std::uint8_t>(event->response_type & 0x7fU);
      if (type == 0) {
        fail_closed();
        return std::nullopt;
      }
      if (event_type == XCB_PROPERTY_NOTIFY && type == XCB_PROPERTY_NOTIFY) {
        const auto &change =
            *reinterpret_cast<const xcb_property_notify_event_t *>(event.get());
        if (change.window == requestor && change.atom == property &&
            change.state == XCB_PROPERTY_NEW_VALUE)
          return *event;
      }
      if (type == XCB_SELECTION_REQUEST || type == XCB_PROPERTY_NOTIFY) {
        if (process_event(*event, deadline, RequestScan::FinishQueuedHistory))
          return std::nullopt;
      } else if (type == XCB_SELECTION_CLEAR) {
        handle_selection_clear(
            *reinterpret_cast<const xcb_selection_clear_event_t *>(event.get()),
            event->full_sequence);
      } else if (xfixes_event_base_ != 0 &&
                 type == xfixes_event_base_ + XCB_XFIXES_SELECTION_NOTIFY) {
        handle_owner_change(*event);
      } else if (event_type == XCB_SELECTION_NOTIFY &&
                 type == XCB_SELECTION_NOTIFY) {
        const auto &notify =
            *reinterpret_cast<const xcb_selection_notify_event_t *>(
                event.get());
        if (notify.requestor == requestor && notify.selection == selection &&
            notify.target == target &&
            (notify.property == property || notify.property == XCB_ATOM_NONE)) {
          return *event;
        }
      }
    }

    if (!connection_healthy()) {
      fail_closed();
      return std::nullopt;
    }
    if (!event_budget_available())
      return std::nullopt;
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
ClipboardManager::get_text_with_owner(Selection sel, ClipboardResult *result) {
  // Preserve the standalone initialization envelope before its read timeout.
  if (!is_open() && !open()) {
    log_read_failure(sel, "open");
    if (result != nullptr) {
      *result = ClipboardResult::NoConnection;
    }
    return std::nullopt;
  }
  return get_text_with_owner(sel, std::chrono::steady_clock::now() + timeout_,
                             result);
}

std::optional<SelectionData> ClipboardManager::read_target(
    Selection sel, xcb_atom_t target, const SelectionRead &origin,
    std::chrono::steady_clock::time_point deadline, ClipboardResult *result) {
  const auto finish = [&](ClipboardResult status) {
    if (result)
      *result = status;
  };
  finish(ClipboardResult::ConversionFailed);
  const auto requestor =
      request_selection_conversion(sel, target, XCB_CURRENT_TIME, deadline);
  if (!requestor) {
    finish(is_open() ? ClipboardResult::Timeout
                     : ClipboardResult::NoConnection);
    return std::nullopt;
  }
  ScopedSelectionQuery query{connection_owner_, connection_generation_,
                             *requestor};
  const auto notify = wait_for_selection_notify(
      *requestor, selection_atom(sel), target, atom_read_property_, deadline);
  if (!notify) {
    log_read_failure(sel, "selection_notify_timeout");
    finish(is_open() ? ClipboardResult::Timeout
                     : ClipboardResult::NoConnection);
    return std::nullopt;
  }
  if (notify->property == XCB_ATOM_NONE) {
    if (selection_identity_matches(sel, origin, deadline)) {
      log_read_failure(sel, "selection_notify_rejected");
      finish(ClipboardResult::NoSelection);
    }
    return std::nullopt;
  }
  // Retire the header's queued NewValue before deletion acknowledges it.
  // Until that deletion, a conforming INCR sender cannot publish a data chunk.
  if (!drain_queued_events_to_fence(deadline, RequestScan::FinishQueuedHistory))
    return std::nullopt;
  const auto property = [&]() -> XcbPtr<xcb_get_property_reply_t> {
    const auto cookie = xcb_get_property(
        connection_, 1, *requestor, atom_read_property_,
        XCB_GET_PROPERTY_TYPE_ANY, 0,
        static_cast<std::uint32_t>((kRawPayloadBytes + 3U) / 4U));
    void *reply = nullptr;
    xcb_generic_error_t *error = nullptr;
    if (!wait_for_reply(cookie.sequence, deadline, &reply, &error)) {
      std::free(reply);
      std::free(error);
      fail_closed();
      return xcb_ptr<xcb_get_property_reply_t>(nullptr);
    }
    auto owned_error = xcb_ptr(error);
    if (owned_error || !reply) {
      std::free(reply);
      fail_closed();
      return xcb_ptr<xcb_get_property_reply_t>(nullptr);
    }
    return xcb_ptr(static_cast<xcb_get_property_reply_t *>(reply));
  };
  auto reply = property();
  SelectionData data{target};
  bool incremental = reply && reply->type == atom_incr_;
  if (incremental) {
    std::uint32_t hint = 0;
    if (reply->format != 32 || reply->bytes_after != 0 ||
        xcb_get_property_value_length(reply.get()) != 4)
      return std::nullopt;
    std::memcpy(&hint, xcb_get_property_value(reply.get()), sizeof(hint));
    if (hint > kRawPayloadBytes)
      return std::nullopt; // Never allocate from hint.
  }
  for (std::size_t chunks = 0; chunks < kIncomingChunks; ++chunks) {
    if (incremental) {
      if (!wait_for_query_event(*requestor, selection_atom(sel), target,
                                atom_read_property_, XCB_PROPERTY_NOTIFY,
                                deadline)) {
        finish(is_open() ? ClipboardResult::Timeout
                         : ClipboardResult::NoConnection);
        return std::nullopt;
      }
      reply = property();
    }
    if (!reply || !connection_healthy()) {
      finish(ClipboardResult::NoConnection);
      return std::nullopt;
    }
    const int length = xcb_get_property_value_length(reply.get());
    if (length < 0 || reply->type == atom_incr_ || reply->bytes_after != 0 ||
        (reply->format != 8 && reply->format != 16 && reply->format != 32) ||
        reply->type == XCB_ATOM_NONE ||
        static_cast<std::size_t>(length) >
            kRawPayloadBytes - data.bytes.size() ||
        static_cast<std::size_t>(length) % (reply->format / 8U) != 0)
      return std::nullopt;
    if (data.type == XCB_ATOM_NONE) {
      data.type = reply->type;
      data.format = reply->format;
    }
    if (data.type != reply->type || data.format != reply->format)
      return std::nullopt;
    if (length > 0) {
      const auto *bytes = static_cast<const std::uint8_t *>(
          xcb_get_property_value(reply.get()));
      data.bytes.insert(data.bytes.end(), bytes, bytes + length);
    }
    if (!incremental || length == 0) {
      if (!selection_identity_matches(sel, origin, deadline)) {
        if (!is_open())
          finish(ClipboardResult::NoConnection);
        return std::nullopt;
      }
      finish(ClipboardResult::Ok);
      return data;
    }
  }
  return std::nullopt;
}

std::optional<SelectionRead> ClipboardManager::get_text_with_owner(
    Selection sel, std::chrono::steady_clock::time_point deadline,
    ClipboardResult *result) {
  if (result)
    *result = ClipboardResult::ConversionFailed;
  const auto origin = get_selection_identity(sel, deadline);
  if (!origin) {
    log_read_failure(sel, "read_origin");
    if (result && !is_open())
      *result = ClipboardResult::NoConnection;
    return std::nullopt;
  }
  if (origin->owner == XCB_WINDOW_NONE) {
    if (result)
      *result = ClipboardResult::Ok;
    return origin;
  }
  const auto data =
      read_target(sel, atom_utf8_string_, *origin, deadline, result);
  if (!data)
    return std::nullopt;
  const PropertyBytes property{data->type, data->format, 0, data->bytes.data(),
                               data->bytes.size()};
  const auto text =
      decode_text_property(property, atom_utf8_string_, atom_incr_);
  if (!text) {
    if (result)
      *result = ClipboardResult::ConversionFailed;
    return std::nullopt;
  }
  auto read = *origin;
  read.text = *text;
  return read;
}

bool ClipboardManager::portable_target(
    xcb_atom_t target, std::chrono::steady_clock::time_point deadline) {
  if (target == atom_utf8_string_ || target == atom_text_plain_ ||
      target == atom_text_plain_utf8_ || target == XCB_ATOM_STRING ||
      target == atom_compound_text_ || target == atom_text_)
    return true;
  const auto cookie = xcb_get_atom_name(connection_, target);
  void *raw = nullptr;
  xcb_generic_error_t *error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw, &error,
                      ReplyIntent::Observation)) {
    std::free(raw);
    std::free(error);
    if (!connection_healthy())
      fail_closed();
    return false;
  }
  auto name = xcb_ptr(static_cast<xcb_get_atom_name_reply_t *>(raw));
  auto owned_error = xcb_ptr(error);
  if (!name || owned_error || name->name_len == 0 || name->name_len > 128)
    return false;
  const std::string_view value{xcb_get_atom_name_name(name.get()),
                               name->name_len};
  const auto token = [](char byte) {
    const auto code = static_cast<unsigned char>(byte);
    return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
           (code >= '0' && code <= '9') || byte == '.' || byte == '-' ||
           byte == '+' || byte == '_';
  };
  const auto slash = value.find('/');
  if (slash == std::string_view::npos)
    return false;
  const auto tail = value.substr(slash + 1U);
  const auto subtype = tail.substr(0, tail.find(';'));
  // Private MIME byte data is portable; arbitrary private/resource atom names
  // provide no such conversion contract and are never probed.
  return !subtype.empty() &&
         std::all_of(subtype.begin(), subtype.end(), token) &&
         std::all_of(tail.begin(), tail.end(),
                     [&](char byte) {
                       return token(byte) || byte == ';' || byte == '=';
                     }) &&
         (value.starts_with("text/") || value == "image/png" ||
          value == "image/jpeg" || value.starts_with("application/"));
}

std::optional<SelectionSnapshot> ClipboardManager::capture_payload(
    Selection sel, std::chrono::steady_clock::time_point deadline) {
  const auto origin = get_selection_identity(sel, deadline);
  if (!origin)
    return std::nullopt;
  if (origin->owner == XCB_WINDOW_NONE)
    return SelectionSnapshot{*origin, {}};
  const auto catalogue =
      read_target(sel, atom_targets_, *origin, deadline, nullptr);
  if (!catalogue || catalogue->type != XCB_ATOM_ATOM ||
      catalogue->format != 32 || catalogue->bytes.empty() ||
      catalogue->bytes.size() > kRawTargets * 4U)
    return std::nullopt;
  std::vector<xcb_atom_t> targets(catalogue->bytes.size() / 4U);
  std::memcpy(targets.data(), catalogue->bytes.data(), catalogue->bytes.size());
  auto payload = std::make_shared<std::vector<SelectionData>>();
  std::vector<xcb_atom_t> data_targets;
  for (const auto target : targets) {
    if (target == atom_targets_ || target == atom_timestamp_ ||
        target == atom_multiple_ || target == atom_save_targets_)
      continue;
    if (std::find(data_targets.begin(), data_targets.end(), target) !=
            data_targets.end() ||
        !portable_target(target, deadline))
      return std::nullopt;
    data_targets.push_back(target);
  }
  if (data_targets.empty())
    return std::nullopt;
  std::size_t total = 0;
  for (const auto target : data_targets) {
    auto data = read_target(sel, target, *origin, deadline, nullptr);
    if (!data || !valid_data(*data) || data->type == XCB_ATOM_WINDOW ||
        data->type == XCB_ATOM_PIXMAP || data->type == XCB_ATOM_DRAWABLE ||
        data->type == XCB_ATOM_COLORMAP || data->type == XCB_ATOM_CURSOR ||
        data->type == XCB_ATOM_FONT || data->type == XCB_ATOM_ATOM ||
        data->bytes.size() > kRawPayloadBytes - total)
      return std::nullopt;
    total += data->bytes.size();
    payload->push_back(std::move(*data));
  }
  if (!selection_identity_matches(sel, *origin, deadline))
    return std::nullopt;
  return SelectionSnapshot{*origin, std::move(payload)};
}

SelectionPayload ClipboardManager::text_payload(std::string_view text) const {
  if (text.size() > kRawPayloadBytes / 4U)
    return {};
  auto payload = std::make_shared<std::vector<SelectionData>>();
  for (const auto target : {atom_utf8_string_, atom_text_plain_utf8_,
                            atom_text_plain_, xcb_atom_t{XCB_ATOM_STRING}}) {
    payload->push_back(
        SelectionData{target, target, 8,
                      std::vector<std::uint8_t>(text.begin(), text.end())});
  }
  return payload;
}

std::optional<std::string> ClipboardManager::get_text(Selection sel) {
  auto read = get_text_with_owner(sel);
  if (!read) {
    return std::nullopt;
  }
  return std::move(read->text);
}

std::optional<SelectionRead>
ClipboardManager::get_selection_identity(Selection sel) {
  return get_selection_identity(sel,
                                std::chrono::steady_clock::now() + timeout_);
}

std::optional<SelectionRead> ClipboardManager::get_selection_identity(
    Selection sel, std::chrono::steady_clock::time_point deadline) {
  if ((!is_open() && !open(deadline)) ||
      std::chrono::steady_clock::now() >= deadline) {
    return std::nullopt;
  }
  (void)pump_events_until(deadline);
  if (!is_open() || std::chrono::steady_clock::now() >= deadline) {
    return std::nullopt;
  }

  const auto cookie = xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error,
                      ReplyIntent::Observation)) {
    if (!connection_healthy())
      fail_closed();
    return std::nullopt;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto error = xcb_ptr(raw_error);
  if (error || !owner) {
    fail_closed();
    return std::nullopt;
  }
  if (!drain_queued_events_to_fence(deadline,
                                    RequestScan::FinishQueuedHistory) ||
      !is_open()) {
    if (connection_ != nullptr && !connection_healthy()) {
      fail_closed();
    }
    return std::nullopt;
  }

  const SelectionState &state = selection_state(sel);
  if (state.observed_owner != owner->owner) {
    return std::nullopt;
  }
  if (owner->owner == XCB_WINDOW_NONE) {
    return SelectionRead{"", XCB_WINDOW_NONE, XCB_CURRENT_TIME,
                         state.owner_generation, connection_generation_};
  }
  if (state.observed_selection_timestamp == XCB_CURRENT_TIME) {
    return std::nullopt;
  }
  return SelectionRead{"", owner->owner, state.observed_selection_timestamp,
                       state.owner_generation, connection_generation_};
}

bool ClipboardManager::selection_identity_matches(
    Selection sel, const SelectionRead &expected) {
  return selection_identity_matches(
      sel, expected, std::chrono::steady_clock::now() + timeout_);
}

bool ClipboardManager::selection_identity_matches(
    Selection sel, const SelectionRead &expected,
    std::chrono::steady_clock::time_point deadline) {
  if (!is_open() || expected.owner == XCB_WINDOW_NONE ||
      expected.selection_timestamp == XCB_CURRENT_TIME ||
      !current_connection(expected) ||
      std::chrono::steady_clock::now() >= deadline) {
    return false;
  }
  (void)pump_events_until(deadline);
  if (!is_open() || std::chrono::steady_clock::now() >= deadline) {
    return false;
  }
  const auto cookie = xcb_get_selection_owner(connection_, selection_atom(sel));
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error,
                      ReplyIntent::Observation)) {
    if (!connection_healthy())
      fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto error = xcb_ptr(raw_error);
  // Ordinary readers do not revoke identity. Finish the bounded pre-fence
  // history before comparing the complete owner token.
  if (!drain_queued_events_to_fence(deadline,
                                    RequestScan::FinishQueuedHistory) ||
      !is_open()) {
    return false;
  }
  const SelectionState &state = selection_state(sel);
  return !error && owner && owner->owner == expected.owner &&
         state.observed_owner == expected.owner &&
         state.observed_selection_timestamp == expected.selection_timestamp &&
         state.owner_generation == expected.owner_generation &&
         connection_healthy();
}

std::optional<bool>
ClipboardManager::has_only_text_targets(Selection sel,
                                        const SelectionRead &expected) {
  return has_only_text_targets(sel, expected,
                               std::chrono::steady_clock::now() + timeout_);
}

std::optional<bool> ClipboardManager::has_only_text_targets(
    Selection sel, const SelectionRead &expected,
    std::chrono::steady_clock::time_point deadline) {
  if (!is_open() || !current_connection(expected)) {
    return std::nullopt;
  }
  if (expected.owner == XCB_WINDOW_NONE) {
    const auto current = get_selection_identity(sel, deadline);
    return current && current->owner == XCB_WINDOW_NONE &&
           current->owner_generation == expected.owner_generation &&
           current->connection_generation == expected.connection_generation &&
           expected.text.empty();
  }
  if (expected.selection_timestamp == XCB_CURRENT_TIME) {
    return std::nullopt;
  }

  if (!selection_identity_matches(sel, expected, deadline)) {
    return false;
  }

  const auto data =
      read_target(sel, atom_targets_, expected, deadline, nullptr);
  if (!data || data->type != XCB_ATOM_ATOM || data->format != 32 ||
      data->bytes.empty() || data->bytes.size() > 256U * sizeof(xcb_atom_t))
    return std::nullopt;
  std::vector<xcb_atom_t> storage(data->bytes.size() / sizeof(xcb_atom_t));
  std::memcpy(storage.data(), data->bytes.data(), data->bytes.size());
  const auto *targets = storage.data();
  const auto target_count = storage.size();
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

  if (!selection_identity_matches(sel, expected, deadline)) {
    return false;
  }
  return text_only;
}

bool ClipboardManager::publish_selection(
    Selection sel, std::optional<std::string_view> text,
    xcb_timestamp_t timestamp, std::chrono::steady_clock::time_point deadline,
    const SelectionRead *expected) {
  return publish_payload(sel, text ? text_payload(*text) : SelectionPayload{},
                         timestamp, deadline, expected);
}

bool ClipboardManager::publish_payload(
    Selection sel, SelectionPayload payload, xcb_timestamp_t timestamp,
    std::chrono::steady_clock::time_point deadline,
    const SelectionRead *expected, PayloadPublication *publication) {
  if (!is_open() || std::chrono::steady_clock::now() >= deadline ||
      (payload &&
       (payload->size() + 2U > kRawTargets ||
        payload_bytes(payload) > kRawPayloadBytes ||
        !std::all_of(payload->begin(), payload->end(), valid_data))) ||
      (expected && !current_connection(*expected))) {
    return false;
  }

  const bool owns_event_budget = expected && !event_budget_;
  if (owns_event_budget)
    event_budget_ = kPumpEventBudget;
  const auto reset_budget = [&](std::optional<std::size_t> *) {
    event_budget_.reset();
  };
  const std::unique_ptr<std::optional<std::size_t>, decltype(reset_budget)>
      budget_scope{owns_event_budget ? &event_budget_ : nullptr, reset_budget};

  const xcb_atom_t selection = selection_atom(sel);
  SelectionState &state = selection_state(sel);
  if (expected && timestamp == XCB_CURRENT_TIME) {
    const auto prepared = server_timestamp(deadline, XCB_CURRENT_TIME,
                                           RequestScan::FinishQueuedHistory);
    if (!prepared) {
      return false;
    }
    timestamp = *prepared;
  }

  const auto original_connection = connection_;
  const auto original_generation = connection_generation_;
  bool grabbed = false;
  const auto release_guard = [&] {
    if (!grabbed) {
      return true;
    }
    grabbed = false;
    if (!is_open() || connection_ != original_connection ||
        connection_generation_ != original_generation) {
      return false;
    }
    if (!complete_checked_request(xcb_ungrab_server_checked(connection_),
                                  deadline)) {
      fail_closed();
      return false;
    }
    return true;
  };
  const auto unlock = [&](xcb_connection_t *) { (void)release_guard(); };
  const std::unique_ptr<xcb_connection_t, decltype(unlock)> release_on_exit{
      original_connection, unlock};
  if (expected) {
    for (;;) {
      // A peer's identity fence may defer another old-generation read. Finish
      // those admitted requests before reacquiring the publication guard.
      while (!deferred_peer_events_.empty() && !handling_selection_request_ &&
             connection_healthy() && event_budget_available() &&
             std::chrono::steady_clock::now() < deadline) {
        service_deferred_peer_event(deadline);
      }
      if (!deferred_peer_events_.empty() || !connection_healthy() ||
          !current_connection(*expected) || !event_budget_available() ||
          std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      grabbed = true;
      if (!complete_checked_request(xcb_grab_server_checked(connection_),
                                    deadline)) {
        fail_closed();
        return false;
      }
      const auto cookie = xcb_get_selection_owner(connection_, selection);
      void *raw_owner = nullptr;
      xcb_generic_error_t *raw_error = nullptr;
      if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error)) {
        fail_closed();
        return false;
      }
      auto owner =
          xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
      auto error = xcb_ptr(raw_error);
      if (error || !owner)
        return false;
      xcb_generic_event_t *raw_request = nullptr;
      const bool history_complete = drain_queued_events_to_fence(
          deadline, RequestScan::Yield, &raw_request);
      auto request = xcb_ptr(raw_request);
      if (!history_complete) {
        if (!release_guard() || !request || !connection_healthy() ||
            process_event(*request, deadline,
                          RequestScan::FinishQueuedHistory)) {
          return false;
        }
        // Only peer service is repeatable: the next iteration obtains a new
        // guarded owner/history fence under the same deadline and total budget.
        continue;
      }
      if (!current_connection(*expected) || owner->owner != expected->owner ||
          state.observed_owner != expected->owner ||
          (expected->owner != XCB_WINDOW_NONE &&
           state.observed_selection_timestamp !=
               expected->selection_timestamp) ||
          state.owner_generation != expected->owner_generation ||
          !connection_healthy() ||
          std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      if (!payload && expected->owner == XCB_WINDOW_NONE) {
        return release_guard();
      }
      break;
    }
  }

  const std::uint64_t previous_owner_generation = state.owner_generation;
  const xcb_window_t published_owner =
      payload ? window_ : xcb_window_t{XCB_WINDOW_NONE};
  if (publication_proof_ && publication_proof_->selection == sel) {
    publication_proof_->publisher = window_;
    publication_proof_->timestamp = timestamp;
  }
  const auto owner_cookie = xcb_set_selection_owner_checked(
      connection_, published_owner, selection, timestamp);
  if (publication)
    publication->submitted = true;
  const auto query_cookie = xcb_get_selection_owner(connection_, selection);
  void *raw_owner = nullptr;
  xcb_generic_error_t *raw_error = nullptr;
  if (!wait_for_reply(query_cookie.sequence, deadline, &raw_owner,
                      &raw_error)) {
    state.owns = false;
    state.payload.reset();
    fail_closed();
    return false;
  }
  auto owner =
      xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_owner));
  auto query_error = xcb_ptr(raw_error);
  if (!complete_checked_request(owner_cookie, deadline) || query_error ||
      !owner || owner->owner != published_owner || !connection_healthy()) {
    state.owns = false;
    state.payload.reset();
    if (!connection_healthy()) {
      fail_closed();
    }
    return false;
  }

  // Commit the new value before pumping requests for its published identity.
  state.payload = std::move(payload);
  state.owns = static_cast<bool>(state.payload);
  // The parameter moved into state; later checks use the publication owner.
  // XCB preserves the full server sequence even when the wire serial wraps.
  // Queued history before this acquisition cannot revoke its new payload.
  state.publication_sequence = owner_cookie.sequence;
  if (state.armed_transition_id != 0) {
    cancel_owner_transition(
        OwnerTransitionToken{sel, state.armed_transition_id});
  }
  state.generation = next_generation(state.generation);
  state.armed_receipt_id = 0;
  state.armed_generation = 0;
  state.expected_requestor_client = 0;
  state.receipt_context_owner = XCB_WINDOW_NONE;
  state.receipt_context_timestamp = XCB_CURRENT_TIME;
  state.receipt_context_owner_generation = 0;
  state.receipt_context_connection_generation = 0;
  state.confirmed_receipt_id = 0;
  state.receipt_request_sequence = 0;
  state.receipt_deadline = std::chrono::steady_clock::time_point::max();

  if (!release_guard()) {
    return false;
  }
  while (connection_healthy() &&
         (state.owner_generation == previous_owner_generation ||
          state.observed_owner != published_owner ||
          state.observed_selection_timestamp == XCB_CURRENT_TIME) &&
         std::chrono::steady_clock::now() < deadline) {
    (void)pump_events_until(deadline);
    if (!connection_healthy() || std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    if (state.owner_generation != previous_owner_generation &&
        state.observed_owner == published_owner &&
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
  if (std::chrono::steady_clock::now() >= deadline || !connection_healthy() ||
      (published_owner != XCB_WINDOW_NONE && !state.owns) ||
      state.owner_generation == previous_owner_generation ||
      state.observed_owner != published_owner ||
      state.observed_selection_timestamp == XCB_CURRENT_TIME) {
    state.owns = false;
    state.payload.reset();
    fail_closed();
    return false;
  }

#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  if (sel == Selection::Clipboard) {
    std::cerr << "[punto-e2e] clipboard_publish generation=" << state.generation
              << " tick_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count()
              << " bytes=" << payload_bytes(state.payload)
              << " timestamp=" << state.observed_selection_timestamp << '\n';
  }
#endif
  if (publication) {
    publication->completed = true;
    publication->generation = state.generation;
  }
  return true;
}

bool ClipboardManager::prepare_publication_witness(
    Selection sel, const SelectionRead &expected,
    std::chrono::steady_clock::time_point deadline, bool reuse) {
  if (publication_proof_ && !reuse)
    return false;
  std::uint64_t session_generation = 0;
  if (session_) {
    auto lease = session_->acquire_write_lease();
    if (!lease || !lease->valid())
      return false;
    session_generation = lease->generation();
    if (connection_session_generation_ != session_generation)
      return false;
    if (publication_proof_ &&
        publication_proof_->session_generation != session_generation)
      return false;
    if (!publication_witness_) {
      auto observer = std::make_unique<ClipboardManager>(*session_, timeout_);
      if (!observer->initialize_connection(
              lease->open_bounded_connection(deadline), deadline, false))
        return false;
      observer->connection_session_generation_ = session_generation;
      auto &baseline = observer->selection_state(sel);
      baseline.observed_owner = expected.owner;
      baseline.observed_selection_timestamp = expected.selection_timestamp;
      baseline.baseline_initialized = true;
      publication_witness_ = std::move(observer);
    }
  }
  if (!publication_witness_ || !publication_witness_->is_open())
    return false;
  const auto current =
      publication_witness_->get_selection_identity(sel, deadline);
  if (!current || current->owner != expected.owner ||
      (current->owner != XCB_WINDOW_NONE &&
       current->selection_timestamp != expected.selection_timestamp))
    return false;
  const auto id =
      publication_proof_
          ? publication_proof_->id
          : (next_publication_id_ = next_generation(next_publication_id_));
  const auto expiry = publication_proof_
                          ? publication_proof_->deadline
                          : std::chrono::steady_clock::now() + kTransferBudget;
  if (std::chrono::steady_clock::now() >= expiry)
    return false;
  publication_proof_ = PublicationProof{id,
                                        sel,
                                        window_,
                                        XCB_CURRENT_TIME,
                                        current->owner_generation,
                                        current->connection_generation,
                                        session_generation,
                                        expiry};
  return true;
}

PayloadPublication ClipboardManager::set_payload_if_owner(
    Selection sel, const SelectionRead &expected, SelectionPayload payload,
    std::chrono::steady_clock::time_point deadline) {
  PayloadPublication publication;
  if (!is_open() || std::chrono::steady_clock::now() >= deadline ||
      !current_connection(expected) || !payload ||
      payload->size() > kRawTargets ||
      payload_bytes(payload) > kRawPayloadBytes)
    return publication;
  std::vector<xcb_atom_t> seen;
  for (const auto &data : *payload) {
    if (!valid_data(data) || data.type == atom_incr_ ||
        data.type == XCB_ATOM_WINDOW || data.type == XCB_ATOM_PIXMAP ||
        data.type == XCB_ATOM_DRAWABLE || data.type == XCB_ATOM_COLORMAP ||
        data.type == XCB_ATOM_CURSOR || data.type == XCB_ATOM_FONT ||
        data.type == XCB_ATOM_ATOM || data.target == atom_targets_ ||
        data.target == atom_timestamp_ ||
        std::find(seen.begin(), seen.end(), data.target) != seen.end() ||
        !portable_target(data.target, deadline))
      return publication;
    seen.push_back(data.target);
  }
  if (!prepare_publication_witness(sel, expected, deadline) ||
      !publication_proof_)
    return publication;
  publication.witness_id = publication_proof_->id;
  (void)publish_payload(
      sel, std::move(payload), expected.selection_timestamp,
      std::min(deadline, std::chrono::steady_clock::now() + timeout_),
      &expected, &publication);
  if (!publication.submitted)
    cancel_publication_witness(publication);
  return publication;
}

void ClipboardManager::cancel_publication_witness(
    const PayloadPublication &publication) noexcept {
  if (publication_proof_ && publication_proof_->id == publication.witness_id) {
    publication_proof_.reset();
    publication_witness_.reset();
  }
}

void ClipboardManager::adopt_publication_witness() noexcept {
  const auto clipboard_generation = next_generation(clipboard_.generation);
  const auto primary_generation = next_generation(primary_.generation);
  const auto clipboard_request_sequence = clipboard_.request_sequence;
  const auto primary_request_sequence = primary_.request_sequence;
  auto observer = std::move(publication_witness_);
  publication_witness_ = std::move(observer->publication_witness_);
  connection_owner_ = std::move(observer->connection_owner_);
  connection_generation_ = next_generation(connection_generation_);
  connection_session_generation_ = observer->connection_session_generation_;
  std::tie(connection_, screen_, window_, xfixes_event_base_, atom_clipboard_,
           atom_primary_, atom_utf8_string_, atom_targets_, atom_text_plain_,
           atom_text_plain_utf8_, atom_compound_text_, atom_text_,
           atom_timestamp_, atom_multiple_, atom_save_targets_, atom_incr_,
           atom_wm_class_, atom_net_active_window_, atom_read_property_,
           atom_server_time_) =
      std::tie(observer->connection_, observer->screen_, observer->window_,
               observer->xfixes_event_base_, observer->atom_clipboard_,
               observer->atom_primary_, observer->atom_utf8_string_,
               observer->atom_targets_, observer->atom_text_plain_,
               observer->atom_text_plain_utf8_, observer->atom_compound_text_,
               observer->atom_text_, observer->atom_timestamp_,
               observer->atom_multiple_, observer->atom_save_targets_,
               observer->atom_incr_, observer->atom_wm_class_,
               observer->atom_net_active_window_, observer->atom_read_property_,
               observer->atom_server_time_);
  clipboard_ = std::move(observer->clipboard_);
  primary_ = std::move(observer->primary_);
  // Owner history belongs to the observer connection; local publication guards
  // and request counters must never repeat across publisher adoption.
  clipboard_.generation = clipboard_generation;
  primary_.generation = primary_generation;
  clipboard_.request_sequence = clipboard_request_sequence;
  primary_.request_sequence = primary_request_sequence;
  observer->connection_ = nullptr;
  observer->screen_ = nullptr;
  observer->window_ = XCB_WINDOW_NONE;
}

bool ClipboardManager::restore_payload_after_uncertain(
    Selection sel, const PayloadPublication &publication,
    const SelectionSnapshot &previous,
    std::chrono::steady_clock::time_point deadline) {
  if (is_open() || !publication.submitted || !publication_proof_ ||
      publication.witness_id != publication_proof_->id ||
      publication_proof_->selection != sel || !publication_witness_)
    return false;
  const auto proof = *publication_proof_;
  deadline = std::min(deadline, proof.deadline);
  if (session_) {
    auto lease = session_->acquire_write_lease();
    if (!lease || !lease->valid() ||
        lease->generation() != proof.session_generation)
      return false;
  }
  auto &observer = *publication_witness_;
  const auto current = observer.get_selection_identity(sel, deadline);
  const auto &history = observer.selection_state(sel);
  if (!current ||
      current->connection_generation != proof.witness_connection_generation ||
      current->owner != XCB_WINDOW_NONE ||
      history.previous_observed_owner != proof.publisher ||
      history.observed_selection_timestamp != proof.timestamp ||
      history.owner_generation !=
          next_generation(next_generation(proof.witness_generation)) ||
      (history.observed_subtype !=
           XCB_XFIXES_SELECTION_EVENT_SELECTION_WINDOW_DESTROY &&
       history.observed_subtype !=
           XCB_XFIXES_SELECTION_EVENT_SELECTION_CLIENT_CLOSE))
    return false;
  // This very observer connection becomes the publisher, retaining its history.
  const auto released_generation = history.owner_generation;
  if (!observer.initialize_selection_baseline(Selection::Primary, deadline))
    return false;
  const auto fenced = observer.get_selection_identity(sel, deadline);
  if (!fenced || fenced->owner != XCB_WINDOW_NONE ||
      fenced->owner_generation != released_generation ||
      fenced->connection_generation != proof.witness_connection_generation)
    return false;
  adopt_publication_witness();
  const auto released = get_selection_identity(sel, deadline);
  if (!released || released->owner != XCB_WINDOW_NONE ||
      released->owner_generation != released_generation ||
      !prepare_publication_witness(sel, *released, deadline, true)) {
    // No restoration Set was submitted. Do not relabel our own adoption as
    // foreign ownership, or carry the original release proof into a new epoch.
    fail_closed();
    cancel_publication_witness(publication);
    return false;
  }
  PayloadPublication submission;
  const bool restored = publish_payload(sel, previous.payload, XCB_CURRENT_TIME,
                                        deadline, &*released, &submission);
  if (restored)
    cancel_publication_witness(publication);
  else if (!submission.submitted) {
    fail_closed();
    cancel_publication_witness(publication);
  }
  return restored;
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
  return publish_selection(sel, text, XCB_CURRENT_TIME,
                           std::chrono::steady_clock::now() + timeout_)
             ? ClipboardResult::Ok
             : ClipboardResult::ConversionFailed;
}

bool ClipboardManager::restore_text_if_generation(
    Selection sel, std::uint64_t expected_generation, std::string_view text) {
  if (text.size() > maximum_payload_bytes())
    return false;
  return restore_payload(sel, expected_generation, text_payload(text));
}

bool ClipboardManager::restore_payload_if_generation(
    Selection sel, std::uint64_t generation,
    const SelectionSnapshot &previous) {
  return restore_payload(sel, generation, previous.payload);
}

bool ClipboardManager::restore_payload(Selection sel,
                                       std::uint64_t expected_generation,
                                       SelectionPayload payload) {
  if (!is_open()) {
    return false;
  }
  SelectionState &state = selection_state(sel);
  if (!state.owns || state.generation != expected_generation) {
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  const auto timestamp =
      server_timestamp(deadline, state.observed_selection_timestamp,
                       RequestScan::FinishQueuedHistory);
  if (!timestamp ||
      !drain_queued_events_to_fence(deadline,
                                    RequestScan::FinishQueuedHistory) ||
      !state.owns || state.generation != expected_generation) {
    return false;
  }
  if (payload_bytes(payload) > kRawPayloadBytes)
    return false;
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  if (sel == Selection::Clipboard) {
    std::cerr << "[punto-e2e] clipboard_restore generation=" << state.generation
              << " tick_us="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count()
              << " bytes=" << payload_bytes(payload) << '\n';
  }
#endif
  const SelectionRead current{"", state.observed_owner,
                              state.observed_selection_timestamp,
                              state.owner_generation, connection_generation_};
  const auto original_proof = publication_proof_;
  if (publication_proof_) {
    const auto id = publication_proof_->id;
    if (std::chrono::steady_clock::now() >= publication_proof_->deadline) {
      // The live generation above admits a NEW restore intent. The expired
      // proof never admits closed-connection recovery or a paste retry.
      publication_proof_.reset();
    }
    if (!prepare_publication_witness(sel, current, deadline, true) ||
        !publication_proof_) {
      publication_proof_ = original_proof;
      return false;
    }
    publication_proof_->id = id;
  }
  PayloadPublication submission;
  const bool restored = publish_payload(sel, std::move(payload), *timestamp,
                                        deadline, &current, &submission);
  if (!submission.submitted)
    publication_proof_ = original_proof;
  if (restored && publication_proof_) {
    cancel_publication_witness(
        PayloadPublication{false, false, 0, publication_proof_->id});
  }
  return restored;
}

bool ClipboardManager::set_text_if_owner(Selection sel,
                                         const SelectionRead &expected,
                                         std::string_view text) {
  return set_text_if_owner(sel, expected, text,
                           std::chrono::steady_clock::now() + timeout_);
}

bool ClipboardManager::set_text_if_owner(
    Selection sel, const SelectionRead &expected, std::string_view text,
    std::chrono::steady_clock::time_point deadline) {
  if (expected.owner != XCB_WINDOW_NONE &&
      expected.selection_timestamp == XCB_CURRENT_TIME) {
    return false;
  }
  return publish_selection(
      sel, text, expected.selection_timestamp,
      std::min(deadline, std::chrono::steady_clock::now() + timeout_),
      &expected);
}

bool ClipboardManager::restore_selection_if_owner_from_client(
    Selection sel, const SelectionRead &expected_current,
    const SelectionRead &previous, std::uint32_t expected_client) {
  if (!is_open() || expected_client == 0 ||
      expected_current.owner == XCB_WINDOW_NONE ||
      expected_current.selection_timestamp == XCB_CURRENT_TIME ||
      !current_connection(expected_current) || !current_connection(previous) ||
      resource_client_id(expected_current.owner) != expected_client ||
      previous.text.size() > maximum_payload_bytes()) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  return publish_selection(sel,
                           previous.owner == XCB_WINDOW_NONE
                               ? std::nullopt
                               : std::optional<std::string_view>{previous.text},
                           expected_current.selection_timestamp, deadline,
                           &expected_current);
}

bool ClipboardManager::restore_selection_after_client_release(
    Selection sel, const SelectionRead &released, const SelectionRead &previous,
    std::uint32_t expected_client) {
  if (!is_open() || expected_client == 0 || released.owner == XCB_WINDOW_NONE ||
      released.selection_timestamp == XCB_CURRENT_TIME ||
      !current_connection(released) || !current_connection(previous) ||
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

  const SelectionRead current{"", state.observed_owner,
                              state.observed_selection_timestamp,
                              state.owner_generation, connection_generation_};
  return publish_selection(sel,
                           previous.owner == XCB_WINDOW_NONE
                               ? std::nullopt
                               : std::optional<std::string_view>{previous.text},
                           current.selection_timestamp,
                           std::chrono::steady_clock::now() + timeout_,
                           &current);
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
  if (!wait_for_reply(cookie.sequence, deadline, &raw_owner, &raw_error,
                      ReplyIntent::Observation)) {
    if (!connection_healthy())
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
                        &raw_class_error, ReplyIntent::Observation)) {
      if (!connection_healthy())
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
        bool valid_class = false;
        if (first_end != std::string_view::npos &&
            first_end + 1 < value.size()) {
          const std::size_t second_end = value.find('\0', first_end + 1);
          klass = value.substr(first_end + 1, second_end - first_end - 1);
          valid_class = second_end != std::string_view::npos &&
                        second_end + 1 == value.size();
        }
        if (valid_class && is_terminal_wm_class(instance, klass)) {
          return ActiveWindowKind::Terminal;
        }
        if (valid_class && is_safe_gui_wm_class(instance, klass)) {
          return ActiveWindowKind::Gui;
        }
      }
    }

    const auto tree_cookie = xcb_query_tree(connection_, window);
    void *raw_tree = nullptr;
    xcb_generic_error_t *raw_tree_error = nullptr;
    if (!wait_for_reply(tree_cookie.sequence, deadline, &raw_tree,
                        &raw_tree_error, ReplyIntent::Observation)) {
      if (!connection_healthy())
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
                      &raw_active_error, ReplyIntent::Observation)) {
    if (!connection_healthy())
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
