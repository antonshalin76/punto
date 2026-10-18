#include "punto/clipboard_manager.hpp"
#include "punto/x11_session.hpp"

#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/xfixes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace initialization_fault {
enum class Stage { None, Atom, Extension };
Stage stage = Stage::None;
xcb_connection_t *connection = nullptr;
pid_t server = -1;
std::atomic<bool> stopped{false};
bool disconnected = false;
bool used_after_disconnect = false;

void suspend(xcb_connection_t *candidate, Stage candidate_stage) {
  if (stage != Stage::None && stage == candidate_stage &&
      connection == nullptr) {
    connection = candidate;
  }
  if (candidate == connection && stage == candidate_stage && !stopped) {
    stopped = ::kill(server, SIGSTOP) == 0;
  }
}
} // namespace initialization_fault

namespace observational_timeout_fault {
xcb_connection_t *connection = nullptr;
xcb_atom_t selection = XCB_ATOM_NONE;
bool armed = false;
unsigned int consumed = 0;
} // namespace observational_timeout_fault

namespace identity_fault {
xcb_connection_t *connection = nullptr;
bool inject_error = false;
bool error_delivered = false;
bool disconnected = false;
bool owner_query_after_close = false;
} // namespace identity_fault

namespace property_fault {
std::atomic<unsigned int> checked_deletes{0};
} // namespace property_fault

namespace peer_response_fault {
enum class Corruption { None, Resource, Opcode, Sequence, Code };
xcb_connection_t *connection = nullptr;
xcb_window_t requestor = XCB_WINDOW_NONE;
unsigned int actual_bad_windows = 0;
Corruption corruption = Corruption::None;
bool delay_next_barrier = false;
unsigned int barriers_before_delay = 0;
unsigned int actual_delayed_barriers = 0;
unsigned int actual_completed_checks = 0;
} // namespace peer_response_fault

namespace read_deadline_fault {
xcb_connection_t *connection = nullptr;
xcb_atom_t utf8 = XCB_ATOM_NONE;
bool armed = false;
bool barrier_pending = false;
unsigned int consumed = 0;
} // namespace read_deadline_fault

namespace publication_fault {
enum class Stage {
  None,
  TimestampTimeout,
  ForeignTakeover,
  BoundaryRead,
  ReadReplyBoundary
};
Stage stage = Stage::None;
xcb_connection_t *connection = nullptr;
xcb_window_t window = XCB_WINDOW_NONE;
std::uint8_t selection_event = 0;
xcb_connection_t *peer = nullptr;
xcb_window_t peer_window = XCB_WINDOW_NONE;
xcb_atom_t clipboard = XCB_ATOM_NONE;
xcb_atom_t utf8 = XCB_ATOM_NONE;
xcb_atom_t property = XCB_ATOM_NONE;
xcb_atom_t timestamp_property = XCB_ATOM_NONE;
xcb_timestamp_t previous_timestamp = XCB_CURRENT_TIME;
bool consumed = false;
} // namespace publication_fault

namespace publication_guard_fault {
enum class Stage {
  None,
  PeerCopy,
  ReleaseCopy,
  ReleaseABA,
  GrabStop,
  SetStop,
  ReleaseStop
};
Stage stage = Stage::None;
xcb_connection_t *connection = nullptr;
xcb_connection_t *peer = nullptr;
xcb_window_t peer_window = XCB_WINDOW_NONE;
xcb_window_t original_owner = XCB_WINDOW_NONE;
xcb_atom_t selection = XCB_ATOM_NONE;
xcb_timestamp_t timestamp = XCB_CURRENT_TIME;
xcb_void_cookie_t peer_copy{};
xcb_atom_t payload_property = XCB_ATOM_NONE;
xcb_atom_t second_payload_property = XCB_ATOM_NONE;
pid_t server = -1;
bool held = false;
bool consumed = false;
bool peer_queued_while_held = false;
bool count_all_writes = false;
unsigned int grabs = 0;
unsigned int releases = 0;
unsigned int writes = 0;
unsigned int writes_while_held = 0;
unsigned int responses = 0;
unsigned int responses_while_held = 0;

void reset(xcb_connection_t *candidate = nullptr) {
  stage = Stage::None;
  connection = candidate;
  peer = nullptr;
  peer_window = XCB_WINDOW_NONE;
  original_owner = XCB_WINDOW_NONE;
  selection = XCB_ATOM_NONE;
  timestamp = XCB_CURRENT_TIME;
  peer_copy = {};
  payload_property = XCB_ATOM_NONE;
  second_payload_property = XCB_ATOM_NONE;
  server = -1;
  held = consumed = peer_queued_while_held = count_all_writes = false;
  grabs = releases = writes = writes_while_held = 0;
  responses = responses_while_held = 0;
}

void stop_after_actual_request(xcb_connection_t *candidate, Stage boundary) {
  if (candidate != connection || stage != boundary) {
    return;
  }
  stage = Stage::None;
  auto *reply = xcb_get_input_focus_reply(
      candidate, xcb_get_input_focus(candidate), nullptr);
  if (!reply || ::kill(server, SIGSTOP) != 0) {
    std::free(reply);
    std::abort();
  }
  std::free(reply);
  consumed = true;
}
} // namespace publication_guard_fault

extern "C" {
int __real_xcb_poll_for_reply(xcb_connection_t *, unsigned int, void **,
                              xcb_generic_error_t **);
int __wrap_xcb_poll_for_reply(xcb_connection_t *connection,
                              unsigned int sequence, void **reply,
                              xcb_generic_error_t **error) {
  const int ready =
      __real_xcb_poll_for_reply(connection, sequence, reply, error);
  if (connection == peer_response_fault::connection &&
      peer_response_fault::actual_delayed_barriers != 0 && ready != 0 &&
      reply && !*reply && error && !*error)
    ++peer_response_fault::actual_completed_checks;
  if (connection == peer_response_fault::connection && error && *error &&
      (*error)->error_code == XCB_WINDOW &&
      (*error)->resource_id == peer_response_fault::requestor) {
    ++peer_response_fault::actual_bad_windows;
    switch (peer_response_fault::corruption) {
    case peer_response_fault::Corruption::None:
      break;
    case peer_response_fault::Corruption::Resource:
      (*error)->resource_id = XCB_WINDOW_NONE;
      break;
    case peer_response_fault::Corruption::Opcode:
      (*error)->major_code = XCB_GRAB_SERVER;
      break;
    case peer_response_fault::Corruption::Sequence:
      ++(*error)->full_sequence;
      break;
    case peer_response_fault::Corruption::Code:
      (*error)->error_code = XCB_VALUE;
      break;
    }
  }
  return ready;
}
xcb_void_cookie_t
__real_xcb_change_property_checked(xcb_connection_t *, std::uint8_t,
                                   xcb_window_t, xcb_atom_t, xcb_atom_t,
                                   std::uint8_t, std::uint32_t, const void *);
xcb_void_cookie_t __wrap_xcb_change_property_checked(
    xcb_connection_t *connection, std::uint8_t mode, xcb_window_t window,
    xcb_atom_t property, xcb_atom_t type, std::uint8_t format,
    std::uint32_t length, const void *data) {
  if (connection == publication_guard_fault::connection &&
      (property == publication_guard_fault::payload_property ||
       property == publication_guard_fault::second_payload_property) &&
      window == publication_guard_fault::peer_window) {
    ++publication_guard_fault::responses;
    if (publication_guard_fault::held) {
      ++publication_guard_fault::responses_while_held;
    }
  }
  return __real_xcb_change_property_checked(connection, mode, window, property,
                                            type, format, length, data);
}
xcb_void_cookie_t __real_xcb_grab_server_checked(xcb_connection_t *);
xcb_void_cookie_t __wrap_xcb_grab_server_checked(xcb_connection_t *connection) {
  const auto cookie = __real_xcb_grab_server_checked(connection);
  if (connection == publication_guard_fault::connection) {
    publication_guard_fault::held = true;
    ++publication_guard_fault::grabs;
    publication_guard_fault::stop_after_actual_request(
        connection, publication_guard_fault::Stage::GrabStop);
  }
  return cookie;
}
xcb_void_cookie_t __real_xcb_ungrab_server_checked(xcb_connection_t *);
xcb_void_cookie_t
__wrap_xcb_ungrab_server_checked(xcb_connection_t *connection) {
  const auto cookie = __real_xcb_ungrab_server_checked(connection);
  if (connection == publication_guard_fault::connection) {
    publication_guard_fault::held = false;
    ++publication_guard_fault::releases;
    const auto stage = publication_guard_fault::stage;
    if (stage == publication_guard_fault::Stage::ReleaseCopy ||
        stage == publication_guard_fault::Stage::ReleaseABA) {
      publication_guard_fault::stage = publication_guard_fault::Stage::None;
      auto *released = xcb_get_input_focus_reply(
          connection, xcb_get_input_focus(connection), nullptr);
      if (!released)
        std::abort();
      std::free(released);
      auto *error = xcb_request_check(
          publication_guard_fault::peer,
          xcb_set_selection_owner_checked(publication_guard_fault::peer,
                                          publication_guard_fault::peer_window,
                                          publication_guard_fault::selection,
                                          publication_guard_fault::timestamp));
      if (error)
        std::abort();
      if (stage == publication_guard_fault::Stage::ReleaseABA) {
        error = xcb_request_check(publication_guard_fault::peer,
                                  xcb_set_selection_owner_checked(
                                      publication_guard_fault::peer,
                                      publication_guard_fault::original_owner,
                                      publication_guard_fault::selection,
                                      publication_guard_fault::timestamp));
        if (error)
          std::abort();
      }
      publication_guard_fault::consumed = true;
    }
    publication_guard_fault::stop_after_actual_request(
        connection, publication_guard_fault::Stage::ReleaseStop);
  }
  return cookie;
}
xcb_void_cookie_t __real_xcb_set_selection_owner_checked(xcb_connection_t *,
                                                         xcb_window_t,
                                                         xcb_atom_t,
                                                         xcb_timestamp_t);
xcb_void_cookie_t
__wrap_xcb_set_selection_owner_checked(xcb_connection_t *connection,
                                       xcb_window_t owner, xcb_atom_t selection,
                                       xcb_timestamp_t timestamp) {
  const auto cookie = __real_xcb_set_selection_owner_checked(
      connection, owner, selection, timestamp);
  if (connection == publication_guard_fault::connection ||
      publication_guard_fault::count_all_writes)
    ++publication_guard_fault::writes;
  if (connection == publication_guard_fault::connection) {
    if (publication_guard_fault::held) {
      ++publication_guard_fault::writes_while_held;
    }
    publication_guard_fault::stop_after_actual_request(
        connection, publication_guard_fault::Stage::SetStop);
  }
  return cookie;
}
xcb_void_cookie_t __real_xcb_convert_selection_checked(xcb_connection_t *,
                                                       xcb_window_t, xcb_atom_t,
                                                       xcb_atom_t, xcb_atom_t,
                                                       xcb_timestamp_t);
xcb_void_cookie_t __wrap_xcb_convert_selection_checked(
    xcb_connection_t *connection, xcb_window_t requestor, xcb_atom_t selection,
    xcb_atom_t target, xcb_atom_t property, xcb_timestamp_t time) {
  const auto cookie = __real_xcb_convert_selection_checked(
      connection, requestor, selection, target, property, time);
  if (connection == read_deadline_fault::connection &&
      selection == XCB_ATOM_PRIMARY && target == read_deadline_fault::utf8 &&
      read_deadline_fault::armed) {
    read_deadline_fault::armed = false;
    read_deadline_fault::barrier_pending = true;
  }
  return cookie;
}
xcb_get_input_focus_cookie_t __real_xcb_get_input_focus(xcb_connection_t *);
xcb_get_input_focus_cookie_t
__wrap_xcb_get_input_focus(xcb_connection_t *connection) {
  const auto cookie = __real_xcb_get_input_focus(connection);
  if (connection == peer_response_fault::connection &&
      peer_response_fault::delay_next_barrier) {
    if (peer_response_fault::barriers_before_delay != 0) {
      --peer_response_fault::barriers_before_delay;
      return cookie;
    }
    peer_response_fault::delay_next_barrier = false;
    auto *actual = xcb_get_input_focus_reply(connection, cookie, nullptr);
    if (!actual)
      std::abort();
    std::free(actual);
    ++peer_response_fault::actual_delayed_barriers;
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  if (connection == read_deadline_fault::connection &&
      read_deadline_fault::barrier_pending) {
    read_deadline_fault::barrier_pending = false;
    ++read_deadline_fault::consumed;
    if (xcb_flush(connection) <= 0) {
      std::abort();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  return cookie;
}
xcb_intern_atom_cookie_t __real_xcb_intern_atom(xcb_connection_t *, uint8_t,
                                                uint16_t, const char *);
xcb_intern_atom_cookie_t __wrap_xcb_intern_atom(xcb_connection_t *connection,
                                                uint8_t only_if_exists,
                                                uint16_t length,
                                                const char *name) {
  initialization_fault::suspend(connection, initialization_fault::Stage::Atom);
  return __real_xcb_intern_atom(connection, only_if_exists, length, name);
}
void __real_xcb_prefetch_extension_data(xcb_connection_t *, xcb_extension_t *);
void __wrap_xcb_prefetch_extension_data(xcb_connection_t *connection,
                                        xcb_extension_t *extension) {
  initialization_fault::suspend(connection,
                                initialization_fault::Stage::Extension);
  __real_xcb_prefetch_extension_data(connection, extension);
}
const xcb_query_extension_reply_t *
__real_xcb_get_extension_data(xcb_connection_t *, xcb_extension_t *);
const xcb_query_extension_reply_t *
__wrap_xcb_get_extension_data(xcb_connection_t *connection,
                              xcb_extension_t *extension) {
  if (connection == initialization_fault::connection &&
      initialization_fault::disconnected) {
    initialization_fault::used_after_disconnect = true;
    return nullptr;
  }
  initialization_fault::suspend(connection,
                                initialization_fault::Stage::Extension);
  return __real_xcb_get_extension_data(connection, extension);
}
void __real_xcb_disconnect(xcb_connection_t *);
void __wrap_xcb_disconnect(xcb_connection_t *connection) {
  if (connection == publication_guard_fault::connection) {
    publication_guard_fault::held = false;
  }
  if (connection == initialization_fault::connection) {
    initialization_fault::disconnected = true;
  }
  if (connection == identity_fault::connection) {
    identity_fault::disconnected = true;
  }
  __real_xcb_disconnect(connection);
}
xcb_generic_event_t *__real_xcb_poll_for_event(xcb_connection_t *);
xcb_generic_event_t *__wrap_xcb_poll_for_event(xcb_connection_t *connection) {
  if (connection == identity_fault::connection &&
      identity_fault::inject_error) {
    identity_fault::inject_error = false;
    identity_fault::error_delivered = true;
    auto *event = static_cast<xcb_generic_event_t *>(
        std::calloc(1, sizeof(xcb_generic_error_t)));
    if (event == nullptr) {
      std::abort();
    }
    event->response_type = 0;
    return event;
  }
  auto *event = __real_xcb_poll_for_event(connection);
  if (!event || connection != publication_fault::connection) {
    return event;
  }
  const auto type = event->response_type & 0x7fU;
  const auto stage = publication_fault::stage;
  const bool timestamp =
      type == XCB_PROPERTY_NOTIFY &&
      reinterpret_cast<xcb_property_notify_event_t *>(event)->window ==
          publication_fault::window &&
      reinterpret_cast<xcb_property_notify_event_t *>(event)->atom ==
          publication_fault::timestamp_property &&
      reinterpret_cast<xcb_property_notify_event_t *>(event)->state ==
          XCB_PROPERTY_NEW_VALUE;
  const bool published =
      type == publication_fault::selection_event &&
      reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(event)->owner ==
          publication_fault::window &&
      reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(event)
              ->selection_timestamp != publication_fault::previous_timestamp &&
      reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(event)
              ->selection == publication_fault::clipboard;
  const bool read_reply =
      type == XCB_SELECTION_NOTIFY &&
      reinterpret_cast<xcb_selection_notify_event_t *>(event)->selection ==
          XCB_ATOM_PRIMARY &&
      reinterpret_cast<xcb_selection_notify_event_t *>(event)->target ==
          publication_fault::utf8 &&
      reinterpret_cast<xcb_selection_notify_event_t *>(event)->property ==
          publication_fault::timestamp_property;
  if ((timestamp && (stage == publication_fault::Stage::TimestampTimeout ||
                     stage == publication_fault::Stage::ForeignTakeover)) ||
      (published && stage == publication_fault::Stage::BoundaryRead) ||
      (read_reply && stage == publication_fault::Stage::ReadReplyBoundary)) {
    publication_fault::stage = publication_fault::Stage::None;
    publication_fault::consumed = true;
    if (stage == publication_fault::Stage::TimestampTimeout) {
      std::free(event);
      return nullptr;
    }
    const auto cookie =
        stage == publication_fault::Stage::ForeignTakeover
            ? xcb_set_selection_owner_checked(
                  publication_fault::peer, publication_fault::peer_window,
                  publication_fault::clipboard, XCB_CURRENT_TIME)
            : xcb_convert_selection_checked(
                  publication_fault::peer, publication_fault::peer_window,
                  publication_fault::clipboard, publication_fault::utf8,
                  publication_fault::property, XCB_CURRENT_TIME);
    auto *error = xcb_request_check(publication_fault::peer, cookie);
    if (error) {
      std::free(error);
      std::abort();
    }
    if (stage == publication_fault::Stage::ReadReplyBoundary) {
      auto *peer_barrier = xcb_get_input_focus_reply(
          publication_fault::peer, xcb_get_input_focus(publication_fault::peer),
          nullptr);
      auto *manager_barrier = xcb_get_input_focus_reply(
          connection, xcb_get_input_focus(connection), nullptr);
      if (!peer_barrier || !manager_barrier)
        std::abort();
      std::free(peer_barrier);
      std::free(manager_barrier);
    }
  }
  return event;
}
xcb_get_selection_owner_cookie_t
__real_xcb_get_selection_owner(xcb_connection_t *, xcb_atom_t);
xcb_get_selection_owner_cookie_t
__wrap_xcb_get_selection_owner(xcb_connection_t *connection,
                               xcb_atom_t selection) {
  if (connection == nullptr || (connection == identity_fault::connection &&
                                identity_fault::disconnected)) {
    identity_fault::owner_query_after_close = true;
    return xcb_get_selection_owner_cookie_t{0};
  }
  const auto cookie = __real_xcb_get_selection_owner(connection, selection);
  if (connection == observational_timeout_fault::connection &&
      selection == observational_timeout_fault::selection &&
      observational_timeout_fault::armed) {
    observational_timeout_fault::armed = false;
    const auto barrier = xcb_get_input_focus(connection);
    if (xcb_flush(connection) <= 0)
      std::abort();
    void *reply = nullptr;
    xcb_generic_error_t *error = nullptr;
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!__real_xcb_poll_for_reply(connection, barrier.sequence, &reply,
                                      &error) &&
           std::chrono::steady_clock::now() < until) {
      pollfd fd{xcb_get_file_descriptor(connection), POLLIN, 0};
      (void)::poll(&fd, 1, 1);
    }
    if (!reply || error)
      std::abort();
    std::free(reply);
    ++observational_timeout_fault::consumed;
    // Only this owned private connection is delayed, after the real reply.
    std::this_thread::sleep_for(std::chrono::milliseconds{40});
  }
  if (connection == publication_guard_fault::connection &&
      publication_guard_fault::stage ==
          publication_guard_fault::Stage::PeerCopy &&
      selection == publication_guard_fault::selection) {
    publication_guard_fault::stage = publication_guard_fault::Stage::None;
    auto *barrier = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), nullptr);
    if (!barrier) {
      std::abort();
    }
    std::free(barrier);
    publication_guard_fault::peer_copy = __real_xcb_set_selection_owner_checked(
        publication_guard_fault::peer, publication_guard_fault::peer_window,
        selection, publication_guard_fault::timestamp);
    if (xcb_flush(publication_guard_fault::peer) <= 0) {
      std::abort();
    }
    publication_guard_fault::peer_queued_while_held =
        publication_guard_fault::held;
    if (!publication_guard_fault::held) {
      auto *error = xcb_request_check(publication_guard_fault::peer,
                                      publication_guard_fault::peer_copy);
      if (error) {
        std::free(error);
        std::abort();
      }
    }
    publication_guard_fault::consumed = true;
  }
  return cookie;
}
xcb_void_cookie_t __real_xcb_delete_property_checked(xcb_connection_t *,
                                                     xcb_window_t, xcb_atom_t);
xcb_void_cookie_t
__wrap_xcb_delete_property_checked(xcb_connection_t *connection,
                                   xcb_window_t window, xcb_atom_t property) {
  property_fault::checked_deletes.fetch_add(1, std::memory_order_relaxed);
  return __real_xcb_delete_property_checked(connection, window, property);
}
}

namespace punto {

struct ClipboardManagerTestAccess {
  static BoundedXcbConnection adopt(xcb_connection_t *connection, int screen) {
    return BoundedXcbConnection{connection, screen};
  }

  static std::unique_ptr<ClipboardManager>
  make(xcb_connection_t *connection, int screen_number,
       std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
    return std::unique_ptr<ClipboardManager>{
        new ClipboardManager{connection, screen_number, timeout}};
  }

  static std::optional<std::string>
  decode(xcb_atom_t type, std::uint8_t format, std::uint32_t bytes_after,
         const std::uint8_t *data, std::size_t size, xcb_atom_t expected,
         xcb_atom_t incr) {
    return ClipboardManager::decode_text_property(
        ClipboardManager::PropertyBytes{type, format, bytes_after, data, size},
        expected, incr);
  }

  static std::size_t maximum_payload_bytes(const ClipboardManager &manager) {
    return manager.maximum_payload_bytes();
  }

  static std::uint32_t resource_client_id(const ClipboardManager &manager,
                                          xcb_window_t window) {
    return manager.resource_client_id(window);
  }

  static xcb_connection_t *connection(const ClipboardManager &manager) {
    return manager.connection_;
  }

  static xcb_window_t window(const ClipboardManager &manager) {
    return manager.window_;
  }

  static std::string close_state(const ClipboardManager &manager) {
    const auto fields = [](std::string_view prefix,
                           const ClipboardManager::SelectionState &state) {
      return std::string{prefix} +
             "_owner=" + std::to_string(state.observed_owner) + " " +
             std::string{prefix} + "_owns=" + std::to_string(state.owns) + " " +
             std::string{prefix} +
             "_generation=" + std::to_string(state.generation) + " " +
             std::string{prefix} +
             "_receipt=" + std::to_string(state.armed_receipt_id);
    };
    return "window=" + std::to_string(manager.window_) +
           " connection_generation=" +
           std::to_string(manager.connection_generation_) + " " +
           fields("clipboard", manager.clipboard_) + " " +
           fields("primary", manager.primary_);
  }

  static void request_timeout(ClipboardManager &manager,
                              std::chrono::milliseconds timeout) {
    manager.timeout_ = timeout;
  }

  static void attach_witness(ClipboardManager &manager,
                             std::unique_ptr<ClipboardManager> observer) {
    manager.publication_witness_ = std::move(observer);
  }

  static ClipboardManager *witness(ClipboardManager &manager) {
    return manager.publication_witness_.get();
  }

  static std::uint64_t outgoing_transfer_id(const ClipboardManager &manager,
                                            xcb_window_t window,
                                            xcb_atom_t property) {
    for (const auto &transfer : manager.outgoing_)
      if (transfer.requestor == window && transfer.property == property)
        return transfer.id;
    return 0;
  }

  static std::size_t deferred_ack_count(const ClipboardManager &manager,
                                        std::uint64_t transfer_id) {
    return static_cast<std::size_t>(std::count_if(
        manager.deferred_peer_events_.begin(),
        manager.deferred_peer_events_.end(), [&](const auto &event) {
          return event.transfer_id == transfer_id &&
                 (event.event.response_type & 0x7fU) == XCB_PROPERTY_NOTIFY;
        }));
  }

  static std::uint8_t selection_event(const ClipboardManager &manager) {
    return manager.xfixes_event_base_ + XCB_XFIXES_SELECTION_NOTIFY;
  }

  static bool event_precedes_publication(std::uint32_t publication,
                                         std::uint32_t event) {
    ClipboardManager::SelectionState state;
    state.owns = true;
    state.publication_sequence = publication;
    return ClipboardManager::event_precedes_publication(state, event);
  }

  static bool reconnect(ClipboardManager &manager, xcb_connection_t *connection,
                        int screen_number) {
    manager.close();
    return manager.initialize_connection(
        BoundedXcbConnection{connection, screen_number},
        std::chrono::steady_clock::now() + std::chrono::seconds{2});
  }
};

} // namespace punto

namespace {

template <typename T> using XcbPtr = std::unique_ptr<T, decltype(&std::free)>;

template <typename T> XcbPtr<T> xcb_ptr(T *pointer) {
  return XcbPtr<T>{pointer, &std::free};
}

void expect(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

class StderrCapture {
public:
  StderrCapture() : file_{std::tmpfile(), &std::fclose} {
    expect(file_ != nullptr, "create owned diagnostic capture file");
    std::fflush(stderr);
    saved_ = ::dup(STDERR_FILENO);
    expect(saved_ >= 0, "save diagnostic stderr descriptor");
    if (::dup2(::fileno(file_.get()), STDERR_FILENO) < 0) {
      ::close(saved_);
      saved_ = -1;
      throw std::runtime_error{"capture diagnostic stderr descriptor"};
    }
  }
  ~StderrCapture() { restore(); }
  StderrCapture(const StderrCapture &) = delete;
  StderrCapture &operator=(const StderrCapture &) = delete;

  std::string finish() {
    restore();
    expect(std::fseek(file_.get(), 0, SEEK_SET) == 0,
           "rewind diagnostic capture");
    std::array<char, 8192> bytes{};
    const auto size = std::fread(bytes.data(), 1, bytes.size(), file_.get());
    expect(size < bytes.size() && !std::ferror(file_.get()),
           "diagnostic capture remains bounded");
    return {bytes.data(), size};
  }

private:
  void restore() noexcept {
    if (saved_ >= 0) {
      std::fflush(stderr);
      (void)::dup2(saved_, STDERR_FILENO);
      ::close(saved_);
      saved_ = -1;
    }
  }
  std::unique_ptr<FILE, int (*)(FILE *)> file_;
  int saved_ = -1;
};

void expect_close_diagnostic(
    const std::string &record, bool failure, const std::string &state,
    std::string_view function, std::uint_least32_t line = 0,
    std::string_view source_file = "test_clipboard_manager_contract.cpp") {
  const std::string marker = "[punto] Clipboard close ";
  const auto start = record.find(marker);
  if (start == std::string::npos ||
      record.find(marker, start + marker.size()) != std::string::npos)
    throw std::runtime_error{
        "one pre-close diagnostic identifies each live connection: " + record};
  const auto end = record.find('\n', start);
  expect(end != std::string::npos && end - start < 1024,
         "close diagnostic is one bounded line");
  const auto event = std::string_view{record}.substr(start, end - start);
  expect(event.find(failure ? "failure=1 " : "failure=0 ") !=
                 std::string_view::npos &&
             event.find(function) != std::string_view::npos &&
             event.find(state) != std::string_view::npos &&
             event.find("/home/") == std::string_view::npos &&
             event.find("baseline") == std::string_view::npos &&
             event.find("payload") == std::string_view::npos,
         "diagnostic retains pre-close scalar tuple without data or paths");
  const auto site =
      std::string{"site="} +
      std::string{failure ? "clipboard_manager.cpp" : source_file} + ":";
  expect(event.find(site) != std::string_view::npos,
         "diagnostic attributes the triggering source file");
  if (line != 0) {
    expect(event.find(std::string{site} + std::to_string(line) + " ") !=
               std::string_view::npos,
           "normal close retains its exact external caller line");
  }
}

std::vector<std::string>
witness_close_states(punto::ClipboardManager &manager) {
  using Access = punto::ClipboardManagerTestAccess;
  std::vector<std::string> states;
  for (auto *witness = Access::witness(manager); witness;
       witness = Access::witness(*witness)) {
    expect(states.size() < 8, "publication witness chain remains bounded");
    if (Access::connection(*witness))
      states.push_back(Access::close_state(*witness));
  }
  return states;
}

void expect_witness_close_diagnostics(const std::string &record,
                                      const std::vector<std::string> &states) {
  std::size_t cursor = 0;
  for (const auto &state : states) {
    const auto end = record.find('\n', cursor);
    expect(end != std::string::npos,
           "each live witness emits its own close diagnostic");
    expect_close_diagnostic(record.substr(cursor, end - cursor + 1), false,
                            state, "~ClipboardManager", 0,
                            "clipboard_manager.cpp");
    cursor = end + 1;
  }
  expect(cursor == record.size(),
         "cleanup emits no duplicate or unknown connection diagnostic");
}

xcb_atom_t intern_atom(xcb_connection_t *connection, std::string_view name) {
  const auto cookie = xcb_intern_atom(
      connection, 0, static_cast<std::uint16_t>(name.size()), name.data());
  xcb_generic_error_t *raw_error = nullptr;
  auto reply = xcb_ptr(xcb_intern_atom_reply(connection, cookie, &raw_error));
  auto error = xcb_ptr(raw_error);
  expect(!error && reply, "intern test atom");
  return reply->atom;
}

struct Connection {
  xcb_connection_t *value = nullptr;
  int screen = -1;
};

Connection connect_to(const char *display = nullptr) {
  int last_error = -1;
  for (int attempt = 0; attempt < 5; ++attempt) {
    Connection result;
    result.value = xcb_connect(display, &result.screen);
    last_error =
        result.value == nullptr ? -1 : xcb_connection_has_error(result.value);
    if (result.value != nullptr && last_error == 0) {
      const xcb_query_extension_reply_t *xfixes =
          xcb_get_extension_data(result.value, &xcb_xfixes_id);
      if (xfixes == nullptr || xfixes->present == 0) {
        xcb_disconnect(result.value);
        throw std::runtime_error{"test X server has no XFixes extension"};
      }
      return result;
    }
    if (result.value != nullptr) {
      xcb_disconnect(result.value);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  throw std::runtime_error{
      "connect to test X server: error=" + std::to_string(last_error) +
      " display=" + (display == nullptr ? "env" : display)};
}

xcb_screen_t *screen_at(xcb_connection_t *connection, int index) {
  xcb_screen_iterator_t it =
      xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int current = 0; current < index && it.rem > 0; ++current) {
    xcb_screen_next(&it);
  }
  expect(it.rem > 0 && it.data != nullptr, "resolve test screen");
  return it.data;
}

xcb_window_t create_window(xcb_connection_t *connection, int screen_number) {
  xcb_screen_t *screen = screen_at(connection, screen_number);
  const xcb_window_t window = xcb_generate_id(connection);
  const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  const auto cookie = xcb_create_window_checked(
      connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1, 1, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_EVENT_MASK,
      &mask);
  auto error = xcb_ptr(xcb_request_check(connection, cookie));
  expect(!error, "create test window");
  return window;
}

void set_active_window(xcb_connection_t *connection, int screen_number,
                       xcb_window_t window) {
  const xcb_atom_t active = intern_atom(connection, "_NET_ACTIVE_WINDOW");
  const auto cookie =
      xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE,
                                  screen_at(connection, screen_number)->root,
                                  active, XCB_ATOM_WINDOW, 32, 1, &window);
  auto error = xcb_ptr(xcb_request_check(connection, cookie));
  expect(!error && xcb_flush(connection) > 0, "publish active test window");
}

xcb_selection_notify_event_t
await_selection_notify(xcb_connection_t *requester,
                       punto::ClipboardManager &manager) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
    (void)manager.pump_events();
    auto event = xcb_ptr(xcb_poll_for_event(requester));
    if (event) {
      const std::uint8_t type =
          static_cast<std::uint8_t>(event->response_type & 0x7fU);
      if (type == XCB_SELECTION_NOTIFY) {
        return *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  throw std::runtime_error{"selection notify timeout"};
}

xcb_selection_notify_event_t
request_selection(xcb_connection_t *requester, xcb_window_t window,
                  punto::ClipboardManager &manager, xcb_atom_t selection,
                  xcb_atom_t target, xcb_atom_t property) {
  const auto cookie = xcb_convert_selection_checked(
      requester, window, selection, target, property, XCB_CURRENT_TIME);
  auto error = xcb_ptr(xcb_request_check(requester, cookie));
  expect(!error && xcb_flush(requester) > 0, "queue selection conversion");
  return await_selection_notify(requester, manager);
}

void serve_one_text_request(
    xcb_connection_t *owner, std::string_view text,
    punto::ClipboardResult result = punto::ClipboardResult::Ok,
    xcb_window_t *requestor = nullptr) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(owner));
    if (event) {
      const std::uint8_t type =
          static_cast<std::uint8_t>(event->response_type & 0x7fU);
      if (type != XCB_SELECTION_REQUEST) {
        continue;
      }
      const auto &request =
          *reinterpret_cast<const xcb_selection_request_event_t *>(event.get());
      if (requestor != nullptr) {
        *requestor = request.requestor;
      }
      const xcb_atom_t property =
          request.property == XCB_ATOM_NONE ? request.target : request.property;
      if (result != punto::ClipboardResult::NoSelection) {
        xcb_change_property(
            owner, XCB_PROP_MODE_REPLACE, request.requestor, property,
            result == punto::ClipboardResult::ConversionFailed
                ? static_cast<xcb_atom_t>(XCB_ATOM_INTEGER)
                : request.target,
            8, static_cast<std::uint32_t>(text.size()), text.data());
      }
      xcb_selection_notify_event_t notify{};
      notify.response_type = XCB_SELECTION_NOTIFY;
      notify.time = request.time;
      notify.requestor = request.requestor;
      notify.selection = request.selection;
      notify.target = request.target;
      notify.property = result == punto::ClipboardResult::NoSelection
                            ? static_cast<xcb_atom_t>(XCB_ATOM_NONE)
                            : property;
      xcb_send_event(owner, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                     reinterpret_cast<const char *>(&notify));
      expect(xcb_flush(owner) > 0, "serve external text request");
      return;
    }
    pollfd descriptor{xcb_get_file_descriptor(owner), POLLIN, 0};
    (void)::poll(&descriptor, 1, 5);
  }
  throw std::runtime_error{"external text request timeout"};
}

xcb_timestamp_t server_timestamp(xcb_connection_t *connection,
                                 xcb_window_t window) {
  const xcb_atom_t property =
      intern_atom(connection, "PUNTO_TEST_SERVER_TIMESTAMP");
  const std::uint8_t value = 1;
  const auto cookie =
      xcb_change_property_checked(connection, XCB_PROP_MODE_REPLACE, window,
                                  property, XCB_ATOM_INTEGER, 8, 1, &value);
  auto error = xcb_ptr(xcb_request_check(connection, cookie));
  expect(!error && xcb_flush(connection) > 0, "request server timestamp event");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(connection));
    if (event && (event->response_type & 0x7fU) == XCB_PROPERTY_NOTIFY) {
      return reinterpret_cast<xcb_property_notify_event_t *>(event.get())->time;
    }
    pollfd descriptor{xcb_get_file_descriptor(connection), POLLIN, 0};
    (void)::poll(&descriptor, 1, 5);
  }
  throw std::runtime_error{"server timestamp event timeout"};
}

void serve_timestamp_and_text(xcb_connection_t *owner,
                              xcb_timestamp_t ownership_timestamp,
                              std::string_view text) {
  const xcb_atom_t timestamp_atom = intern_atom(owner, "TIMESTAMP");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  std::size_t served = 0;
  while (served < 2 && std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(owner));
    if (!event) {
      pollfd descriptor{xcb_get_file_descriptor(owner), POLLIN, 0};
      (void)::poll(&descriptor, 1, 5);
      continue;
    }
    if ((event->response_type & 0x7fU) != XCB_SELECTION_REQUEST) {
      continue;
    }
    const auto &request =
        *reinterpret_cast<const xcb_selection_request_event_t *>(event.get());
    const xcb_atom_t property =
        request.property == XCB_ATOM_NONE ? request.target : request.property;
    if (request.target == timestamp_atom) {
      xcb_change_property(owner, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, XCB_ATOM_INTEGER, 32, 1,
                          &ownership_timestamp);
    } else {
      xcb_change_property(owner, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, request.target, 8,
                          static_cast<std::uint32_t>(text.size()), text.data());
    }
    xcb_selection_notify_event_t notify{};
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.time = request.time;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.property = property;
    xcb_send_event(owner, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char *>(&notify));
    expect(xcb_flush(owner) > 0, "serve timestamp/text request");
    ++served;
  }
  expect(served == 2, "serve baseline timestamp and text requests");
}

void reject_one_timestamp_request(xcb_connection_t *owner) {
  const xcb_atom_t timestamp_atom = intern_atom(owner, "TIMESTAMP");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(owner));
    if (!event) {
      pollfd descriptor{xcb_get_file_descriptor(owner), POLLIN, 0};
      (void)::poll(&descriptor, 1, 5);
      continue;
    }
    if ((event->response_type & 0x7fU) != XCB_SELECTION_REQUEST) {
      continue;
    }
    const auto &request =
        *reinterpret_cast<const xcb_selection_request_event_t *>(event.get());
    expect(request.target == timestamp_atom,
           "initial baseline requests TIMESTAMP");
    xcb_selection_notify_event_t notify{};
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.time = request.time;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.property = XCB_ATOM_NONE;
    xcb_send_event(owner, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char *>(&notify));
    expect(xcb_flush(owner) > 0, "reject unsupported TIMESTAMP request");
    return;
  }
  throw std::runtime_error{"unsupported TIMESTAMP request timeout"};
}

void serve_text_and_targets(xcb_connection_t *owner, std::string_view text,
                            const std::vector<xcb_atom_t> &targets) {
  const xcb_atom_t targets_atom = intern_atom(owner, "TARGETS");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  std::size_t served = 0;
  while (served < 2 && std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(owner));
    if (!event) {
      pollfd descriptor{xcb_get_file_descriptor(owner), POLLIN, 0};
      (void)::poll(&descriptor, 1, 5);
      continue;
    }
    const std::uint8_t type =
        static_cast<std::uint8_t>(event->response_type & 0x7fU);
    if (type != XCB_SELECTION_REQUEST) {
      continue;
    }
    const auto &request =
        *reinterpret_cast<const xcb_selection_request_event_t *>(event.get());
    const xcb_atom_t property =
        request.property == XCB_ATOM_NONE ? request.target : request.property;
    if (request.target == targets_atom) {
      xcb_change_property(owner, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, XCB_ATOM_ATOM, 32,
                          static_cast<std::uint32_t>(targets.size()),
                          targets.data());
    } else {
      xcb_change_property(owner, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, request.target, 8,
                          static_cast<std::uint32_t>(text.size()), text.data());
    }
    xcb_selection_notify_event_t notify{};
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.time = request.time;
    notify.requestor = request.requestor;
    notify.selection = request.selection;
    notify.target = request.target;
    notify.property = property;
    xcb_send_event(owner, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char *>(&notify));
    expect(xcb_flush(owner) > 0, "serve external target contract");
    ++served;
  }
  expect(served == 2, "serve text and TARGETS requests");
}

std::string read_property(xcb_connection_t *connection, xcb_window_t window,
                          xcb_atom_t property, xcb_atom_t expected_type,
                          bool remove = true) {
  const auto cookie = xcb_get_property(connection, remove, window, property,
                                       XCB_GET_PROPERTY_TYPE_ANY, 0, 4096);
  xcb_generic_error_t *raw_error = nullptr;
  auto reply = xcb_ptr(xcb_get_property_reply(connection, cookie, &raw_error));
  auto error = xcb_ptr(raw_error);
  expect(!error && reply, "read selection property");
  expect(reply->type == expected_type, "selection property actual_type");
  expect(reply->format == 8, "selection property format");
  expect(reply->bytes_after == 0, "selection property must be complete");
  const int size = xcb_get_property_value_length(reply.get());
  expect(size >= 0, "selection property length");
  return std::string{
      static_cast<const char *>(xcb_get_property_value(reply.get())),
      static_cast<std::size_t>(size)};
}

std::uint32_t read_u32_property(xcb_connection_t *connection,
                                xcb_window_t window, xcb_atom_t property,
                                xcb_atom_t expected_type) {
  const auto cookie =
      xcb_get_property(connection, 1, window, property, expected_type, 0, 1);
  xcb_generic_error_t *raw_error = nullptr;
  auto reply = xcb_ptr(xcb_get_property_reply(connection, cookie, &raw_error));
  auto error = xcb_ptr(raw_error);
  expect(!error && reply, "read 32-bit selection property");
  expect(reply->type == expected_type && reply->format == 32 &&
             reply->bytes_after == 0 &&
             xcb_get_property_value_length(reply.get()) == 4,
         "selection property is one complete 32-bit value");
  std::uint32_t value = 0;
  std::memcpy(&value, xcb_get_property_value(reply.get()), sizeof(value));
  return value;
}

std::vector<xcb_atom_t> read_atom_property(xcb_connection_t *connection,
                                           xcb_window_t window,
                                           xcb_atom_t property) {
  const auto cookie =
      xcb_get_property(connection, 1, window, property, XCB_ATOM_ATOM, 0, 64);
  xcb_generic_error_t *raw_error = nullptr;
  auto reply = xcb_ptr(xcb_get_property_reply(connection, cookie, &raw_error));
  auto error = xcb_ptr(raw_error);
  expect(!error && reply && reply->type == XCB_ATOM_ATOM &&
             reply->format == 32 && reply->bytes_after == 0,
         "read TARGETS atom list");
  const int bytes = xcb_get_property_value_length(reply.get());
  expect(bytes >= 0 && bytes % 4 == 0, "TARGETS has complete atoms");
  const auto *atoms =
      static_cast<const xcb_atom_t *>(xcb_get_property_value(reply.get()));
  return std::vector<xcb_atom_t>{atoms, atoms + bytes / 4};
}

template <typename Predicate>
void pump_until(punto::ClipboardManager &manager, Predicate predicate,
                const char *message) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    (void)manager.pump_events();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  expect(predicate(), message);
}

void test_delivery_receipts_and_targets() {
  Connection owner = connect_to();
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
  expect(manager->is_open(), "manager adopts healthy XCB connection");
  expect(manager->set_text(punto::Selection::Clipboard, "payload") ==
             punto::ClipboardResult::Ok,
         "own clipboard payload");
  const std::uint64_t generation =
      manager->selection_generation(punto::Selection::Clipboard);
  expect(generation != 0, "set_text creates generation");
  expect(manager->selection_request_seq(punto::Selection::Clipboard) == 0,
         "set_text alone is not a delivery");

  Connection requester = connect_to();
  const xcb_window_t window = create_window(requester.value, requester.screen);
  const xcb_atom_t clipboard = intern_atom(requester.value, "CLIPBOARD");
  const xcb_atom_t targets = intern_atom(requester.value, "TARGETS");
  const xcb_atom_t utf8 = intern_atom(requester.value, "UTF8_STRING");
  const xcb_atom_t metadata_property =
      intern_atom(requester.value, "PUNTO_TEST_TARGETS");

  const auto metadata = request_selection(
      requester.value, window, *manager, clipboard, targets, metadata_property);
  expect(metadata.property == metadata_property, "TARGETS request succeeds");
  const auto advertised =
      read_atom_property(requester.value, window, metadata_property);
  const xcb_atom_t timestamp = intern_atom(requester.value, "TIMESTAMP");
  expect(std::find(advertised.begin(), advertised.end(), timestamp) !=
             advertised.end(),
         "TARGETS advertises ICCCM TIMESTAMP");
  expect(manager->selection_request_seq(punto::Selection::Clipboard) == 0,
         "TARGETS metadata must not confirm payload delivery");

  const xcb_atom_t timestamp_property =
      intern_atom(requester.value, "PUNTO_TEST_TIMESTAMP");
  const auto timestamp_notify =
      request_selection(requester.value, window, *manager, clipboard, timestamp,
                        timestamp_property);
  expect(timestamp_notify.property == timestamp_property,
         "TIMESTAMP request succeeds");
  expect(read_u32_property(requester.value, window, timestamp_property,
                           XCB_ATOM_INTEGER) != XCB_CURRENT_TIME,
         "TIMESTAMP returns the server-fenced acquisition time");
  expect(manager->selection_request_seq(punto::Selection::Clipboard) == 0,
         "TIMESTAMP metadata is not a payload receipt");

  const xcb_atom_t unsupported =
      intern_atom(requester.value, "PUNTO_TEST_UNSUPPORTED");
  const auto rejected =
      request_selection(requester.value, window, *manager, clipboard,
                        unsupported, metadata_property);
  expect(rejected.property == XCB_ATOM_NONE, "unsupported target is rejected");
  expect(manager->selection_request_seq(punto::Selection::Clipboard) == 0,
         "rejected target must not confirm payload delivery");

  const auto delivered = request_selection(requester.value, window, *manager,
                                           clipboard, utf8, XCB_ATOM_NONE);
  expect(delivered.property == utf8,
         "ICCCM property=None falls back to requested target");
  expect(read_property(requester.value, window, utf8, utf8) == "payload",
         "text payload is exact");
  const std::uint64_t first_request_seq =
      manager->selection_request_seq(punto::Selection::Clipboard);
  expect(first_request_seq == 1,
         "first text request advances independent receipt sequence");

  const auto delivered_again = request_selection(
      requester.value, window, *manager, clipboard, utf8, XCB_ATOM_NONE);
  expect(delivered_again.property == utf8,
         "second text request for the same generation succeeds");
  expect(read_property(requester.value, window, utf8, utf8) == "payload",
         "second text request returns the same payload");
  expect(manager->selection_generation(punto::Selection::Clipboard) ==
             generation,
         "repeated delivery does not change content generation");
  expect(manager->selection_request_seq(punto::Selection::Clipboard) ==
             first_request_seq + 1,
         "every successful text request advances the receipt sequence");

  set_active_window(requester.value, requester.screen, window);
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "arm requestor-bound paste receipt");
  Connection foreign = connect_to();
  const xcb_window_t foreign_window =
      create_window(foreign.value, foreign.screen);
  const xcb_atom_t foreign_clipboard = intern_atom(foreign.value, "CLIPBOARD");
  const xcb_atom_t foreign_utf8 = intern_atom(foreign.value, "UTF8_STRING");
  (void)request_selection(foreign.value, foreign_window, *manager,
                          foreign_clipboard, foreign_utf8, XCB_ATOM_NONE);
  expect(!manager->paste_receipt_seen(*receipt),
         "unrelated clipboard consumer cannot acknowledge paste");
  expect(manager->paste_receipt_request_seq(*receipt) == 0,
         "unrelated clipboard consumer cannot advance receipt activity");
  const xcb_window_t same_client_helper =
      create_window(requester.value, requester.screen);
  (void)request_selection(requester.value, same_client_helper, *manager,
                          clipboard, utf8, XCB_ATOM_NONE);
  expect(manager->paste_receipt_seen(*receipt),
         "same-client helper window acknowledges active application paste");
  const std::uint64_t receipt_request_seq =
      manager->paste_receipt_request_seq(*receipt);
  expect(receipt_request_seq == 1,
         "first same-client delivery advances receipt activity");
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  const auto delayed_active_request = request_selection(
      requester.value, window, *manager, clipboard, utf8, XCB_ATOM_NONE);
  expect(delayed_active_request.property == utf8,
         "delayed active-window paste request succeeds after early receipt");
  expect(read_property(requester.value, window, utf8, utf8) == "payload",
         "replacement ownership retained for delayed active-window paste");
  expect(manager->paste_receipt_request_seq(*receipt) ==
             receipt_request_seq + 1,
         "later same-client delivery renews receipt activity");
  (void)request_selection(foreign.value, foreign_window, *manager,
                          foreign_clipboard, foreign_utf8, XCB_ATOM_NONE);
  expect(manager->paste_receipt_request_seq(*receipt) ==
             receipt_request_seq + 1,
         "foreign delivery does not renew receipt activity");

  expect(manager->set_text(punto::Selection::Clipboard, "next payload") ==
             punto::ClipboardResult::Ok,
         "new clipboard generation replaces confirmed receipt payload");
  expect(!manager->paste_receipt_seen(*receipt) &&
             manager->paste_receipt_request_seq(*receipt) == 0,
         "new clipboard generation invalidates confirmed receipt");

  const auto expired_receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(expired_receipt.has_value(), "arm receipt for expiry boundary");
  manager->activate_paste_receipt(*expired_receipt,
                                  std::chrono::steady_clock::now());
  const auto expired_request = request_selection(
      requester.value, window, *manager, clipboard, utf8, XCB_ATOM_NONE);
  expect(expired_request.property == XCB_ATOM_NONE,
         "expired receipt rejects a late expected-client request");
  expect(manager->paste_receipt_status(*expired_receipt) ==
             punto::PasteReceiptStatus::Expired,
         "expired receipt remains observable until lifecycle cleanup");
  manager->cancel_paste_receipt(*expired_receipt);

  const auto delivered_before_deadline =
      manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(delivered_before_deadline.has_value(),
         "arm receipt across no-receipt boundary");
  manager->activate_paste_receipt(*delivered_before_deadline,
                                  std::chrono::steady_clock::now() +
                                      std::chrono::seconds{2});
  const auto delivered_before = request_selection(
      requester.value, window, *manager, clipboard, utf8, XCB_ATOM_NONE);
  expect(delivered_before.property == utf8,
         "expected client receives payload before deadline");
  const auto delivered_sequence =
      manager->paste_receipt_request_seq(*delivered_before_deadline);
  manager->activate_paste_receipt(*delivered_before_deadline,
                                  std::chrono::steady_clock::now());
  const xcb_atom_t text_plain = intern_atom(requester.value, "text/plain");
  const auto delivered_after = request_selection(
      requester.value, window, *manager, clipboard, text_plain, XCB_ATOM_NONE);
  expect(delivered_after.property == text_plain,
         "delivered receipt remains available after no-receipt deadline");
  expect(manager->paste_receipt_status(*delivered_before_deadline) ==
                 punto::PasteReceiptStatus::Delivered &&
             manager->paste_receipt_request_seq(*delivered_before_deadline) ==
                 delivered_sequence + 1,
         "post-deadline target stays in delivered lifecycle");
  manager->cancel_paste_receipt(*delivered_before_deadline);

  const auto cleared_receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(cleared_receipt.has_value(), "arm receipt before ownership loss");
  (void)request_selection(requester.value, window, *manager, clipboard, utf8,
                          XCB_ATOM_NONE);
  expect(manager->paste_receipt_seen(*cleared_receipt),
         "confirm receipt before ownership loss");
  auto takeover_error = xcb_ptr(xcb_request_check(
      foreign.value,
      xcb_set_selection_owner_checked(foreign.value, foreign_window,
                                      foreign_clipboard, XCB_CURRENT_TIME)));
  expect(!takeover_error && xcb_flush(foreign.value) > 0,
         "foreign client takes clipboard ownership");
  (void)manager->pump_events();
  expect(!manager->paste_receipt_seen(*cleared_receipt) &&
             manager->paste_receipt_request_seq(*cleared_receipt) == 0,
         "selection clear invalidates confirmed receipt immediately");
  xcb_disconnect(foreign.value);

  expect(manager->set_text(punto::Selection::Primary, "primary") ==
             punto::ClipboardResult::Ok,
         "own PRIMARY payload");
  const std::uint64_t primary_generation =
      manager->selection_generation(punto::Selection::Primary);
  const xcb_atom_t primary = XCB_ATOM_PRIMARY;
  const xcb_atom_t primary_property =
      intern_atom(requester.value, "PUNTO_TEST_PRIMARY");
  const auto primary_notify = request_selection(
      requester.value, window, *manager, primary, utf8, primary_property);
  expect(primary_notify.property == primary_property,
         "PRIMARY payload transfer succeeds");
  expect(primary_generation != 0, "PRIMARY has a content generation");
  expect(manager->selection_request_seq(punto::Selection::Primary) == 1,
         "PRIMARY has an independent request sequence");

  xcb_disconnect(requester.value);
}

void test_preexisting_owner_has_timestamp_fenced_baseline() {
  Connection external = connect_to();
  const xcb_window_t external_window =
      create_window(external.value, external.screen);
  const xcb_atom_t clipboard = intern_atom(external.value, "CLIPBOARD");
  const xcb_timestamp_t ownership_timestamp =
      server_timestamp(external.value, external_window);
  auto own_error = xcb_ptr(xcb_request_check(
      external.value,
      xcb_set_selection_owner_checked(external.value, external_window,
                                      clipboard, ownership_timestamp)));
  expect(!own_error && xcb_flush(external.value) > 0,
         "publish clipboard before manager open");
  std::thread server{[&] {
    serve_timestamp_and_text(external.value, ownership_timestamp,
                             "pre-existing");
  }};

  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  expect(manager->is_open(), "manager opens with pre-existing owner");

  const auto read = manager->get_text_with_owner(punto::Selection::Clipboard);
  server.join();
  expect(read && read->text == "pre-existing" &&
             read->owner == external_window &&
             read->selection_timestamp != XCB_CURRENT_TIME,
         "pre-existing owner is immediately readable with XFixes fence");
  expect(manager->set_text_if_owner(punto::Selection::Clipboard, *read,
                                    "replacement"),
         "baseline timestamp permits exact conditional takeover");
  xcb_disconnect(external.value);
}

void test_preexisting_owner_without_timestamp_uses_local_fence() {
  Connection external = connect_to();
  const xcb_window_t external_window =
      create_window(external.value, external.screen);
  auto own_error = xcb_ptr(xcb_request_check(
      external.value,
      xcb_set_selection_owner_checked(external.value, external_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!own_error && xcb_flush(external.value) > 0,
         "publish non-ICCCM PRIMARY owner before manager open");
  std::thread timestamp_rejection{
      [&] { reject_one_timestamp_request(external.value); }};

  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  timestamp_rejection.join();
  expect(manager->is_open(),
         "manager opens when an existing owner rejects TIMESTAMP");

  std::thread text_server{
      [&] { serve_one_text_request(external.value, "selected"); }};
  const auto read = manager->get_text_with_owner(punto::Selection::Primary);
  text_server.join();
  expect(read && read->text == "selected" && read->owner == external_window &&
             read->selection_timestamp != XCB_CURRENT_TIME,
         "local server fence keeps the non-ICCCM owner readable");
  xcb_disconnect(external.value);
}

void test_text_read_classifies_owner_rejection_and_timeout() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen,
      std::chrono::milliseconds{100});
  punto::ClipboardResult result = punto::ClipboardResult::ConversionFailed;
  const auto empty =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  expect(empty && empty->text.empty() && empty->owner == XCB_WINDOW_NONE &&
             result == punto::ClipboardResult::Ok,
         "no owner remains a known empty selection, not an explicit refusal");

  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  auto owner_error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!owner_error && xcb_flush(source.value) > 0,
         "publish the real classification control owner");
  for (const auto expected :
       {punto::ClipboardResult::Ok, punto::ClipboardResult::NoSelection,
        punto::ClipboardResult::ConversionFailed}) {
    std::thread server{
        [&] { serve_one_text_request(source.value, "selected", expected); }};
    result = punto::ClipboardResult::NoConnection;
    const auto read =
        manager->get_text_with_owner(punto::Selection::Primary, &result);
    server.join();
    expect(result == expected && manager->is_open(),
           "valid text, actual NONE and malformed properties stay distinct");
    expect(expected == punto::ClipboardResult::Ok
               ? read && read->text == "selected" &&
                     read->owner == source_window
               : !read,
           "classification preserves the optional payload contract");
  }

  result = punto::ClipboardResult::NoSelection;
  const auto unanswered =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  expect(!unanswered && result == punto::ClipboardResult::Timeout &&
             manager->is_open(),
         "a healthy unanswered conversion is not an explicit owner refusal");
  bool request_observed = false;
  while (auto event = xcb_ptr(xcb_poll_for_event(source.value))) {
    request_observed = request_observed ||
                       (event->response_type & 0x7fU) == XCB_SELECTION_REQUEST;
  }
  expect(request_observed, "timeout control issued a real selection request");
  xcb_disconnect(source.value);
  const auto release_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
  bool server_released = false;
  while (!server_released &&
         std::chrono::steady_clock::now() < release_deadline) {
    const auto cookie =
        xcb_get_selection_owner(manager_connection.value, XCB_ATOM_PRIMARY);
    expect(xcb_flush(manager_connection.value) > 0,
           "submit actual post-disconnect owner observation");
    void *raw_reply = nullptr;
    xcb_generic_error_t *raw_error = nullptr;
    while (!xcb_poll_for_reply(manager_connection.value, cookie.sequence,
                               &raw_reply, &raw_error) &&
           std::chrono::steady_clock::now() < release_deadline) {
      pollfd descriptor{xcb_get_file_descriptor(manager_connection.value),
                        POLLIN, 0};
      (void)::poll(&descriptor, 1, 1);
    }
    auto reply =
        xcb_ptr(static_cast<xcb_get_selection_owner_reply_t *>(raw_reply));
    auto error = xcb_ptr(raw_error);
    expect(reply && !error,
           "post-disconnect owner reply stays within its original deadline");
    server_released = reply->owner == XCB_WINDOW_NONE;
    if (!server_released) {
      (void)::poll(nullptr, 0, 1);
    }
  }
  expect(server_released,
         "server processes the real owner's exit before empty recovery");
  const auto released =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  expect(released && released->owner == XCB_WINDOW_NONE &&
             result == punto::ClipboardResult::Ok,
         "the same manager recovers after the nonresponding owner exits");
  manager->close();
  const auto closed =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  expect(!closed && result == punto::ClipboardResult::NoConnection,
         "a closed raw connection cannot become an owner rejection");
}

xcb_selection_request_event_t await_text_request(xcb_connection_t *owner,
                                                 xcb_atom_t utf8) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (std::chrono::steady_clock::now() < deadline) {
    auto event = xcb_ptr(xcb_poll_for_event(owner));
    if (event && (event->response_type & 0x7fU) == XCB_SELECTION_REQUEST) {
      const auto request =
          *reinterpret_cast<xcb_selection_request_event_t *>(event.get());
      expect(request.selection == XCB_ATOM_PRIMARY && request.target == utf8,
             "outgoing read requests actual PRIMARY UTF8 bytes");
      return request;
    }
    pollfd descriptor{xcb_get_file_descriptor(owner), POLLIN, 0};
    (void)::poll(&descriptor, 1, 5);
  }
  throw std::runtime_error{"PRIMARY read request timeout"};
}

bool reply_to_text_request(xcb_connection_t *owner,
                           const xcb_selection_request_event_t &request,
                           std::string_view text, bool allow_expired = false) {
  const auto property = xcb_change_property_checked(
      owner, XCB_PROP_MODE_REPLACE, request.requestor, request.property,
      request.target, 8, static_cast<std::uint32_t>(text.size()), text.data());
  auto property_error = xcb_ptr(xcb_request_check(owner, property));
  xcb_selection_notify_event_t notify{};
  notify.response_type = XCB_SELECTION_NOTIFY;
  notify.time = request.time;
  notify.requestor = request.requestor;
  notify.selection = request.selection;
  notify.target = request.target;
  notify.property = request.property;
  const auto notification = xcb_send_event_checked(
      owner, 0, request.requestor, XCB_EVENT_MASK_NO_EVENT,
      reinterpret_cast<const char *>(&notify));
  auto notify_error = xcb_ptr(xcb_request_check(owner, notification));
  if (allow_expired && property_error && notify_error &&
      property_error->error_code == XCB_WINDOW &&
      notify_error->error_code == XCB_WINDOW) {
    return false;
  }
  expect(!property_error && !notify_error && xcb_flush(owner) > 0,
         "deliver real UTF8 property and notification");
  return true;
}

void test_expired_read_cannot_supply_the_next_payload() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const auto source_window = create_window(source.value, source.screen);
  const auto utf8 = intern_atom(source.value, "UTF8_STRING");
  auto ownership_error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!ownership_error, "publish late-response provider");
  punto::ClipboardResult result = punto::ClipboardResult::Ok;
  const auto expired =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  expect(!expired && result == punto::ClipboardResult::Timeout &&
             manager->is_open(),
         "first real read times out without closing the manager");
  const auto old_request = await_text_request(source.value, utf8);
  const auto before =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(before.has_value(), "capture same manager epoch before new read");
  std::atomic<bool> read_completed{false};
  bool old_target_expired = false;
  bool current_unresolved_after_old = false;
  xcb_window_t current_requestor = XCB_WINDOW_NONE;
  std::exception_ptr peer_error;
  std::thread provider{[&] {
    try {
      const auto current = await_text_request(source.value, utf8);
      current_requestor = current.requestor;
      old_target_expired =
          !reply_to_text_request(source.value, old_request, "OLD", true);
      if (!old_target_expired) {
        const auto limit =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        while (!read_completed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < limit) {
          std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
      }
      current_unresolved_after_old =
          !read_completed.load(std::memory_order_acquire);
      (void)reply_to_text_request(source.value, current, "CURRENT");
    } catch (...) {
      peer_error = std::current_exception();
    }
  }};
  const auto current =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  read_completed.store(true, std::memory_order_release);
  provider.join();
  if (peer_error) {
    std::rethrow_exception(peer_error);
  }
  expect(current && current->text == "CURRENT",
         "late OLD bytes cannot satisfy the subsequent read");
  expect(old_target_expired && current_unresolved_after_old &&
             current_requestor != old_request.requestor,
         "expired requestor is destroyed before the new read receives data");
  expect(manager->is_open() &&
             current->connection_generation == before->connection_generation,
         "late-response isolation does not use reconnect as recovery");
  xcb_disconnect(source.value);
}

void test_read_rejects_owner_aba_and_recovers() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const auto source_window = create_window(source.value, source.screen);
  const auto foreign_window = create_window(source.value, source.screen);
  const auto utf8 = intern_atom(source.value, "UTF8_STRING");
  const auto publish = [&](xcb_window_t owner) {
    auto error = xcb_ptr(xcb_request_check(
        source.value,
        xcb_set_selection_owner_checked(source.value, owner, XCB_ATOM_PRIMARY,
                                        XCB_CURRENT_TIME)));
    auto confirmed = xcb_ptr(xcb_get_selection_owner_reply(
        source.value, xcb_get_selection_owner(source.value, XCB_ATOM_PRIMARY),
        nullptr));
    expect(!error && confirmed && confirmed->owner == owner,
           "real server confirms each ownership transition");
  };
  publish(source_window);
  const auto before =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(before && before->owner == source_window,
         "capture initial A ownership history");
  std::exception_ptr peer_error;
  std::thread provider{[&] {
    try {
      const auto request = await_text_request(source.value, utf8);
      publish(foreign_window);
      publish(source_window);
      (void)reply_to_text_request(source.value, request, "old A");
    } catch (...) {
      peer_error = std::current_exception();
    }
  }};
  punto::ClipboardResult result = punto::ClipboardResult::Ok;
  const auto stale =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  provider.join();
  if (peer_error) {
    std::rethrow_exception(peer_error);
  }
  expect(!stale && result == punto::ClipboardResult::ConversionFailed &&
             manager->is_open(),
         "final owner A does not authorize a read across A-B-A history");
  const auto after = manager->get_selection_identity(punto::Selection::Primary);
  expect(after && after->owner == before->owner &&
             after->owner_generation != before->owner_generation &&
             after->connection_generation == before->connection_generation,
         "ABA invalidates ownership history without reconnecting");
  std::thread fresh_provider{
      [&] { serve_one_text_request(source.value, "fresh A"); }};
  const auto fresh =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  fresh_provider.join();
  expect(fresh && fresh->text == "fresh A" &&
             result == punto::ClipboardResult::Ok &&
             fresh->connection_generation == before->connection_generation,
         "same manager accepts a new continuous A read after ABA rejection");
  xcb_disconnect(source.value);
}

void test_default_read_keeps_its_own_deadline() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen,
      std::chrono::milliseconds{10});
  expect(manager->is_open(), "default deadline control initializes normally");
  Connection source = connect_to();
  const auto source_window = create_window(source.value, source.screen);
  auto error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error, "publish default budget provider");
  read_deadline_fault::connection = manager_connection.value;
  read_deadline_fault::utf8 = intern_atom(source.value, "UTF8_STRING");
  read_deadline_fault::consumed = 0;
  read_deadline_fault::armed = true;
  std::thread provider{
      [&] { serve_one_text_request(source.value, "selected"); }};
  punto::ClipboardResult result = punto::ClipboardResult::Ok;
  const auto started = std::chrono::steady_clock::now();
  const auto read =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  provider.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(
      !read && result == punto::ClipboardResult::NoConnection &&
          !manager->is_open() && read_deadline_fault::consumed == 1 &&
          elapsed < std::chrono::milliseconds{150},
      "default read keeps 10ms instead of inheriting a larger global budget");
  read_deadline_fault::connection = nullptr;
  xcb_disconnect(source.value);
}

void test_selection_identity_does_not_require_payload() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  const std::uint64_t initial_generation =
      manager->selection_owner_generation(punto::Selection::Primary);
  auto error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0,
         "publish unreadable PRIMARY owner");
  pump_until(
      *manager,
      [&] {
        return manager->selection_owner_generation(punto::Selection::Primary) !=
               initial_generation;
      },
      "observe unreadable PRIMARY owner");

  const auto identity =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(identity && identity->owner == source_window &&
             identity->text.empty() &&
             identity->selection_timestamp != XCB_CURRENT_TIME,
         "capture exact PRIMARY identity without requesting its payload");
  expect(
      manager->selection_identity_matches(punto::Selection::Primary, *identity),
      "payload-free identity remains independently verifiable");
  xcb_disconnect(source.value);
}

void test_read_services_bounded_requests_without_dropping_owned_payload(
    bool read_reply_boundary = false) {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
             punto::ClipboardResult::Ok,
         "own baseline before ordinary request traffic");
  const auto generation =
      manager->selection_generation(punto::Selection::Clipboard);
  Connection source = connect_to();
  const auto source_window = create_window(source.value, source.screen);
  auto owner_error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!owner_error && xcb_flush(source.value) > 0,
         "publish PRIMARY before the ordinary request prefix");
  const auto clipboard_before =
      manager->get_selection_identity(punto::Selection::Clipboard);
  const auto primary_before =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(clipboard_before && primary_before &&
             primary_before->owner == source_window,
         "capture the healthy connection and baseline before queuing traffic");
  const auto request_sequence =
      manager->selection_request_seq(punto::Selection::Clipboard);
  punto::ClipboardResult result = punto::ClipboardResult::Ok;
  expect(!manager->get_text_with_owner(punto::Selection::Primary,
                                       std::chrono::steady_clock::now() -
                                           std::chrono::milliseconds{1},
                                       &result) &&
             result == punto::ClipboardResult::ConversionFailed &&
             manager->is_open() &&
             manager->selection_generation(punto::Selection::Clipboard) ==
                 generation &&
             manager->selection_request_seq(punto::Selection::Clipboard) ==
                 request_sequence &&
             manager->locally_owns_generation(punto::Selection::Clipboard,
                                              generation),
         "expired read leaves the owned payload and request history unchanged");

  Connection requester = connect_to();
  const auto requestor = create_window(requester.value, requester.screen);
  const auto clipboard = intern_atom(requester.value, "CLIPBOARD");
  const auto utf8 = intern_atom(requester.value, "UTF8_STRING");
  const std::array<xcb_atom_t, 2> properties{
      intern_atom(requester.value, "PUNTO_READ_PREFIX_FIRST"),
      intern_atom(requester.value, "PUNTO_READ_PREFIX_SECOND")};
  const auto reply_property =
      intern_atom(requester.value, "PUNTO_READ_REPLY_PEER");
  for (const auto property : properties) {
    auto error = xcb_ptr(xcb_request_check(
        requester.value,
        xcb_convert_selection_checked(requester.value, requestor, clipboard,
                                      utf8, property, XCB_CURRENT_TIME)));
    expect(!error, "queue a real baseline read before identity observation");
  }
  xcb_generic_error_t *raw_error = nullptr;
  auto queued = xcb_ptr(xcb_get_input_focus_reply(
      requester.value, xcb_get_input_focus(requester.value), &raw_error));
  auto queued_error = xcb_ptr(raw_error);
  expect(queued && !queued_error,
         "server confirms both requests without pumping the manager");

  if (read_reply_boundary) {
    publication_fault::connection = manager_connection.value;
    publication_fault::peer = requester.value;
    publication_fault::peer_window = requestor;
    publication_fault::clipboard = clipboard;
    publication_fault::utf8 = utf8;
    publication_fault::property = reply_property;
    publication_fault::timestamp_property =
        intern_atom(requester.value, "PUNTO_SELECTION_READ");
    publication_fault::consumed = false;
    publication_fault::stage = publication_fault::Stage::ReadReplyBoundary;
  }

  std::exception_ptr source_error;
  std::thread source_server{[&] {
    try {
      serve_one_text_request(source.value, "selected PRIMARY");
    } catch (...) {
      source_error = std::current_exception();
    }
  }};
  const auto started = std::chrono::steady_clock::now();
  const auto selected = manager->get_text_with_owner(
      punto::Selection::Primary, started + std::chrono::milliseconds{100},
      &result);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  source_server.join();
  if (read_reply_boundary) {
    publication_fault::stage = publication_fault::Stage::None;
    publication_fault::connection = nullptr;
    expect(publication_fault::consumed,
           "real PRIMARY notify queues the peer before header retirement");
  }
  expect(selected && selected->text == "selected PRIMARY" &&
             selected->owner == primary_before->owner &&
             selected->selection_timestamp ==
                 primary_before->selection_timestamp &&
             selected->owner_generation == primary_before->owner_generation &&
             selected->connection_generation ==
                 primary_before->connection_generation,
         "bounded ordinary clipboard readers preserve the exact PRIMARY read");
  if (source_error)
    std::rethrow_exception(source_error);
  expect(elapsed < std::chrono::milliseconds{100},
         "peer request service retains the original PRIMARY read deadline");
  expect(manager->is_open() && result == punto::ClipboardResult::Ok &&
             manager->selection_generation(punto::Selection::Clipboard) ==
                 generation &&
             manager->selection_request_seq(punto::Selection::Clipboard) ==
                 request_sequence + properties.size() +
                     (read_reply_boundary ? 1U : 0U) &&
             manager->locally_owns_generation(punto::Selection::Clipboard,
                                              generation),
         "read itself serves both peers and preserves clipboard ownership");
  for (const auto property : properties) {
    const auto notify = await_selection_notify(requester.value, *manager);
    expect(notify.requestor == requestor && notify.selection == clipboard &&
               notify.target == utf8 && notify.property == property &&
               read_property(requester.value, requestor, property, utf8) ==
                   "baseline",
           "both original requests independently receive complete baseline");
  }
  if (read_reply_boundary) {
    const auto notify = await_selection_notify(requester.value, *manager);
    expect(notify.requestor == requestor && notify.selection == clipboard &&
               notify.target == utf8 && notify.property == reply_property &&
               read_property(requester.value, requestor, reply_property,
                             utf8) == "baseline",
           "post-notify peer receives its exact owned payload and response");
  }
  std::thread provider{
      [&] { serve_one_text_request(source.value, "fresh PRIMARY"); }};
  const auto fresh =
      manager->get_text_with_owner(punto::Selection::Primary, &result);
  provider.join();
  const auto clipboard_after =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(fresh && fresh->text == "fresh PRIMARY" &&
             result == punto::ClipboardResult::Ok &&
             fresh->connection_generation ==
                 primary_before->connection_generation &&
             clipboard_after &&
             clipboard_after->owner == clipboard_before->owner &&
             clipboard_after->connection_generation ==
                 clipboard_before->connection_generation &&
             clipboard_after->owner_generation ==
                 clipboard_before->owner_generation &&
             manager->selection_generation(punto::Selection::Clipboard) ==
                 generation &&
             manager->owns_generation(punto::Selection::Clipboard, generation),
         "fresh read recovers on the same connection and owned generation");
  expect(manager->restore_text_if_generation(punto::Selection::Clipboard,
                                             generation, "restored"),
         "peer-servicing read preserves generation-scoped restoration duty");
  expect(manager->get_text(punto::Selection::Clipboard) ==
             std::optional<std::string>{"restored"},
         "retained restoration publishes exact saved text");
  xcb_disconnect(requester.value);
  xcb_disconnect(source.value);
}

void test_text_read_deletes_private_property_atomically() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  auto error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0,
         "publish PRIMARY payload owner");

  property_fault::checked_deletes.store(0, std::memory_order_relaxed);
  xcb_window_t requestor = XCB_WINDOW_NONE;
  std::thread server{[&] {
    serve_one_text_request(source.value, "selected", punto::ClipboardResult::Ok,
                           &requestor);
  }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  server.join();
  expect(selected && selected->text == "selected", "read exact PRIMARY text");
  expect(property_fault::checked_deletes.load(std::memory_order_relaxed) == 0,
         "fresh query has no stale property requiring a preflight delete");
  xcb_generic_error_t *raw_error = nullptr;
  auto query_window = xcb_ptr(xcb_get_window_attributes_reply(
      manager_connection.value,
      xcb_get_window_attributes(manager_connection.value, requestor),
      &raw_error));
  auto query_error = xcb_ptr(raw_error);
  expect(!query_window && query_error && query_error->error_code == XCB_WINDOW,
         "completed read destroys its private requestor and returned property");
  raw_error = nullptr;
  auto publisher = xcb_ptr(xcb_get_window_attributes_reply(
      manager_connection.value,
      xcb_get_window_attributes(
          manager_connection.value,
          punto::ClipboardManagerTestAccess::window(*manager)),
      &raw_error));
  auto publisher_error = xcb_ptr(raw_error);
  expect(publisher && !publisher_error,
         "read resource retirement preserves the publisher and owner watcher");
  xcb_disconnect(source.value);
}

void test_owner_transition_token_tracks_exact_identity_history() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  const xcb_window_t same_client_window =
      create_window(source.value, source.screen);
  const xcb_atom_t primary = XCB_ATOM_PRIMARY;
  set_active_window(source.value, source.screen, source_window);
  const std::uint32_t source_client =
      punto::ClipboardManagerTestAccess::resource_client_id(*manager,
                                                            source_window);
  expect(source_client != 0, "derive exact source-client identity");

  const auto previous = manager->get_text_with_owner(punto::Selection::Primary);
  expect(previous && previous->owner == XCB_WINDOW_NONE,
         "transition fixture starts with absent PRIMARY");

  const auto publish_source = [&](xcb_window_t owner) {
    auto error = xcb_ptr(xcb_request_check(
        source.value, xcb_set_selection_owner_checked(
                          source.value, owner, primary, XCB_CURRENT_TIME)));
    expect(!error && xcb_flush(source.value) > 0,
           "publish source PRIMARY owner");
  };
  publish_source(source_window);
  std::thread first_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  first_server.join();
  expect(selected && selected->owner == source_window,
         "capture source PRIMARY identity");
  expect(
      manager->selection_identity_matches(punto::Selection::Primary, *selected),
      "exact PRIMARY identity remains valid without another payload read");
  const auto released_token = manager->arm_owner_transition(
      punto::Selection::Primary, *selected, source_client);
  expect(released_token.has_value(), "arm exact source transition");

  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  publish_source(source_window);
  const std::uint64_t churn_generation =
      manager->selection_owner_generation(punto::Selection::Primary);
  pump_until(
      *manager,
      [&] {
        return manager->selection_owner_generation(punto::Selection::Primary) !=
               churn_generation;
      },
      "observe same-owner XFixes churn");
  expect(!manager->selection_identity_matches(punto::Selection::Primary,
                                              *selected),
         "same-owner timestamp churn invalidates the previous identity");
  expect(manager->owner_transition_status(*released_token) ==
             punto::OwnerTransitionStatus::Pending,
         "same-owner timestamp churn does not prove source collapse");

  publish_source(XCB_WINDOW_NONE);
  pump_until(
      *manager,
      [&] {
        return manager->owner_transition_status(*released_token) !=
               punto::OwnerTransitionStatus::Pending;
      },
      "observe source release");
  expect(manager->owner_transition_status(*released_token) ==
             punto::OwnerTransitionStatus::Released,
         "exact source-to-None transition is distinguished");
  expect(manager->restore_selection_after_owner_transition(*released_token,
                                                           *previous),
         "identity-fenced restore ignores harmless same-owner churn");

  publish_source(source_window);
  std::thread second_server{
      [&] { serve_one_text_request(source.value, "second"); }};
  const auto second = manager->get_text_with_owner(punto::Selection::Primary);
  second_server.join();
  expect(second && second->owner == source_window,
         "capture second source selection");
  const auto same_client_token = manager->arm_owner_transition(
      punto::Selection::Primary, *second, source_client);
  expect(same_client_token.has_value(), "arm same-client transition");
  publish_source(same_client_window);
  pump_until(
      *manager,
      [&] {
        return manager->owner_transition_status(*same_client_token) !=
               punto::OwnerTransitionStatus::Pending;
      },
      "observe same-client owner transition");
  expect(manager->owner_transition_status(*same_client_token) ==
             punto::OwnerTransitionStatus::SameClientOwner,
         "same-client collapse is distinguished from foreign takeover");
  expect(manager->restore_selection_after_owner_transition(*same_client_token,
                                                           *previous),
         "same-client collapse may restore the previous absent PRIMARY");

  publish_source(source_window);
  std::thread third_server{
      [&] { serve_one_text_request(source.value, "third"); }};
  const auto third = manager->get_text_with_owner(punto::Selection::Primary);
  third_server.join();
  const auto ambiguous_token = manager->arm_owner_transition(
      punto::Selection::Primary, *third, source_client);
  expect(ambiguous_token.has_value(), "arm foreign-takeover transition");
  Connection foreign = connect_to();
  const xcb_window_t foreign_window =
      create_window(foreign.value, foreign.screen);
  auto foreign_error = xcb_ptr(xcb_request_check(
      foreign.value,
      xcb_set_selection_owner_checked(foreign.value, foreign_window, primary,
                                      XCB_CURRENT_TIME)));
  expect(!foreign_error && xcb_flush(foreign.value) > 0,
         "foreign client takes PRIMARY");
  pump_until(
      *manager,
      [&] {
        return manager->owner_transition_status(*ambiguous_token) !=
               punto::OwnerTransitionStatus::Pending;
      },
      "observe foreign takeover");
  expect(manager->owner_transition_status(*ambiguous_token) ==
             punto::OwnerTransitionStatus::Ambiguous,
         "foreign takeover latches ambiguity");
  foreign_error = xcb_ptr(xcb_request_check(
      foreign.value,
      xcb_set_selection_owner_checked(foreign.value, XCB_WINDOW_NONE, primary,
                                      XCB_CURRENT_TIME)));
  expect(!foreign_error && xcb_flush(foreign.value) > 0,
         "foreign owner releases PRIMARY");
  (void)manager->pump_events();
  expect(manager->owner_transition_status(*ambiguous_token) ==
             punto::OwnerTransitionStatus::Ambiguous,
         "later None cannot erase foreign-takeover history");
  expect(!manager->restore_selection_after_owner_transition(*ambiguous_token,
                                                            *previous),
         "ambiguous transition cannot restore stale selection data");

  xcb_disconnect(foreign.value);
  xcb_disconnect(source.value);
}

void test_identity_check_stops_after_initial_pump_closes_connection() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  auto error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0, "publish identity fixture");
  std::thread server{[&] { serve_one_text_request(source.value, "selected"); }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  server.join();
  expect(selected.has_value(), "capture identity before pump fault");

  identity_fault::connection = manager_connection.value;
  identity_fault::inject_error = true;
  identity_fault::error_delivered = false;
  identity_fault::disconnected = false;
  identity_fault::owner_query_after_close = false;
  expect(!manager->selection_identity_matches(punto::Selection::Primary,
                                              *selected),
         "identity check fails closed after pump error");
  expect(identity_fault::error_delivered,
         "identity fault reached the initial method pump");
  expect(!manager->is_open(), "identity pump error closes the connection");
  expect(!identity_fault::owner_query_after_close,
         "identity check performs no XCB owner query after close");
  identity_fault::connection = nullptr;
  xcb_disconnect(source.value);
}

void test_identity_check_rejects_incomplete_event_backlog() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  const auto publish_source = [&] {
    auto error = xcb_ptr(xcb_request_check(
        source.value,
        xcb_set_selection_owner_checked(source.value, source_window,
                                        XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
    expect(!error && xcb_flush(source.value) > 0,
           "publish same-owner PRIMARY identity");
  };

  publish_source();
  std::thread first_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  first_server.join();
  expect(selected.has_value(), "capture identity before event backlog");
  expect(
      manager->selection_identity_matches(punto::Selection::Primary, *selected),
      "fully observed unchanged identity remains valid");

  const xcb_atom_t noise =
      intern_atom(manager_connection.value, "PUNTO_IDENTITY_BACKLOG");
  const std::uint32_t value = 1;
  const auto queue_noise = [&] {
    for (std::size_t index = 0; index < 160; ++index) {
      xcb_change_property(manager_connection.value, XCB_PROP_MODE_REPLACE,
                          punto::ClipboardManagerTestAccess::window(*manager),
                          noise, XCB_ATOM_CARDINAL, 32, 1, &value);
    }
    const auto noise_fence = xcb_get_input_focus(manager_connection.value);
    xcb_generic_error_t *noise_raw_error = nullptr;
    auto noise_reply = xcb_ptr(xcb_get_input_focus_reply(
        manager_connection.value, noise_fence, &noise_raw_error));
    auto noise_error = xcb_ptr(noise_raw_error);
    expect(!noise_error && noise_reply,
           "property prefix reached manager event queue");
  };
  queue_noise();
  punto::ClipboardResult result = punto::ClipboardResult::Ok;
  expect(!manager->get_text_with_owner(punto::Selection::Primary,
                                       std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds{100},
                                       &result) &&
             result == punto::ClipboardResult::ConversionFailed &&
             manager->is_open(),
         "actual incomplete queued history cannot authorize PRIMARY text");
  queue_noise();

  std::this_thread::sleep_for(std::chrono::milliseconds{5});
  publish_source();
  const auto identity_fence = xcb_get_input_focus(source.value);
  xcb_generic_error_t *identity_raw_error = nullptr;
  auto identity_reply = xcb_ptr(xcb_get_input_focus_reply(
      source.value, identity_fence, &identity_raw_error));
  auto identity_error = xcb_ptr(identity_raw_error);
  expect(!identity_error && identity_reply,
         "new same-owner identity reached the X server");

  expect(!manager->selection_identity_matches(punto::Selection::Primary,
                                              *selected),
         "incomplete pre-fence event history cannot confirm old identity");
  const std::uint64_t old_generation = selected->owner_generation;
  pump_until(
      *manager,
      [&] {
        return manager->selection_owner_generation(punto::Selection::Primary) !=
               old_generation;
      },
      "observe queued same-owner identity after bounded rejection");
  expect(!manager->selection_identity_matches(punto::Selection::Primary,
                                              *selected),
         "fully observed same-owner identity rejects stale receipt");

  std::thread second_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto refreshed =
      manager->get_text_with_owner(punto::Selection::Primary);
  second_server.join();
  expect(refreshed.has_value() && manager->selection_identity_matches(
                                      punto::Selection::Primary, *refreshed),
         "identity verification recovers without reconnect");
  xcb_disconnect(source.value);
}

void test_selection_identity_does_not_survive_reconnect() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  auto error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0, "publish reconnect fixture");
  std::thread first_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  first_server.join();
  expect(selected.has_value(), "capture identity before reconnect");
  expect(
      manager->selection_identity_matches(punto::Selection::Primary, *selected),
      "current connection generation admits unchanged identity");
  auto stale_generation = *selected;
  stale_generation.connection_generation =
      selected->connection_generation ==
              std::numeric_limits<std::uint64_t>::max()
          ? selected->connection_generation - 1
          : selected->connection_generation + 1;
  expect(!manager->selection_identity_matches(punto::Selection::Primary,
                                              stale_generation),
         "generation alone rejects an otherwise unchanged identity");
  error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, XCB_WINDOW_NONE,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0,
         "release PRIMARY before reconnect");

  Connection replacement = connect_to();
  const bool reopened = punto::ClipboardManagerTestAccess::reconnect(
      *manager, replacement.value, replacement.screen);
  expect(reopened, "reopen clipboard manager on a new XCB connection");
  error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0,
         "republish PRIMARY after reconnect");
  expect(!manager->selection_identity_matches(punto::Selection::Primary,
                                              *selected),
         "receipt from the old connection epoch is rejected");
  std::thread second_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto refreshed =
      manager->get_text_with_owner(punto::Selection::Primary);
  second_server.join();
  expect(refreshed.has_value(), "capture identity after reconnect");
  expect(manager->selection_identity_matches(punto::Selection::Primary,
                                             *refreshed),
         "identity captured in the new connection epoch remains valid");
  xcb_disconnect(source.value);
}

void test_empty_and_embedded_nul_payloads() {
  Connection owner = connect_to();
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);

  const std::string embedded{"a\0b", 3};
  expect(manager->set_text(punto::Selection::Clipboard, embedded) ==
             punto::ClipboardResult::Ok,
         "set embedded NUL payload");
  const auto read_back = manager->get_text(punto::Selection::Clipboard);
  expect(read_back && *read_back == embedded,
         "self-conversion preserves exact byte length");

  expect(manager->set_text(punto::Selection::Clipboard, "") ==
             punto::ClipboardResult::Ok,
         "empty clipboard payload is valid");
  const std::uint64_t request_seq =
      manager->selection_request_seq(punto::Selection::Clipboard);
  const auto empty = manager->get_text(punto::Selection::Clipboard);
  expect(empty && empty->empty(), "empty payload round-trips");
  expect(manager->selection_request_seq(punto::Selection::Clipboard) ==
             request_seq + 1,
         "empty payload produces a delivery receipt");
}

void test_strict_property_validation_and_incr_rejection() {
  constexpr xcb_atom_t expected = 101;
  constexpr xcb_atom_t incr = 102;
  const std::array<std::uint8_t, 3> bytes{'a', 0, 'b'};

  const auto valid = punto::ClipboardManagerTestAccess::decode(
      expected, 8, 0, bytes.data(), bytes.size(), expected, incr);
  expect(valid && valid->size() == bytes.size() && (*valid)[1] == '\0',
         "valid format-8 property preserves bytes");
  const auto empty = punto::ClipboardManagerTestAccess::decode(
      expected, 8, 0, nullptr, 0, expected, incr);
  expect(empty && empty->empty(), "empty format-8 property is valid");

  expect(!punto::ClipboardManagerTestAccess::decode(
             expected + 1, 8, 0, bytes.data(), bytes.size(), expected, incr),
         "wrong actual_type is rejected");
  expect(!punto::ClipboardManagerTestAccess::decode(
             expected, 32, 0, bytes.data(), bytes.size(), expected, incr),
         "wrong actual_format is rejected");
  expect(!punto::ClipboardManagerTestAccess::decode(
             expected, 8, 1, bytes.data(), bytes.size(), expected, incr),
         "partial property is rejected");
  expect(!punto::ClipboardManagerTestAccess::decode(
             incr, 32, 0, bytes.data(), bytes.size(), expected, incr),
         "INCR transfer is explicitly rejected");
  expect(!punto::ClipboardManagerTestAccess::decode(expected, 8, 0, nullptr, 1,
                                                    expected, incr),
         "non-empty property requires data");
  expect(!punto::ClipboardManagerTestAccess::decode(
             expected, 8, 0, bytes.data(), 16U * 1024U * 1024U + 1U, expected,
             incr),
         "oversize property is rejected before dereference");
}

void test_live_oversize_incr_offer_refuses_and_recovers() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);

  Connection producer = connect_to();
  const xcb_window_t producer_window =
      create_window(producer.value, producer.screen);
  const xcb_atom_t clipboard = intern_atom(producer.value, "CLIPBOARD");
  const xcb_atom_t incr = intern_atom(producer.value, "INCR");
  auto owner_error = xcb_ptr(xcb_request_check(
      producer.value,
      xcb_set_selection_owner_checked(producer.value, producer_window,
                                      clipboard, XCB_CURRENT_TIME)));
  expect(!owner_error, "external producer owns clipboard");

  std::atomic<bool> responded{false};
  xcb_window_t completed_requestor = XCB_WINDOW_NONE;
  const auto initial_identity =
      manager->get_selection_identity(punto::Selection::Clipboard);
  std::thread producer_thread{[&]() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline) {
      auto event = xcb_ptr(xcb_poll_for_event(producer.value));
      if (event) {
        const std::uint8_t type =
            static_cast<std::uint8_t>(event->response_type & 0x7fU);
        if (type == XCB_SELECTION_REQUEST) {
          const auto &request =
              *reinterpret_cast<const xcb_selection_request_event_t *>(
                  event.get());
          completed_requestor = request.requestor;
          const std::uint32_t announced_bytes = 32U * 1024U * 1024U;
          xcb_change_property(producer.value, XCB_PROP_MODE_REPLACE,
                              request.requestor, request.property, incr, 32, 1,
                              &announced_bytes);
          xcb_selection_notify_event_t notify{};
          notify.response_type = XCB_SELECTION_NOTIFY;
          notify.time = request.time;
          notify.requestor = request.requestor;
          notify.selection = request.selection;
          notify.target = request.target;
          notify.property = request.property;
          xcb_send_event(producer.value, 0, request.requestor,
                         XCB_EVENT_MASK_NO_EVENT,
                         reinterpret_cast<const char *>(&notify));
          (void)xcb_flush(producer.value);
          responded.store(true, std::memory_order_release);
          return;
        }
      }
      pollfd descriptor{xcb_get_file_descriptor(producer.value), POLLIN, 0};
      (void)::poll(&descriptor, 1, 5);
    }
  }};

  const auto result = manager->get_text(punto::Selection::Clipboard);
  producer_thread.join();
  expect(responded.load(std::memory_order_acquire),
         "external producer offered INCR");
  expect(!result, "INCR offer is not accepted as text");
  expect(manager->is_open() && initial_identity.has_value(),
         "oversize INCR refuses without erasing a healthy clipboard epoch");
  auto destroyed_fence = xcb_ptr(xcb_get_input_focus_reply(
      manager_connection.value, xcb_get_input_focus(manager_connection.value),
      nullptr));
  expect(static_cast<bool>(destroyed_fence),
         "same-connection destroy ordering fence");
  xcb_generic_error_t *raw_query_error = nullptr;
  auto attributes = xcb_ptr(xcb_get_window_attributes_reply(
      producer.value,
      xcb_get_window_attributes(producer.value, completed_requestor),
      &raw_query_error));
  auto query_error = xcb_ptr(raw_query_error);
  expect(!attributes && query_error && query_error->error_code == XCB_WINDOW,
         "oversize INCR destroys the actual private requestor");
  std::thread fresh_provider{
      [&] { serve_one_text_request(producer.value, "fresh"); }};
  const auto fresh = manager->get_text(punto::Selection::Clipboard);
  fresh_provider.join();
  const auto final_identity =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(fresh == std::optional<std::string>{"fresh"} && final_identity &&
             final_identity->connection_generation ==
                 initial_identity->connection_generation &&
             final_identity->owner == initial_identity->owner,
         "fresh text succeeds with unchanged connection and original owner");
  xcb_disconnect(producer.value);
}

void test_generation_guard_and_oversize_rejection() {
  Connection owner = connect_to();
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "temporary") ==
             punto::ClipboardResult::Ok,
         "set temporary clipboard");
  const std::uint64_t temporary_generation =
      manager->selection_generation(punto::Selection::Clipboard);

  expect(manager->set_text(punto::Selection::Clipboard, "newer") ==
             punto::ClipboardResult::Ok,
         "set newer clipboard");
  const std::uint64_t newer_generation =
      manager->selection_generation(punto::Selection::Clipboard);
  expect(newer_generation != temporary_generation, "generations advance");
  expect(!manager->restore_text_if_generation(punto::Selection::Clipboard,
                                              temporary_generation, "old"),
         "stale restore cannot overwrite newer generation");
  expect(manager->selection_generation(punto::Selection::Clipboard) ==
             newer_generation,
         "rejected restore leaves generation unchanged");
  const auto before_restore = manager->get_selection_identity(
      punto::Selection::Clipboard,
      std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
  expect(before_restore.has_value(), "capture published clipboard identity");
  const auto receipt_window = create_window(owner.value, owner.screen);
  set_active_window(owner.value, owner.screen, receipt_window);
  const auto old_receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(old_receipt.has_value(), "arm receipt of the old clipboard value");
  expect(manager->restore_text_if_generation(punto::Selection::Clipboard,
                                             newer_generation, "old"),
         "matching generation may be restored");
  expect(manager->selection_generation(punto::Selection::Clipboard) !=
             newer_generation,
         "successful restore creates a new generation");
  const auto after_restore = manager->get_selection_identity(
      punto::Selection::Clipboard,
      std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
  expect(
      after_restore && after_restore->owner == before_restore->owner &&
          after_restore->owner_generation != before_restore->owner_generation &&
          after_restore->selection_timestamp !=
              before_restore->selection_timestamp,
      "new clipboard value publishes fresh XFixes identity at the same owner");
  const auto restored = manager->get_text(punto::Selection::Clipboard);
  expect(restored == std::optional<std::string>{"old"},
         "freshly published clipboard identity serves the restored payload");
  expect(manager->paste_receipt_status(*old_receipt) ==
             punto::PasteReceiptStatus::Invalid,
         "restoration invalidates the previous payload receipt");

  const std::size_t maximum =
      punto::ClipboardManagerTestAccess::maximum_payload_bytes(*manager);
  expect(maximum > 0, "X server reports property capacity");
  expect(maximum <= 4096U,
         "synchronous payload stays within the bounded transport cap");
  const std::string too_large(maximum + 1, 'x');
  expect(manager->set_text(punto::Selection::Clipboard, too_large) ==
             punto::ClipboardResult::ConversionFailed,
         "payload requiring INCR is rejected before ownership change");
}

void test_restore_never_reclaims_external_ownership() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "temporary") ==
             punto::ClipboardResult::Ok,
         "own temporary clipboard before external takeover");
  const std::uint64_t generation =
      manager->selection_generation(punto::Selection::Clipboard);

  Connection external = connect_to();
  const xcb_window_t external_window =
      create_window(external.value, external.screen);
  const xcb_atom_t clipboard = intern_atom(external.value, "CLIPBOARD");
  auto takeover_error = xcb_ptr(xcb_request_check(
      external.value,
      xcb_set_selection_owner_checked(external.value, external_window,
                                      clipboard, XCB_CURRENT_TIME)));
  expect(!takeover_error, "external client takes clipboard ownership");

  expect(!manager->restore_text_if_generation(punto::Selection::Clipboard,
                                              generation, "original"),
         "restore rejects an external ownership change");

  xcb_generic_error_t *raw_error = nullptr;
  auto owner = xcb_ptr(xcb_get_selection_owner_reply(
      external.value, xcb_get_selection_owner(external.value, clipboard),
      &raw_error));
  auto error = xcb_ptr(raw_error);
  expect(!error && owner && owner->owner == external_window,
         "failed restore does not reclaim the external selection");
  xcb_disconnect(external.value);
}

void test_restore_publication_boundary(publication_fault::Stage stage) {
  Connection owner = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      owner.value, owner.screen, std::chrono::milliseconds{50});
  expect(manager->set_text(punto::Selection::Clipboard, "temporary") ==
             punto::ClipboardResult::Ok,
         "publish temporary payload before boundary control");
  const auto generation =
      manager->selection_generation(punto::Selection::Clipboard);
  const auto identity = manager->get_selection_identity(
      punto::Selection::Clipboard,
      std::chrono::steady_clock::now() + std::chrono::milliseconds{50});
  expect(identity.has_value(), "capture identity before restore boundary");
  Connection peer = connect_to();
  const auto peer_window = create_window(peer.value, peer.screen);
  publication_fault::connection = owner.value;
  publication_fault::window =
      punto::ClipboardManagerTestAccess::window(*manager);
  publication_fault::selection_event =
      punto::ClipboardManagerTestAccess::selection_event(*manager);
  publication_fault::peer = peer.value;
  publication_fault::peer_window = peer_window;
  publication_fault::clipboard = intern_atom(peer.value, "CLIPBOARD");
  publication_fault::utf8 = intern_atom(peer.value, "UTF8_STRING");
  publication_fault::property =
      intern_atom(peer.value, "PUNTO_RESTORE_BOUNDARY_READ");
  publication_fault::timestamp_property =
      intern_atom(peer.value, "PUNTO_SERVER_TIME");
  publication_fault::previous_timestamp = identity->selection_timestamp;
  publication_fault::consumed = false;
  const auto queued_property =
      intern_atom(peer.value, "PUNTO_RESTORE_QUEUED_READ");
  if (stage == publication_fault::Stage::BoundaryRead) {
    auto queued_error = xcb_ptr(xcb_request_check(
        peer.value,
        xcb_convert_selection_checked(
            peer.value, peer_window, publication_fault::clipboard,
            publication_fault::utf8, queued_property, XCB_CURRENT_TIME)));
    auto barrier = xcb_ptr(xcb_get_input_focus_reply(
        owner.value, xcb_get_input_focus(owner.value), nullptr));
    expect(!queued_error && barrier,
           "real queued read precedes the restore timestamp without pumping");
  }
  publication_fault::stage = stage;
  const bool restored = manager->restore_text_if_generation(
      punto::Selection::Clipboard, generation, "baseline");
  publication_fault::stage = publication_fault::Stage::None;
  publication_fault::connection = nullptr;
  if (stage == publication_fault::Stage::BoundaryRead) {
    const auto queued_notify = await_selection_notify(peer.value, *manager);
    expect(queued_notify.requestor == peer_window &&
               queued_notify.selection == publication_fault::clipboard &&
               queued_notify.target == publication_fault::utf8 &&
               queued_notify.property == queued_property,
           "queued prior read receives its actual UTF8 notification");
    expect(read_property(peer.value, peer_window, queued_property,
                         publication_fault::utf8) == "temporary",
           "queued prior read receives exact temporary bytes before restore");
  }
  expect(publication_fault::consumed,
         "real timestamp or publication boundary was reached");
  if (stage == publication_fault::Stage::BoundaryRead) {
    expect(restored, "restore succeeds before serving its new identity");
    const auto boundary_notify = await_selection_notify(peer.value, *manager);
    expect(boundary_notify.requestor == peer_window &&
               boundary_notify.selection == publication_fault::clipboard &&
               boundary_notify.target == publication_fault::utf8 &&
               boundary_notify.property == publication_fault::property,
           "new publication read receives its distinct UTF8 notification");
    expect(read_property(peer.value, peer_window, publication_fault::property,
                         publication_fault::utf8) == "baseline",
           "request at owner notification reads new baseline, not old "
           "replacement");
  } else {
    expect(!restored,
           "failed timestamp or foreign owner cannot publish restoration");
    expect(manager->selection_generation(punto::Selection::Clipboard) ==
               generation,
           "rejected restoration does not advance payload generation");
    if (stage == publication_fault::Stage::TimestampTimeout) {
      expect(manager->get_text(punto::Selection::Clipboard) ==
                 std::optional<std::string>{"temporary"},
             "timestamp timeout keeps the owned payload unchanged");
    } else {
      auto reply = xcb_ptr(xcb_get_selection_owner_reply(
          peer.value,
          xcb_get_selection_owner(peer.value, publication_fault::clipboard),
          nullptr));
      expect(reply && reply->owner == peer_window,
             "takeover during timestamp acquisition remains owned by foreign "
             "client");
      expect(manager->set_text(punto::Selection::Clipboard,
                               "temporary-again") == punto::ClipboardResult::Ok,
             "publish predecessor before undrained owner loss");
      auto takeover_error = xcb_ptr(xcb_request_check(
          peer.value, xcb_set_selection_owner_checked(
                          peer.value, peer_window, publication_fault::clipboard,
                          XCB_CURRENT_TIME)));
      auto taken_owner = xcb_ptr(xcb_get_selection_owner_reply(
          peer.value,
          xcb_get_selection_owner(peer.value, publication_fault::clipboard),
          nullptr));
      expect(!takeover_error && taken_owner &&
                 taken_owner->owner == peer_window,
             "peer takeover precedes reacquisition without manager pumping");
    }
  }
  expect(manager->set_text(punto::Selection::Clipboard, "fresh") ==
             punto::ClipboardResult::Ok,
         "new operation recovers on the same manager after a restore boundary");
  const auto notify = request_selection(
      peer.value, peer_window, *manager, publication_fault::clipboard,
      publication_fault::utf8, publication_fault::property);
  expect(notify.property == publication_fault::property &&
             read_property(peer.value, peer_window, publication_fault::property,
                           publication_fault::utf8) == "fresh",
         "peer reads fresh recovery payload without rebuilding the manager");
  expect(manager->locally_owns_generation(
             punto::Selection::Clipboard,
             manager->selection_generation(punto::Selection::Clipboard)),
         "successful recovery retains coherent local publication state");
  xcb_disconnect(peer.value);
}

void test_publication_rejects_obsolete_authority() {
  using Access = punto::ClipboardManagerTestAccess;
  const auto last = std::numeric_limits<std::uint32_t>::max();
  expect(
      Access::event_precedes_publication(2, 1) &&
          !Access::event_precedes_publication(2, 2) &&
          !Access::event_precedes_publication(2, 3) &&
          Access::event_precedes_publication(1, last) &&
          !Access::event_precedes_publication(last, 1) &&
          Access::event_precedes_publication(0, std::uint32_t{1} << 31),
      "publication serial qualifies equality, wrap and ambiguous half range");
  Connection owner = connect_to();
  auto manager =
      Access::make(owner.value, owner.screen, std::chrono::milliseconds{50});
  expect(manager->set_text(punto::Selection::Clipboard, "predecessor") ==
             punto::ClipboardResult::Ok,
         "publish predecessor for queued conversion");
  const auto source = manager->get_text_with_owner(punto::Selection::Clipboard);
  expect(source.has_value(), "capture predecessor owner identity");
  Connection peer = connect_to();
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const auto property = intern_atom(peer.value, "PUNTO_OBSOLETE_CONVERSION");
  set_active_window(owner.value, owner.screen, window);
  const auto previous_receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard);
  const auto previous_transition = manager->arm_owner_transition(
      punto::Selection::Clipboard, *source,
      Access::resource_client_id(*manager, source->owner));
  expect(previous_receipt && previous_transition,
         "arm predecessor receipt and owner-transition authority");
  const auto previous_requests =
      manager->selection_request_seq(punto::Selection::Clipboard);
  auto conversion_error = xcb_ptr(xcb_request_check(
      peer.value,
      xcb_convert_selection_checked(peer.value, window, clipboard, utf8,
                                    property, XCB_CURRENT_TIME)));
  expect(!conversion_error, "queue real conversion before new publication");
  expect(manager->set_text(punto::Selection::Clipboard, "fresh") ==
             punto::ClipboardResult::Ok,
         "new publication consumes queued obsolete history within its budget");
  const auto obsolete = await_selection_notify(peer.value, *manager);
  auto untouched = xcb_ptr(
      xcb_get_property_reply(peer.value,
                             xcb_get_property(peer.value, 0, window, property,
                                              XCB_GET_PROPERTY_TYPE_ANY, 0, 1),
                             nullptr));
  expect(obsolete.property == XCB_ATOM_NONE && untouched &&
             untouched->type == XCB_ATOM_NONE && untouched->format == 0 &&
             xcb_get_property_value_length(untouched.get()) == 0 &&
             manager->selection_request_seq(punto::Selection::Clipboard) ==
                 previous_requests,
         "obsolete conversion cannot write fresh data or advance receipts");
  expect(manager->paste_receipt_status(*previous_receipt) ==
                 punto::PasteReceiptStatus::Invalid &&
             manager->owner_transition_status(*previous_transition) ==
                 punto::OwnerTransitionStatus::Invalid &&
             !manager->restore_selection_after_owner_transition(
                 *previous_transition, *source),
         "new publication invalidates both forms of predecessor authority");
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "arm receipt after obsolete conversion refusal");
  const auto fresh = request_selection(peer.value, window, *manager, clipboard,
                                       utf8, property);
  expect(fresh.property == property &&
             read_property(peer.value, window, property, utf8) == "fresh" &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Delivered,
         "fresh conversion delivers exact bytes without rebuilding manager");
  const auto current =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(current.has_value(), "capture fresh owner for new transition");
  const auto transition = manager->arm_owner_transition(
      punto::Selection::Clipboard, *current,
      Access::resource_client_id(*manager, current->owner));
  const auto successor = create_window(owner.value, owner.screen);
  auto takeover_error = xcb_ptr(xcb_request_check(
      owner.value, xcb_set_selection_owner_checked(
                       owner.value, successor, clipboard, XCB_CURRENT_TIME)));
  expect(transition && !takeover_error,
         "new transition observes genuine post-publication owner loss");
  (void)manager->pump_events();
  expect(
      manager->owner_transition_status(*transition) ==
              punto::OwnerTransitionStatus::SameClientOwner &&
          manager->restore_selection_after_owner_transition(*transition,
                                                            *source) &&
          manager->get_text(punto::Selection::Clipboard) == "predecessor",
      "new explicit transition remains usable after stale authority retires");
  xcb_disconnect(peer.value);
}

void test_xfixes_timestamp_fences_same_owner_new_copy() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection external = connect_to();
  const xcb_window_t external_window =
      create_window(external.value, external.screen);
  const xcb_atom_t clipboard = intern_atom(external.value, "CLIPBOARD");

  auto own = [&] {
    auto error = xcb_ptr(xcb_request_check(
        external.value,
        xcb_set_selection_owner_checked(external.value, external_window,
                                        clipboard, XCB_CURRENT_TIME)));
    expect(!error, "external clipboard owner update");
  };
  own();
  std::thread first_server{
      [&] { serve_one_text_request(external.value, "first"); }};
  auto first = manager->get_text_with_owner(punto::Selection::Clipboard);
  first_server.join();
  expect(first && first->text == "first" &&
             first->selection_timestamp != XCB_CURRENT_TIME,
         "first copy has an XFixes timestamp fence");

  auto reassert_error = xcb_ptr(xcb_request_check(
      external.value,
      xcb_set_selection_owner_checked(external.value, external_window,
                                      clipboard, first->selection_timestamp)));
  expect(!reassert_error, "actual same-window exact-timestamp reassertion");
  pump_until(
      *manager,
      [&] {
        return manager->selection_owner_generation(
                   punto::Selection::Clipboard) != first->owner_generation;
      },
      "observe real equal-timestamp owner-generation change");
  expect(!manager->set_text_if_owner(punto::Selection::Clipboard, *first,
                                     "stale equal-timestamp replacement"),
         "equal timestamp cannot rehabilitate a stale generation");
  std::thread reassert_server{
      [&] { serve_one_text_request(external.value, "reasserted"); }};
  const auto reasserted =
      manager->get_text_with_owner(punto::Selection::Clipboard);
  reassert_server.join();
  expect(reasserted && reasserted->owner == first->owner &&
             reasserted->selection_timestamp == first->selection_timestamp &&
             reasserted->owner_generation != first->owner_generation &&
             reasserted->text == "reasserted",
         "rejected publication preserves exact foreign owner and payload");

  const auto other_window = create_window(external.value, external.screen);
  for (const auto window : {other_window, external_window}) {
    auto error = xcb_ptr(xcb_request_check(
        external.value,
        xcb_set_selection_owner_checked(external.value, window, clipboard,
                                        reasserted->selection_timestamp)));
    expect(!error, "actual A-B-A transition with identical timestamp");
  }
  expect(!manager->set_text_if_owner(punto::Selection::Clipboard, *reasserted,
                                     "stale A-B-A replacement"),
         "equal owner and timestamp do not erase intervening ownership");

  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  own();
  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  (void)manager->pump_events();
  expect(!manager->set_text_if_owner(punto::Selection::Clipboard, *first,
                                     "stale restore"),
         "same-owner newer copy rejects stale restore");

  xcb_generic_error_t *raw_error = nullptr;
  auto current = xcb_ptr(xcb_get_selection_owner_reply(
      external.value, xcb_get_selection_owner(external.value, clipboard),
      &raw_error));
  auto error = xcb_ptr(raw_error);
  expect(!error && current && current->owner == external_window,
         "stale restore does not reclaim newer same-window clipboard");

  std::thread second_server{
      [&] { serve_one_text_request(external.value, "second"); }};
  auto second = manager->get_text_with_owner(punto::Selection::Clipboard);
  second_server.join();
  expect(second && second->text == "second", "read the newer same-owner copy");
  expect(manager->set_text_if_owner(punto::Selection::Clipboard, *second,
                                    "replacement"),
         "current timestamp may atomically replace external clipboard");
  const auto replacement = manager->get_text(punto::Selection::Clipboard);
  expect(replacement && *replacement == "replacement",
         "timestamp-fenced replacement owns exact payload");
  xcb_disconnect(external.value);
}

void test_conditional_publication_from_absent_owner() {
  Connection connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(connection.value,
                                                         connection.screen);
  Connection peer = connect_to();
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto absent =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(absent && absent->owner == XCB_WINDOW_NONE,
         "actual initially absent clipboard baseline");
  auto error = xcb_ptr(xcb_request_check(
      peer.value, xcb_set_selection_owner_checked(peer.value, window, clipboard,
                                                  XCB_CURRENT_TIME)));
  expect(!error, "foreign copy after the absent snapshot");
  expect(!manager->set_text_if_owner(punto::Selection::Clipboard, *absent,
                                     "stale absent publication"),
         "absent snapshot cannot overwrite a new foreign clipboard");
  std::thread provider{[&] { serve_one_text_request(peer.value, "foreign"); }};
  const auto foreign = manager->get_text(punto::Selection::Clipboard);
  provider.join();
  expect(foreign == std::optional<std::string>{"foreign"},
         "foreign owner and exact payload remain usable after rejection");
  error =
      xcb_ptr(xcb_request_check(peer.value, xcb_set_selection_owner_checked(
                                                peer.value, XCB_WINDOW_NONE,
                                                clipboard, XCB_CURRENT_TIME)));
  expect(!error &&
             !manager->set_text_if_owner(punto::Selection::Clipboard, *absent,
                                         "stale absent ABA publication"),
         "NONE-foreign-NONE cannot revive the old absent authority");
  const auto fresh =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(fresh && fresh->owner == XCB_WINDOW_NONE &&
             manager->set_text_if_owner(punto::Selection::Clipboard, *fresh,
                                        "fresh"),
         "fresh absent authority publishes normally without restart");
  const auto current =
      manager->get_selection_identity(punto::Selection::Clipboard);
  publication_guard_fault::reset(connection.value);
  expect(current &&
             !manager->set_text_if_owner(punto::Selection::Clipboard, *current,
                                         "expired",
                                         std::chrono::steady_clock::now() -
                                             std::chrono::milliseconds{1}) &&
             publication_guard_fault::grabs == 0 &&
             publication_guard_fault::writes == 0 &&
             manager->get_text(punto::Selection::Clipboard) ==
                 std::optional<std::string>{"fresh"},
         "expired caller deadline neither grabs nor writes or renews");
  publication_guard_fault::reset();
  xcb_disconnect(peer.value);
}

void test_conditional_publication_preserves_queued_foreign_copy() {
  Connection connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(connection.value,
                                                         connection.screen);
  Connection peer = connect_to();
  Connection reader = connect_to();
  auto observer =
      punto::ClipboardManagerTestAccess::make(reader.value, reader.screen);
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  expect(manager->set_text(punto::Selection::Clipboard, "temporary") ==
             punto::ClipboardResult::Ok,
         "publish queued-copy baseline");
  const auto original =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(original.has_value(), "capture exact pre-copy authority");
  publication_guard_fault::reset(connection.value);
  publication_guard_fault::stage = publication_guard_fault::Stage::PeerCopy;
  publication_guard_fault::peer = peer.value;
  publication_guard_fault::peer_window = window;
  publication_guard_fault::selection = clipboard;
  publication_guard_fault::timestamp = original->selection_timestamp;
  (void)manager->set_text_if_owner(punto::Selection::Clipboard, *original,
                                   "replacement");
  const bool queued_under_guard =
      publication_guard_fault::consumed &&
      publication_guard_fault::peer_queued_while_held;
  auto error = xcb_ptr(
      xcb_request_check(peer.value, publication_guard_fault::peer_copy));
  auto owner = xcb_ptr(xcb_get_selection_owner_reply(
      peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
  expect(
      queued_under_guard && !error && owner && owner->owner == window &&
          publication_guard_fault::grabs == 1 &&
          publication_guard_fault::releases == 1 &&
          publication_guard_fault::writes == 1 &&
          publication_guard_fault::writes_while_held == 1,
      "real copy submitted between guard query and write wins after release");
  publication_guard_fault::reset();
  std::thread provider{[&] { serve_one_text_request(peer.value, "new copy"); }};
  const auto payload = observer->get_text(punto::Selection::Clipboard);
  provider.join();
  expect(payload == std::optional<std::string>{"new copy"},
         "queued foreign copy retains its exact usable payload");
  (void)manager->pump_events();
  expect(!manager->restore_text_if_generation(
             punto::Selection::Clipboard,
             manager->selection_generation(punto::Selection::Clipboard),
             "late baseline"),
         "late local restoration cannot reclaim the queued user copy");
  xcb_disconnect(peer.value);
}

void test_conditional_publication_readmission(
    unsigned int request_count, publication_guard_fault::Stage handoff,
    bool absent_clipboard = false) {
  using namespace std::chrono_literals;
  Connection connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(connection.value,
                                                         connection.screen);
  Connection observer = connect_to();
  punto::ClipboardManagerTestAccess::attach_witness(
      *manager,
      punto::ClipboardManagerTestAccess::make(observer.value, observer.screen));
  Connection peer = connect_to();
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const std::array<xcb_atom_t, 2> properties{
      intern_atom(peer.value, "PUNTO_GUARD_RESPONSE"),
      intern_atom(peer.value, "PUNTO_GUARD_RESPONSE_SECOND")};
  const auto baseline_selection = absent_clipboard
                                      ? punto::Selection::Primary
                                      : punto::Selection::Clipboard;
  const xcb_atom_t request_selection =
      absent_clipboard ? static_cast<xcb_atom_t>(XCB_ATOM_PRIMARY) : clipboard;
  expect(manager->set_text(baseline_selection, "temporary") ==
             punto::ClipboardResult::Ok,
         "publish queued-request baseline");
  const auto original =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(original.has_value(), "capture authority before real queued request");
  expect(!absent_clipboard || original->owner == XCB_WINDOW_NONE,
         "timestamp preparation starts with an actually absent CLIPBOARD");
  const auto baseline_identity =
      manager->get_selection_identity(baseline_selection);
  expect(baseline_identity.has_value(), "capture the exact peer-read baseline");
  const auto original_generation =
      manager->selection_generation(baseline_selection);
  for (unsigned int i = 0; i < request_count; ++i) {
    auto error = xcb_ptr(xcb_request_check(
        peer.value, xcb_convert_selection_checked(
                        peer.value, window, request_selection, utf8,
                        properties[i % properties.size()], XCB_CURRENT_TIME)));
    expect(!error,
           "real queued SelectionRequest before conditional publication");
  }
  auto queued = xcb_ptr(xcb_get_input_focus_reply(
      peer.value, xcb_get_input_focus(peer.value), nullptr));
  expect(queued != nullptr,
         "server has accepted the complete peer read prefix");
  publication_guard_fault::reset(connection.value);
  publication_guard_fault::stage = handoff;
  publication_guard_fault::peer = peer.value;
  publication_guard_fault::peer_window = window;
  publication_guard_fault::original_owner = original->owner;
  publication_guard_fault::selection = clipboard;
  publication_guard_fault::timestamp = original->selection_timestamp;
  publication_guard_fault::payload_property = properties[0];
  publication_guard_fault::second_payload_property = properties[1];
  const auto start = std::chrono::steady_clock::now();
  const auto publication = manager->set_payload_if_owner(
      punto::Selection::Clipboard, *original, manager->text_payload("fresh"),
      start + 100ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const bool unchanged = handoff == publication_guard_fault::Stage::None;
  const bool complete_prefix = request_count < 64;
  if (unchanged && complete_prefix) {
    expect(publication.submitted && publication.completed && elapsed < 150ms,
           "bounded old-generation reads admit one raw publication in the same "
           "call and original deadline");
    expect(
        manager->is_open() &&
            publication_guard_fault::grabs >= (absent_clipboard ? 1U : 2U) &&
            publication_guard_fault::releases ==
                publication_guard_fault::grabs &&
            publication_guard_fault::writes == 1 &&
            publication_guard_fault::writes_while_held == 1 &&
            publication_guard_fault::responses == request_count &&
            publication_guard_fault::responses_while_held == 0,
        "service all exact peer reads outside grab before one guarded write");
    for (unsigned int i = 0; i < request_count; ++i) {
      const auto notify = await_selection_notify(peer.value, *manager);
      expect(notify.requestor == window &&
                 notify.selection == request_selection &&
                 notify.target == utf8 && notify.property == properties[i] &&
                 read_property(peer.value, window, properties[i], utf8) ==
                     "temporary",
             "every queued peer receives the old payload, not the new value");
    }
    expect(manager->get_text(punto::Selection::Clipboard) == "fresh" &&
               manager->locally_owns_generation(punto::Selection::Clipboard,
                                                publication.generation),
           "raw publication commits its exact new payload and generation");
    if (absent_clipboard) {
      const auto after =
          manager->get_selection_identity(punto::Selection::Primary);
      expect(after && after->owner == baseline_identity->owner &&
                 after->selection_timestamp ==
                     baseline_identity->selection_timestamp &&
                 after->owner_generation ==
                     baseline_identity->owner_generation &&
                 after->connection_generation ==
                     baseline_identity->connection_generation &&
                 manager->selection_generation(punto::Selection::Primary) ==
                     original_generation &&
                 manager->locally_owns_generation(punto::Selection::Primary,
                                                  original_generation),
             "timestamp preparation preserves the full owned PRIMARY identity "
             "and payload generation");
    }
  } else {
    expect(!publication.submitted && !publication.completed &&
               manager->is_open() && elapsed < 150ms &&
               publication_guard_fault::writes == 0 &&
               publication_guard_fault::responses_while_held == 0 &&
               publication_guard_fault::responses <= 64 &&
               publication_guard_fault::releases ==
                   publication_guard_fault::grabs,
           "owner drift or incomplete aggregate history refuses without a "
           "publication write or renewed event budget");
    if (unchanged) {
      expect(manager->locally_owns_generation(punto::Selection::Clipboard,
                                              original_generation) &&
                 manager->selection_generation(punto::Selection::Clipboard) ==
                     original_generation,
             "budget refusal retains the old owned payload");
    } else {
      auto owner = xcb_ptr(xcb_get_selection_owner_reply(
          peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
      expect(publication_guard_fault::consumed && owner &&
                 owner->owner ==
                     (handoff == publication_guard_fault::Stage::ReleaseABA
                          ? original->owner
                          : window) &&
                 !manager->selection_identity_matches(
                     punto::Selection::Clipboard, *original),
             "checked ungrab handoff preserves foreign ownership or rejects "
             "the equal-timestamp ABA identity");
    }
  }
  manager->cancel_publication_witness(publication);
  publication_guard_fault::reset();
  xcb_disconnect(peer.value);
}

void test_conditional_publication_releases_before_serving_request() {
  test_conditional_publication_readmission(
      2, publication_guard_fault::Stage::None);
}

void test_conditional_publication_readmission_refusals() {
  for (const auto stage : {publication_guard_fault::Stage::ReleaseCopy,
                           publication_guard_fault::Stage::ReleaseABA})
    test_conditional_publication_readmission(1, stage);
  test_conditional_publication_readmission(
      160, publication_guard_fault::Stage::None);
}

void test_conditional_publication_timestamp_services_other_selection() {
  test_conditional_publication_readmission(
      2, publication_guard_fault::Stage::None, true);
}

void test_rich_clipboard_is_detected_before_mutation() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection external = connect_to();
  const xcb_window_t external_window =
      create_window(external.value, external.screen);
  const xcb_atom_t clipboard = intern_atom(external.value, "CLIPBOARD");
  const xcb_atom_t targets = intern_atom(external.value, "TARGETS");
  const xcb_atom_t utf8 = intern_atom(external.value, "UTF8_STRING");
  const xcb_atom_t html = intern_atom(external.value, "text/html");

  const auto own = [&] {
    auto error = xcb_ptr(xcb_request_check(
        external.value,
        xcb_set_selection_owner_checked(external.value, external_window,
                                        clipboard, XCB_CURRENT_TIME)));
    expect(!error, "external clipboard owner update");
  };

  own();
  std::thread text_server{[&] {
    serve_text_and_targets(external.value, "plain", {targets, utf8});
  }};
  const auto plain = manager->get_text_with_owner(punto::Selection::Clipboard);
  expect(plain && plain->text == "plain", "read plain clipboard snapshot");
  const auto plain_only =
      manager->has_only_text_targets(punto::Selection::Clipboard, *plain);
  text_server.join();
  expect(plain_only && *plain_only,
         "plain clipboard targets are safe to restore as text");

  own();
  std::thread rich_server{[&] {
    serve_text_and_targets(external.value, "visible", {targets, utf8, html});
  }};
  const auto rich = manager->get_text_with_owner(punto::Selection::Clipboard);
  expect(rich && rich->text == "visible", "read rich clipboard text view");
  const auto rich_only =
      manager->has_only_text_targets(punto::Selection::Clipboard, *rich);
  rich_server.join();
  expect(rich_only && !*rich_only,
         "custom rich target is rejected before clipboard mutation");

  xcb_disconnect(external.value);
}

void test_primary_restore_after_exact_client_release() {
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  Connection source = connect_to();
  const xcb_window_t source_window = create_window(source.value, source.screen);
  const xcb_atom_t primary = intern_atom(source.value, "PRIMARY");
  set_active_window(source.value, source.screen, source_window);

  const auto previous = manager->get_text_with_owner(punto::Selection::Primary);
  expect(previous && previous->owner == XCB_WINDOW_NONE,
         "capture initially absent PRIMARY");

  auto own_source =
      xcb_ptr(xcb_request_check(source.value, xcb_set_selection_owner_checked(
                                                  source.value, source_window,
                                                  primary, XCB_CURRENT_TIME)));
  expect(!own_source, "source client owns PRIMARY");
  std::thread source_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  source_server.join();
  expect(selected && selected->text == "selected",
         "capture source PRIMARY with owner fence");
  const std::uint32_t source_client =
      punto::ClipboardManagerTestAccess::resource_client_id(*manager,
                                                            source_window);
  expect(source_client != 0, "derive source client identity");

  auto release =
      xcb_ptr(xcb_request_check(source.value, xcb_set_selection_owner_checked(
                                                  source.value, XCB_WINDOW_NONE,
                                                  primary, XCB_CURRENT_TIME)));
  expect(!release, "source client releases PRIMARY after paste");
  const auto absent = manager->get_text_with_owner(punto::Selection::Primary);
  expect(absent && absent->owner == XCB_WINDOW_NONE,
         "observe exact PRIMARY release");
  expect(manager->restore_selection_after_client_release(
             punto::Selection::Primary, *selected, *previous, source_client),
         "exact source release restores an absent previous PRIMARY");

  Connection previous_owner = connect_to();
  const xcb_window_t previous_window =
      create_window(previous_owner.value, previous_owner.screen);
  auto previous_own = xcb_ptr(xcb_request_check(
      previous_owner.value,
      xcb_set_selection_owner_checked(previous_owner.value, previous_window,
                                      primary, XCB_CURRENT_TIME)));
  expect(!previous_own, "previous client owns rich restore text fixture");
  std::thread previous_server{
      [&] { serve_one_text_request(previous_owner.value, "saved primary"); }};
  const auto saved = manager->get_text_with_owner(punto::Selection::Primary);
  previous_server.join();
  expect(saved && saved->text == "saved primary", "snapshot previous PRIMARY");

  own_source =
      xcb_ptr(xcb_request_check(source.value, xcb_set_selection_owner_checked(
                                                  source.value, source_window,
                                                  primary, XCB_CURRENT_TIME)));
  expect(!own_source, "source owns replacement range");
  std::thread second_source_server{
      [&] { serve_one_text_request(source.value, "second selection"); }};
  const auto second_selected =
      manager->get_text_with_owner(punto::Selection::Primary);
  second_source_server.join();
  expect(second_selected && second_selected->text == "second selection",
         "snapshot second source range");
  release =
      xcb_ptr(xcb_request_check(source.value, xcb_set_selection_owner_checked(
                                                  source.value, XCB_WINDOW_NONE,
                                                  primary, XCB_CURRENT_TIME)));
  expect(!release, "release second source range");
  (void)manager->get_text_with_owner(punto::Selection::Primary);
  expect(
      manager->restore_selection_after_client_release(
          punto::Selection::Primary, *second_selected, *saved, source_client),
      "exact release restores saved PRIMARY text");
  const auto restored = manager->get_text(punto::Selection::Primary);
  expect(restored && *restored == "saved primary",
         "restored PRIMARY owns the exact saved text");

  xcb_disconnect(previous_owner.value);
  xcb_disconnect(source.value);
}

void test_active_window_kind_is_tristate_and_boundary_safe() {
  Connection window_client = connect_to();
  const xcb_atom_t active =
      intern_atom(window_client.value, "_NET_ACTIVE_WINDOW");
  Connection manager_connection = connect_to();
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen);
  expect(manager->active_window_kind() == punto::ActiveWindowKind::Unknown,
         "missing active-window property is unknown");

  const xcb_window_t window =
      create_window(window_client.value, window_client.screen);
  xcb_screen_t *screen = screen_at(window_client.value, window_client.screen);
  auto property_error = xcb_ptr(xcb_request_check(
      window_client.value,
      xcb_change_property_checked(window_client.value, XCB_PROP_MODE_REPLACE,
                                  screen->root, active, XCB_ATOM_WINDOW, 32, 1,
                                  &window)));
  expect(!property_error, "publish active window property");
  const std::string unknown_class{"postman\0Postman\0", 16};
  property_error = xcb_ptr(xcb_request_check(
      window_client.value,
      xcb_change_property_checked(
          window_client.value, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
          XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(unknown_class.size()),
          unknown_class.data())));
  expect(!property_error, "publish unknown active window");
  expect(manager->active_window_kind() == punto::ActiveWindowKind::Gui,
         "well-formed nonterminal class uses guarded GUI execution");

  for (const auto &malformed :
       {std::string{"postman"}, std::string{"\0Postman\0", 9},
        std::string{"postman\0Postman", 15}}) {
    property_error = xcb_ptr(xcb_request_check(
        window_client.value,
        xcb_change_property_checked(
            window_client.value, XCB_PROP_MODE_REPLACE, window,
            XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
            static_cast<std::uint32_t>(malformed.size()), malformed.data())));
    expect(!property_error, "publish malformed class fixture");
    expect(manager->active_window_kind() == punto::ActiveWindowKind::Unknown,
           "malformed or empty class component stays unknown");
  }

  const std::string gui_class{"gedit\0Gedit\0", 12};
  property_error = xcb_ptr(xcb_request_check(
      window_client.value,
      xcb_change_property_checked(window_client.value, XCB_PROP_MODE_REPLACE,
                                  window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                                  static_cast<std::uint32_t>(gui_class.size()),
                                  gui_class.data())));
  expect(!property_error, "publish allowlisted GUI active window");
  expect(manager->active_window_kind() == punto::ActiveWindowKind::Gui,
         "allowlisted gedit window is GUI-safe");

  const std::string terminal_class{"st\0St\0", 6};
  property_error = xcb_ptr(xcb_request_check(
      window_client.value,
      xcb_change_property_checked(
          window_client.value, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
          XCB_ATOM_STRING, 8, static_cast<std::uint32_t>(terminal_class.size()),
          terminal_class.data())));
  expect(!property_error, "publish terminal active window");
  expect(manager->active_window_kind() == punto::ActiveWindowKind::Terminal,
         "exact st component is a terminal window");
  xcb_disconnect(window_client.value);
}

void test_pump_has_a_hard_event_budget() {
  Connection owner = connect_to();
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "payload") ==
             punto::ClipboardResult::Ok,
         "own payload for pump budget test");

  Connection requester = connect_to();
  const xcb_window_t window = create_window(requester.value, requester.screen);
  const xcb_atom_t clipboard = intern_atom(requester.value, "CLIPBOARD");
  const xcb_atom_t targets = intern_atom(requester.value, "TARGETS");
  const xcb_atom_t property = intern_atom(requester.value, "PUNTO_TEST_BUDGET");
  for (std::size_t i = 0; i < 256; ++i) {
    xcb_convert_selection(requester.value, window, clipboard, targets, property,
                          XCB_CURRENT_TIME);
  }
  expect(xcb_flush(requester.value) > 0, "flush request flood");
  std::this_thread::sleep_for(std::chrono::milliseconds{20});

  const std::size_t processed = manager->pump_events();
  expect(processed > 0, "pump consumes ready events");
  expect(processed <= 64, "one pump call cannot starve the main loop");
  xcb_disconnect(requester.value);
}

void test_context_bound_concurrent_requests_deliver_to_both_clients() {
  // Two requests leave one for the final identity fence: the preliminary pump
  // can serve only one. Peer replies are collected without another manager
  // pump.
  for (const bool replace_primary : {true, false}) {
    Connection publisher = connect_to();
    auto candidate = punto::ClipboardManagerTestAccess::make(publisher.value,
                                                             publisher.screen);
    Connection editor = connect_to();
    const auto editor_window = create_window(editor.value, editor.screen);
    const std::array<Connection, 2> readers{connect_to(), connect_to()};
    const std::array<xcb_window_t, 2> reader_windows{
        create_window(readers[0].value, readers[0].screen),
        create_window(readers[1].value, readers[1].screen)};
    const auto clipboard_atom = intern_atom(editor.value, "CLIPBOARD");
    const auto utf8_atom = intern_atom(editor.value, "UTF8_STRING");
    const std::array<xcb_atom_t, 2> properties{
        intern_atom(editor.value, "PUNTO_PREARM_FIRST_READER"),
        intern_atom(editor.value, "PUNTO_PREARM_SECOND_READER")};
    auto primary_error = xcb_ptr(xcb_request_check(
        editor.value,
        xcb_set_selection_owner_checked(editor.value, editor_window,
                                        XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
    expect(!primary_error, "publish actual pre-arm PRIMARY source");
    set_active_window(editor.value, editor.screen, editor_window);
    expect(candidate->set_text(punto::Selection::Clipboard,
                               "pre-arm payload") == punto::ClipboardResult::Ok,
           "publish owned pre-arm clipboard payload");
    const auto original =
        candidate->get_selection_identity(punto::Selection::Primary);
    expect(original && original->owner == editor_window,
           "capture exact original PRIMARY before queued readers");
    const auto generation =
        candidate->selection_generation(punto::Selection::Clipboard);
    const auto request_sequence =
        candidate->selection_request_seq(punto::Selection::Clipboard);
    if (replace_primary) {
      auto replacement_error = xcb_ptr(xcb_request_check(
          readers[0].value,
          xcb_set_selection_owner_checked(readers[0].value, reader_windows[0],
                                          XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
      expect(!replacement_error, "queue actual foreign PRIMARY replacement");
    }
    for (std::size_t index = 0; index < readers.size(); ++index) {
      xcb_convert_selection(readers[index].value, reader_windows[index],
                            clipboard_atom, utf8_atom, properties[index],
                            XCB_CURRENT_TIME);
      auto barrier = xcb_ptr(xcb_get_input_focus_reply(
          readers[index].value, xcb_get_input_focus(readers[index].value),
          nullptr));
      expect(barrier != nullptr,
             "each ordinary pre-arm conversion reaches the actual X server");
    }
    const auto started = std::chrono::steady_clock::now();
    const auto armed =
        candidate->arm_paste_receipt(punto::Selection::Clipboard, *original);
    const auto serviced =
        candidate->selection_request_seq(punto::Selection::Clipboard) -
        request_sequence;
    std::cerr << "[punto-contract] prearm_peer_fence replaced_primary="
              << replace_primary << " request_delta=" << serviced
              << " receipt=" << armed.has_value() << '\n';
    expect(
        serviced == 2,
        "arm itself serves both real requests across the final identity fence");
    expect(std::chrono::steady_clock::now() - started <
               std::chrono::milliseconds{500},
           "pre-arm request service retains the original operation budget");
    std::array<std::size_t, 2> responses{};
    for (std::size_t index = 0; index < readers.size(); ++index) {
      while (auto event = xcb_ptr(xcb_poll_for_event(readers[index].value))) {
        if ((event->response_type & 0x7fU) != XCB_SELECTION_NOTIFY)
          continue;
        const auto &notify =
            *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
        ++responses[index];
        expect(notify.requestor == reader_windows[index] &&
                   notify.selection == clipboard_atom &&
                   notify.target == utf8_atom &&
                   notify.property == properties[index] &&
                   read_property(readers[index].value, reader_windows[index],
                                 properties[index],
                                 utf8_atom) == "pre-arm payload",
               "ordinary pre-arm reader receives exact real payload and reply");
      }
    }
    expect(responses == std::array<std::size_t, 2>{1, 1},
           "both real readers complete once without another manager pump");
    expect(candidate->selection_generation(punto::Selection::Clipboard) ==
               generation,
           "ordinary pre-arm reads preserve the publication generation");
    const auto after =
        candidate->get_selection_identity(punto::Selection::Primary);
    if (replace_primary) {
      expect(
          !armed && after && after->owner == reader_windows[0] &&
              !candidate->selection_identity_matches(punto::Selection::Primary,
                                                     *original),
          "queued real PRIMARY replacement rejects the original exact token");
    } else {
      expect(
          after && after->owner == original->owner &&
              after->selection_timestamp == original->selection_timestamp &&
              after->owner_generation == original->owner_generation &&
              after->connection_generation == original->connection_generation,
          "ordinary readers leave the complete original PRIMARY tuple intact");
      expect(
          armed &&
              candidate->paste_receipt_status(*armed) ==
                  punto::PasteReceiptStatus::Pending &&
              candidate->paste_receipt_request_seq(*armed) == 0,
          "unchanged PRIMARY admits a pending receipt after ordinary readers");
    }
    xcb_disconnect(editor.value);
    for (const auto &reader : readers)
      xcb_disconnect(reader.value);
  }
  Connection owner = connect_to();
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "concurrent payload") ==
             punto::ClipboardResult::Ok,
         "own clipboard for two ordinary consumers");
  const std::array<Connection, 2> clients{connect_to(), connect_to()};
  const std::array<xcb_window_t, 2> windows{
      create_window(clients[0].value, clients[0].screen),
      create_window(clients[1].value, clients[1].screen)};
  const auto clipboard = intern_atom(clients[0].value, "CLIPBOARD");
  const auto utf8 = intern_atom(clients[0].value, "UTF8_STRING");
  const std::array<xcb_atom_t, 2> properties{
      intern_atom(clients[0].value, "PUNTO_CONCURRENT_EDITOR"),
      intern_atom(clients[1].value, "PUNTO_CONCURRENT_READER")};
  auto error = xcb_ptr(xcb_request_check(
      clients[0].value,
      xcb_set_selection_owner_checked(clients[0].value, windows[0],
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error, "publish unchanged editor PRIMARY");
  set_active_window(clients[0].value, clients[0].screen, windows[0]);
  const auto context =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(context && context->owner == windows[0],
         "capture actual editor PRIMARY identity");
  const auto receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard, *context);
  expect(receipt.has_value(), "arm exact editor receipt");
  const auto foreign = request_selection(clients[1].value, windows[1], *manager,
                                         clipboard, utf8, properties[1]);
  expect(foreign.property == properties[1] &&
             read_property(clients[1].value, windows[1], properties[1], utf8) ==
                 "concurrent payload" &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Pending &&
             manager->paste_receipt_request_seq(*receipt) == 0,
         "ordinary reader receives bytes without acknowledging editor paste");
  const auto generation =
      manager->selection_generation(punto::Selection::Clipboard);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  manager->activate_paste_receipt(*receipt, deadline);
  for (std::size_t index = 0; index < clients.size(); ++index) {
    xcb_convert_selection(clients[index].value, windows[index], clipboard, utf8,
                          properties[index], XCB_CURRENT_TIME);
    auto barrier = xcb_ptr(xcb_get_input_focus_reply(
        clients[index].value, xcb_get_input_focus(clients[index].value),
        nullptr));
    expect(barrier != nullptr, "ordered client conversion reaches X server");
  }
  std::array<std::size_t, 2> responses{};
  while ((responses[0] == 0 || responses[1] == 0) &&
         std::chrono::steady_clock::now() < deadline) {
    (void)manager->pump_events();
    for (std::size_t index = 0; index < clients.size(); ++index) {
      while (auto event = xcb_ptr(xcb_poll_for_event(clients[index].value))) {
        if ((event->response_type & 0x7fU) != XCB_SELECTION_NOTIFY)
          continue;
        const auto &notify =
            *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
        ++responses[index];
        expect(notify.requestor == windows[index] &&
                   notify.selection == clipboard && notify.target == utf8 &&
                   notify.property == properties[index],
               index == 0 ? "editor conversion must succeed"
                          : "concurrent ordinary reader must not be refused");
        expect(read_property(clients[index].value, windows[index],
                             properties[index], utf8) == "concurrent payload",
               "each concurrent client receives exact payload bytes");
      }
    }
  }
  expect(responses == std::array<std::size_t, 2>{1, 1} &&
             std::chrono::steady_clock::now() < deadline,
         "both conversions finish once within original shared deadline");
  expect(manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Delivered &&
             manager->paste_receipt_request_seq(*receipt) == 1 &&
             manager->selection_generation(punto::Selection::Clipboard) ==
                 generation &&
             manager->selection_identity_matches(punto::Selection::Primary,
                                                 *context),
         "only editor confirms paste; payload and PRIMARY authority unchanged");
  for (const auto &client : clients)
    xcb_disconnect(client.value);
}

void test_deferred_request_lifetime_and_fresh_recovery() {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  for (const std::string_view scenario :
       {"cancel-rearm", "deadline", "foreign-receipt-expiry", "new-generation",
        "foreign-clipboard", "primary-aba", "reopen"}) {
    try {
      Connection owner = connect_to();
      auto manager =
          punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
      Connection client = connect_to();
      Connection foreign = connect_to();
      const auto foreign_window = create_window(foreign.value, foreign.screen);
      const bool foreign_reader = scenario == "foreign-receipt-expiry";
      const std::array<xcb_connection_t *, 2> requesters{
          client.value, foreign_reader ? foreign.value : client.value};
      const std::array<xcb_window_t, 2> windows{
          create_window(client.value, client.screen),
          foreign_reader ? foreign_window
                         : create_window(client.value, client.screen)};
      const auto clipboard = intern_atom(client.value, "CLIPBOARD");
      const auto utf8 = intern_atom(client.value, "UTF8_STRING");
      const std::array<xcb_atom_t, 2> properties{
          intern_atom(client.value, "PUNTO_LIFETIME_FIRST"),
          intern_atom(client.value, "PUNTO_LIFETIME_DEFERRED")};
      const auto fresh_property =
          intern_atom(client.value, "PUNTO_LIFETIME_FRESH");
      const std::string old_payload = "old lifetime payload";
      const std::string fresh_payload = "new lifetime payload";
      const std::string sentinel = "untouched deferred property";
      const auto barrier = [&](xcb_connection_t *connection) {
        auto reply = xcb_ptr(xcb_get_input_focus_reply(
            connection, xcb_get_input_focus(connection), nullptr));
        expect(reply != nullptr, "lifetime operation reaches actual X server");
      };
      const auto own = [&](xcb_connection_t *connection, xcb_window_t window,
                           xcb_atom_t selection) {
        auto error = xcb_ptr(xcb_request_check(
            connection, xcb_set_selection_owner_checked(
                            connection, window, selection, XCB_CURRENT_TIME)));
        expect(!error, "publish real lifetime ownership transition");
        barrier(connection);
      };
      own(client.value, windows[0], XCB_ATOM_PRIMARY);
      set_active_window(client.value, client.screen, windows[0]);
      expect(manager->set_text(punto::Selection::Clipboard, old_payload) ==
                 punto::ClipboardResult::Ok,
             "publish lifetime source generation");
      const auto context =
          manager->get_selection_identity(punto::Selection::Primary);
      expect(context && context->owner == windows[0],
             "capture lifetime PRIMARY authority");
      const auto receipt =
          manager->arm_paste_receipt(punto::Selection::Clipboard, *context);
      expect(receipt.has_value(), "arm lifetime source receipt");
      const auto generation =
          manager->selection_generation(punto::Selection::Clipboard);
      auto error = xcb_ptr(xcb_request_check(
          requesters[1],
          xcb_change_property_checked(
              requesters[1], XCB_PROP_MODE_REPLACE, windows[1], properties[1],
              utf8, 8, static_cast<std::uint32_t>(sentinel.size()),
              sentinel.data())));
      expect(!error, "install deferred property write sentinel");
      const auto deadline = Clock::now() + (foreign_reader ? 20ms : 1000ms);
      manager->activate_paste_receipt(*receipt, deadline);
      for (std::size_t index = 0; index < windows.size(); ++index) {
        xcb_convert_selection(requesters[index], windows[index], clipboard,
                              utf8, properties[index], XCB_CURRENT_TIME);
        barrier(requesters[index]);
      }
      std::array<std::optional<xcb_selection_notify_event_t>, 2> responses;
      const auto collect = [&] {
        for (std::size_t channel = 0; channel < requesters.size(); ++channel) {
          if (channel == 1 && requesters[1] == requesters[0])
            continue;
          barrier(requesters[channel]);
          while (auto event =
                     xcb_ptr(xcb_poll_for_event(requesters[channel]))) {
            if ((event->response_type & 0x7fU) != XCB_SELECTION_NOTIFY)
              continue;
            const auto &notify =
                *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
            const auto index = notify.requestor == windows[0] ? 0U : 1U;
            expect(notify.requestor == windows[index] &&
                       notify.selection == clipboard && notify.target == utf8 &&
                       notify.time == XCB_CURRENT_TIME &&
                       (notify.property == properties[index] ||
                        notify.property == XCB_ATOM_NONE) &&
                       !responses[index],
                   "exact lifetime notification fields without duplicate "
                   "responses");
            responses[index] = notify;
          }
        }
      };
      expect(manager->pump_events() <= 64,
             "lifetime first pump retains event bound");
      collect();
      expect(responses[0] && responses[0]->property == properties[0] &&
                 !responses[1] &&
                 read_property(client.value, windows[0], properties[0], utf8) ==
                     old_payload &&
                 read_property(requesters[1], windows[1], properties[1], utf8,
                               false) == sentinel &&
                 manager->paste_receipt_status(*receipt) ==
                     punto::PasteReceiptStatus::Delivered &&
                 manager->paste_receipt_request_seq(*receipt) == 1 &&
                 manager->selection_generation(punto::Selection::Clipboard) ==
                     generation,
             "one public pump delivers first request and leaves second "
             "actually deferred");
      if (foreign_reader)
        expect(Clock::now() < deadline,
               "ordinary reader is deferred before receipt authority expires");

      std::optional<punto::PasteReceiptToken> fresh_receipt;
      std::string expected_payload = old_payload;
      if (scenario == "cancel-rearm") {
        manager->cancel_paste_receipt(*receipt);
        // Context-free rearm does not pump away the old request before R2
        // exists.
        fresh_receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
      } else if (scenario == "deadline" || foreign_reader) {
        // Cross the captured bound, not an arbitrary application settle delay.
        std::this_thread::sleep_until(deadline + 1ms);
      } else if (scenario == "new-generation") {
        expect(manager->set_text(punto::Selection::Clipboard, fresh_payload) ==
                   punto::ClipboardResult::Ok,
               "publish new generation while old request may finish before "
               "publication");
        expected_payload = fresh_payload;
        fresh_receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
      } else if (scenario == "foreign-clipboard") {
        own(foreign.value, foreign_window, clipboard);
      } else if (scenario == "primary-aba") {
        own(foreign.value, foreign_window, XCB_ATOM_PRIMARY);
        own(client.value, windows[0], XCB_ATOM_PRIMARY);
      } else {
        own(client.value, XCB_WINDOW_NONE, XCB_ATOM_PRIMARY);
        Connection replacement = connect_to();
        expect(punto::ClipboardManagerTestAccess::reconnect(
                   *manager, replacement.value, replacement.screen),
               "reopen same manager with old deferred request outstanding");
        own(client.value, windows[0], XCB_ATOM_PRIMARY);
        expect(manager->set_text(punto::Selection::Clipboard, fresh_payload) ==
                   punto::ClipboardResult::Ok,
               "publish fresh connection generation");
        expected_payload = fresh_payload;
        fresh_receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
      }
      if (scenario == "cancel-rearm" || scenario == "new-generation" ||
          scenario == "reopen") {
        expect(fresh_receipt.has_value(),
               "rearm before draining obsolete request");
        manager->activate_paste_receipt(*fresh_receipt, Clock::now() + 1s);
        expect(manager->paste_receipt_status(*receipt) ==
                       punto::PasteReceiptStatus::Invalid &&
                   manager->paste_receipt_status(*fresh_receipt) ==
                       punto::PasteReceiptStatus::Pending &&
                   manager->paste_receipt_request_seq(*fresh_receipt) == 0,
               "old receipt invalid and rearmed receipt pending before old "
               "work drains");
      }
      if (scenario != "reopen") {
        pump_until(
            *manager,
            [&] {
              collect();
              return responses[1].has_value();
            },
            "deferred lifetime request reaches terminal notification");
        if (scenario == "new-generation" &&
            responses[1]->property == properties[1]) {
          expect(
              read_property(requesters[1], windows[1], properties[1], utf8) ==
                  old_payload,
              "prepublication old request receives only old generation bytes");
        } else {
          expect(responses[1]->property == XCB_ATOM_NONE &&
                     read_property(requesters[1], windows[1], properties[1],
                                   utf8, false) == sentinel,
                 "obsolete deferred request is refused without writing its "
                 "property");
        }
        if (foreign_reader)
          expect(
              manager->paste_receipt_status(*receipt) ==
                      punto::PasteReceiptStatus::Delivered &&
                  manager->paste_receipt_request_seq(*receipt) == 1,
              "ordinary peer expiry does not erase first completed delivery");
      } else {
        (void)manager->pump_events();
        collect();
        expect((!responses[1] || responses[1]->property == XCB_ATOM_NONE) &&
                   read_property(requesters[1], windows[1], properties[1], utf8,
                                 false) == sentinel,
               "closed connection discards old work without a new-epoch "
               "payload write");
      }
      if (scenario == "foreign-clipboard") {
        auto owner_reply = xcb_ptr(xcb_get_selection_owner_reply(
            client.value, xcb_get_selection_owner(client.value, clipboard),
            nullptr));
        expect(owner_reply && owner_reply->owner == foreign_window,
               "deferred rejection preserves actual foreign clipboard owner");
        std::exception_ptr provider_error;
        std::jthread provider{[&] {
          try {
            serve_one_text_request(foreign.value, "foreign clipboard bytes");
          } catch (...) {
            provider_error = std::current_exception();
          }
        }};
        const auto foreign_text =
            manager->get_text(punto::Selection::Clipboard);
        provider.join();
        if (provider_error)
          std::rethrow_exception(provider_error);
        expect(foreign_text && *foreign_text == "foreign clipboard bytes",
               "foreign owner still serves exact actual clipboard bytes");
        owner_reply = xcb_ptr(xcb_get_selection_owner_reply(
            client.value, xcb_get_selection_owner(client.value, clipboard),
            nullptr));
        expect(owner_reply && owner_reply->owner == foreign_window,
               "reading foreign bytes does not reclaim its ownership");
        own(foreign.value, XCB_WINDOW_NONE, clipboard);
        expect(manager->set_text(punto::Selection::Clipboard, fresh_payload) ==
                   punto::ClipboardResult::Ok,
               "fresh explicit publication after foreign owner releases");
        expected_payload = fresh_payload;
      }
      if (scenario == "primary-aba") {
        const auto changed =
            manager->get_selection_identity(punto::Selection::Primary);
        expect(changed && changed->owner == context->owner &&
                   changed->owner_generation != context->owner_generation &&
                   !manager->selection_identity_matches(
                       punto::Selection::Primary, *context),
               "actual PRIMARY ABA cannot revive old full-history authority");
      }
      if (!fresh_receipt) {
        manager->cancel_paste_receipt(*receipt);
        const auto fresh_context =
            manager->get_selection_identity(punto::Selection::Primary);
        expect(fresh_context.has_value(),
               "capture fresh recovery PRIMARY identity");
        fresh_receipt = manager->arm_paste_receipt(punto::Selection::Clipboard,
                                                   *fresh_context);
        expect(fresh_receipt.has_value(),
               "arm fresh recovery after lifetime rejection");
        manager->activate_paste_receipt(*fresh_receipt, Clock::now() + 1s);
      }
      expect(fresh_receipt &&
                 manager->paste_receipt_status(*receipt) ==
                     punto::PasteReceiptStatus::Invalid &&
                 manager->paste_receipt_status(*fresh_receipt) ==
                     punto::PasteReceiptStatus::Pending &&
                 manager->paste_receipt_request_seq(*fresh_receipt) == 0,
             "obsolete work cannot resurrect old receipt or confirm fresh "
             "receipt");
      const auto fresh = request_selection(client.value, windows[0], *manager,
                                           clipboard, utf8, fresh_property);
      expect(fresh.requestor == windows[0] && fresh.selection == clipboard &&
                 fresh.target == utf8 && fresh.time == XCB_CURRENT_TIME &&
                 fresh.property == fresh_property &&
                 read_property(client.value, windows[0], fresh_property,
                               utf8) == expected_payload &&
                 manager->paste_receipt_status(*fresh_receipt) ==
                     punto::PasteReceiptStatus::Delivered &&
                 manager->paste_receipt_request_seq(*fresh_receipt) == 1 &&
                 manager->is_open(),
             "fresh real conversion alone delivers exact bytes and confirms "
             "new receipt");
      collect();
      if (scenario == "reopen")
        expect((!responses[1] || responses[1]->property == XCB_ATOM_NONE) &&
                   read_property(requesters[1], windows[1], properties[1], utf8,
                                 false) == sentinel,
               "fresh epoch cannot replay discarded old request");
      xcb_disconnect(foreign.value);
      xcb_disconnect(client.value);
    } catch (const std::exception &error) {
      throw std::runtime_error{std::string{scenario} + ": " + error.what()};
    }
  }
}

void test_context_bound_request_flood_does_not_reenter_payload() {
  Connection owner = connect_to();
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "payload") ==
             punto::ClipboardResult::Ok,
         "own clipboard for context-bound request flood");

  Connection requester = connect_to();
  const xcb_window_t window = create_window(requester.value, requester.screen);
  const xcb_atom_t clipboard = intern_atom(requester.value, "CLIPBOARD");
  const xcb_atom_t utf8 = intern_atom(requester.value, "UTF8_STRING");
  const xcb_atom_t property =
      intern_atom(requester.value, "PUNTO_TEST_CONTEXT_BUDGET");
  auto own_error = xcb_ptr(xcb_request_check(
      requester.value,
      xcb_set_selection_owner_checked(requester.value, window, XCB_ATOM_PRIMARY,
                                      XCB_CURRENT_TIME)));
  expect(!own_error, "publish context for request flood");
  set_active_window(requester.value, requester.screen, window);
  const auto context =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(context && context->owner == window,
         "capture actual requestor PRIMARY context");
  const auto receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard, *context);
  expect(receipt.has_value(), "arm context-bound flood receipt");
  manager->activate_paste_receipt(*receipt, std::chrono::steady_clock::now() +
                                                std::chrono::seconds{1});
  const auto generation =
      manager->selection_generation(punto::Selection::Clipboard);

  const auto server_barrier = [&] {
    auto reply = xcb_ptr(xcb_get_input_focus_reply(
        requester.value, xcb_get_input_focus(requester.value), nullptr));
    expect(reply != nullptr, "request flood reaches the actual X server");
  };
  constexpr std::size_t kRequestCount = 32;
  for (std::size_t i = 0; i < kRequestCount; ++i) {
    xcb_convert_selection(requester.value, window, clipboard, utf8, property,
                          XCB_CURRENT_TIME);
  }
  server_barrier();
  pollfd ready{xcb_get_file_descriptor(owner.value), POLLIN, 0};
  expect(::poll(&ready, 1, 1000) == 1, "actual request flood is ready to pump");
  const auto processed = manager->pump_events();
  server_barrier();

  std::size_t responses = 0;
  std::size_t successful = 0;
  bool cancelled = false;
  const auto collect_responses = [&] {
    while (auto event = xcb_ptr(xcb_poll_for_event(requester.value))) {
      if ((event->response_type & 0x7fU) == XCB_SELECTION_NOTIFY) {
        const auto &notify =
            *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
        ++responses;
        expect(notify.requestor == window && notify.selection == clipboard &&
                   notify.target == utf8 && notify.time == XCB_CURRENT_TIME &&
                   (notify.property == property ||
                    notify.property == XCB_ATOM_NONE),
               "flood retains exact request/notification attribution");
        if (notify.property != XCB_ATOM_NONE) {
          ++successful;
          expect(!cancelled && read_property(requester.value, window, property,
                                             utf8) == "payload",
                 "only live bounded prefix can deliver original payload");
        }
      }
    }
  };
  collect_responses();
  expect(processed > 0 && responses > 0,
         "one pump has an actual request/response witness");
  expect(processed <= 64 && responses <= 64 && successful <= 1,
         "one public pump bounds both events and successful peer service");
  expect(
      manager->is_open() &&
          manager->selection_generation(punto::Selection::Clipboard) ==
              generation &&
          manager->selection_request_seq(punto::Selection::Clipboard) ==
              successful &&
          manager->paste_receipt_status(*receipt) ==
              (successful ? punto::PasteReceiptStatus::Delivered
                          : punto::PasteReceiptStatus::Pending),
      "bounded valid prefix preserves generation and exact receipt activity");
  auto actual_owner = xcb_ptr(xcb_get_selection_owner_reply(
      requester.value, xcb_get_selection_owner(requester.value, clipboard),
      nullptr));
  expect(actual_owner &&
             actual_owner->owner ==
                 punto::ClipboardManagerTestAccess::window(*manager),
         "bounded peer service retains actual clipboard ownership");

  manager->cancel_paste_receipt(*receipt);
  cancelled = true;
  const std::string sentinel = "cancelled flood property";
  auto sentinel_error = xcb_ptr(xcb_request_check(
      requester.value,
      xcb_change_property_checked(
          requester.value, XCB_PROP_MODE_REPLACE, window, property, utf8, 8,
          static_cast<std::uint32_t>(sentinel.size()), sentinel.data())));
  expect(!sentinel_error, "install cancelled flood payload write sentinel");
  expect(manager->set_text(punto::Selection::Clipboard, "fresh payload") ==
             punto::ClipboardResult::Ok,
         "new generation cannot relabel queued obsolete requests");
  const auto fresh_context =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(fresh_context && fresh_context->owner == window,
         "fresh request proves context without resetting PRIMARY");
  const auto fresh_receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard, *fresh_context);
  expect(fresh_receipt.has_value(),
         "arm fresh receipt with obsolete work outstanding");
  expect(manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Invalid &&
             manager->paste_receipt_status(*fresh_receipt) ==
                 punto::PasteReceiptStatus::Pending,
         "old receipt invalid and new receipt unconfirmed before fresh input");
  server_barrier();
  collect_responses();
  const auto drain_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (responses < kRequestCount &&
         std::chrono::steady_clock::now() < drain_deadline) {
    const auto before_responses = responses;
    const auto before_successful = successful;
    expect(manager->pump_events() <= 64,
           "obsolete flood pump retains event bound");
    server_barrier();
    collect_responses();
    expect(responses - before_responses <= 64 &&
               successful - before_successful <= 1,
           "each public pump retains one successful peer-service bound");
  }
  try {
    expect(responses == kRequestCount &&
               read_property(requester.value, window, property, utf8, false) ==
                   sentinel &&
               manager->paste_receipt_status(*fresh_receipt) ==
                   punto::PasteReceiptStatus::Pending &&
               manager->paste_receipt_request_seq(*fresh_receipt) == 0 &&
               manager->selection_request_seq(punto::Selection::Clipboard) ==
                   successful,
           "all cancelled old requests finish without new payload writes or "
           "receipt confirmation");
  } catch (const std::exception &) {
    int sentinel_equal = -1;
    try {
      sentinel_equal = read_property(requester.value, window, property, utf8,
                                     false) == sentinel;
    } catch (...) {
    }
    std::cerr << "[punto-contract] cancelled_flood responses=" << responses
              << " expected=" << kRequestCount << " successful=" << successful
              << " sentinel_equal=" << sentinel_equal << " fresh_status="
              << static_cast<unsigned>(
                     manager->paste_receipt_status(*fresh_receipt))
              << " fresh_seq="
              << manager->paste_receipt_request_seq(*fresh_receipt)
              << " selection_seq="
              << manager->selection_request_seq(punto::Selection::Clipboard)
              << " is_open=" << manager->is_open() << '\n';
    throw;
  }
  const auto recovered = request_selection(requester.value, window, *manager,
                                           clipboard, utf8, property);
  expect(recovered.property == property &&
             read_property(requester.value, window, property, utf8) ==
                 "fresh payload" &&
             manager->paste_receipt_status(*fresh_receipt) ==
                 punto::PasteReceiptStatus::Delivered,
         "fresh context-bound payload recovers without reconnect");
  xcb_disconnect(requester.value);
}

struct NestedXServer {
  pid_t pid = -1;
  std::string display;

  ~NestedXServer() {
    if (pid > 0) {
      (void)::kill(pid, SIGCONT);
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
    }
  }
};

NestedXServer start_nested_xvfb() {
  int descriptors[2]{};
  expect(::pipe(descriptors) == 0, "create Xvfb display pipe");
  const pid_t pid = ::fork();
  expect(pid >= 0, "fork nested Xvfb");
  if (pid == 0) {
    ::close(descriptors[0]);
    if (::dup2(descriptors[1], STDOUT_FILENO) < 0) {
      _exit(126);
    }
    ::close(descriptors[1]);
    ::execl("/usr/bin/Xvfb", "Xvfb", "-displayfd", "1", "-nolisten", "tcp",
            "-extension", "GLX", static_cast<char *>(nullptr));
    _exit(127);
  }

  ::close(descriptors[1]);
  pollfd descriptor{descriptors[0], POLLIN, 0};
  expect(::poll(&descriptor, 1, 2000) == 1, "nested Xvfb startup timeout");
  std::array<char, 32> buffer{};
  const ssize_t count =
      ::read(descriptors[0], buffer.data(), buffer.size() - 1);
  ::close(descriptors[0]);
  expect(count > 1, "nested Xvfb did not publish display");
  std::string number{buffer.data(), static_cast<std::size_t>(count)};
  while (!number.empty() && (number.back() == '\n' || number.back() == '\r')) {
    number.pop_back();
  }
  expect(!number.empty(), "nested Xvfb display is empty");
  return NestedXServer{pid, ":" + number};
}

void test_conditional_publication_uncertainty_releases_server(
    publication_guard_fault::Stage stage) {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  NestedXServer server = start_nested_xvfb();
  Connection connection = connect_to(server.display.c_str());
  auto manager = punto::ClipboardManagerTestAccess::make(connection.value,
                                                         connection.screen);
  Connection peer = connect_to(server.display.c_str());
  auto peer_connection =
      punto::ClipboardManagerTestAccess::adopt(peer.value, peer.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
             punto::ClipboardResult::Ok,
         "publish before uncertain actual server request");
  const auto original =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(original.has_value(), "capture authority before guard uncertainty");
  publication_guard_fault::reset(connection.value);
  publication_guard_fault::stage = stage;
  publication_guard_fault::server = server.pid;
  std::jthread rescue{[pid = server.pid] {
    std::this_thread::sleep_for(150ms);
    (void)::kill(pid, SIGCONT);
  }};
  const auto started = Clock::now();
  const bool published = manager->set_text_if_owner(
      punto::Selection::Clipboard, *original, "uncertain", started + 50ms);
  const auto elapsed = Clock::now() - started;
  (void)::kill(server.pid, SIGCONT);
  expect(
      publication_guard_fault::consumed && !published && !manager->is_open() &&
          !publication_guard_fault::held && elapsed >= 45ms &&
          elapsed < 120ms && publication_guard_fault::grabs == 1 &&
          publication_guard_fault::writes ==
              (stage == publication_guard_fault::Stage::GrabStop ? 0U : 1U),
      "accepted grab/write/release uncertainty closes under caller deadline");
  const auto cookie = xcb_get_input_focus(peer.value);
  punto::x11_detail::XcbOperationResult result{};
  auto reply = xcb_ptr(
      static_cast<xcb_get_input_focus_reply_t *>(peer_connection.wait_for_reply(
          cookie.sequence, Clock::now() + 100ms, result)));
  expect(reply && result == punto::x11_detail::XcbOperationResult::Success,
         "actual peer progresses after uncertain grab disconnects");
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  auto owner = xcb_ptr(xcb_get_selection_owner_reply(
      peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
  expect(owner && owner->owner == XCB_WINDOW_NONE,
         "closed publisher leaves no owner or transferable stale payload");
  publication_guard_fault::reset();
}

void test_disconnect_is_fail_closed_not_process_fatal() {
  NestedXServer server = start_nested_xvfb();
  Connection owner = connect_to(server.display.c_str());
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);
  expect(manager->set_text(punto::Selection::Clipboard, "payload") ==
             punto::ClipboardResult::Ok,
         "own selection on nested X server");

  expect(::kill(server.pid, SIGTERM) == 0, "stop nested X server");
  expect(::waitpid(server.pid, nullptr, 0) == server.pid,
         "reap nested X server");
  server.pid = -1;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (manager->is_open() && std::chrono::steady_clock::now() < deadline) {
    (void)manager->pump_events();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  expect(!manager->is_open(), "disconnect revokes clipboard state");
  expect(manager->pump_events() == 0, "pump after disconnect is harmless");
  expect(!manager->get_text(punto::Selection::Clipboard),
         "read after disconnect fails closed");
  manager->close();
  manager->close();
}

void test_absent_owner_is_a_known_empty_selection() {
  NestedXServer server = start_nested_xvfb();
  Connection owner = connect_to(server.display.c_str());
  auto manager =
      punto::ClipboardManagerTestAccess::make(owner.value, owner.screen);

  const auto clipboard = manager->get_text(punto::Selection::Clipboard);
  const auto primary = manager->get_text(punto::Selection::Primary);
  expect(clipboard.has_value() && clipboard->empty(),
         "absent CLIPBOARD owner is known empty state");
  expect(primary.has_value() && primary->empty(),
         "absent PRIMARY owner is known empty state");
}

void test_stopped_server_is_bounded_and_fail_closed() {
  NestedXServer server = start_nested_xvfb();
  Connection owner = connect_to(server.display.c_str());
  auto manager = punto::ClipboardManagerTestAccess::make(
      owner.value, owner.screen, std::chrono::milliseconds{50});
  expect(manager->set_text(punto::Selection::Clipboard, "payload") ==
             punto::ClipboardResult::Ok,
         "own selection before stopped-server test");

  expect(::kill(server.pid, SIGSTOP) == 0, "suspend nested X server");
  const auto started = std::chrono::steady_clock::now();
  const bool owned = manager->verify_ownership();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // SIGTERM is not delivered while the test server is stopped. Resume it
  // before NestedXServer performs its bounded cleanup.
  expect(::kill(server.pid, SIGCONT) == 0, "resume nested X server");
  expect(!owned, "unresponsive X server cannot confirm ownership");
  expect(elapsed < std::chrono::milliseconds{500},
         "ownership verification obeys its deadline");
  expect(!manager->is_open(), "X timeout revokes clipboard connection");
}

void test_cold_initialization() {
  NestedXServer server = start_nested_xvfb();
  int screen = -1;
  auto *connection = xcb_connect(server.display.c_str(), &screen);
  expect(connection != nullptr && xcb_connection_has_error(connection) == 0,
         "connect without prewarming XFixes");
  auto manager = punto::ClipboardManagerTestAccess::make(connection, screen);
  expect(manager->is_open(), "cold connection initializes successfully");
}

void test_initialization_stall(initialization_fault::Stage stage) {
  NestedXServer server = start_nested_xvfb();
  int screen = -1;
  auto *connection = xcb_connect(server.display.c_str(), &screen);
  expect(connection != nullptr && xcb_connection_has_error(connection) == 0,
         "connect before initialization stall");
  initialization_fault::stage = stage;
  initialization_fault::connection = connection;
  initialization_fault::server = server.pid;
  initialization_fault::stopped = false;
  initialization_fault::disconnected = false;
  initialization_fault::used_after_disconnect = false;
  // Rescue the old unbounded extension lookup so RED terminates on its own.
  std::jthread resume_server{[pid = server.pid](std::stop_token stop) {
    while (!initialization_fault::stopped.load() && !stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (stop.stop_requested()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    (void)::kill(pid, SIGCONT);
  }};
  const auto started = std::chrono::steady_clock::now();
  auto manager = punto::ClipboardManagerTestAccess::make(
      connection, screen, std::chrono::milliseconds{50});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  (void)::kill(server.pid, SIGCONT);
  const bool stopped = initialization_fault::stopped;
  const bool disconnected = initialization_fault::disconnected;
  const bool used_after_disconnect =
      initialization_fault::used_after_disconnect;
  initialization_fault::stage = initialization_fault::Stage::None;
  initialization_fault::connection = nullptr;
  expect(stopped, "fault reached the requested initialization boundary");
  expect(!used_after_disconnect,
         "initialization must not use a revoked XCB connection");
  expect(elapsed < std::chrono::milliseconds{250},
         "cold initialization obeys its deadline while X server stalls");
  expect(disconnected && !manager->is_open(),
         "initialization timeout closes its connection");
}

void test_caller_initialization_deadline() {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  NestedXServer server = start_nested_xvfb();
  struct PrivateAuthority {
    char path[48] = "/tmp/punto-clipboard-deadline-XXXXXX";
    ~PrivateAuthority() { (void)::unlink(path); }
  } authority;
  const int descriptor = ::mkstemp(authority.path);
  expect(descriptor >= 0, "create private empty Xauthority for nested server");
  expect(::close(descriptor) == 0, "close private Xauthority");
  punto::X11Session session{[&] {
    punto::X11SessionInfo info;
    info.uid = static_cast<std::uint32_t>(::geteuid());
    info.gid = static_cast<std::uint32_t>(::getegid());
    info.display = server.display;
    info.xauthority_path = authority.path;
    return punto::x11_detail::ProbeResult{
        punto::x11_detail::ProbeStatus::Healthy, std::move(info)};
  }};
  expect(session.initialize(), "publish only the private nested X11 session");

  const auto open_with_pause = [&](initialization_fault::Stage stage,
                                   bool caller_budget) {
    auto manager = std::make_unique<punto::ClipboardManager>(session, 10ms);
    initialization_fault::stage = stage;
    initialization_fault::connection = nullptr;
    initialization_fault::server = server.pid;
    initialization_fault::stopped = false;
    initialization_fault::disconnected = false;
    initialization_fault::used_after_disconnect = false;
    std::jthread resume_server{[pid = server.pid](std::stop_token stop) {
      while (!initialization_fault::stopped.load() && !stop.stop_requested()) {
        std::this_thread::sleep_for(1ms);
      }
      if (!stop.stop_requested()) {
        std::this_thread::sleep_for(20ms);
        (void)::kill(pid, SIGCONT);
      }
    }};
    const auto started = Clock::now();
    const bool opened =
        caller_budget ? manager->open(started + 100ms) : manager->open();
    const auto elapsed = Clock::now() - started;
    (void)::kill(server.pid, SIGCONT);
    resume_server.request_stop();
    resume_server.join();
    const bool stopped = initialization_fault::stopped;
    const bool used_after_disconnect =
        initialization_fault::used_after_disconnect;
    initialization_fault::stage = initialization_fault::Stage::None;
    initialization_fault::connection = nullptr;
    expect(stopped,
           "public open reaches the requested initialization boundary");
    expect(!used_after_disconnect,
           "deadline initialization never uses revoked XCB");
    expect(opened == caller_budget && manager->is_open() == caller_budget,
           "caller deadline admits initialization beyond unchanged 10ms "
           "request budget");
    expect(elapsed < 100ms,
           "initialization never receives a fresh caller budget");
    return manager;
  };

  for (const auto stage : {initialization_fault::Stage::Atom,
                           initialization_fault::Stage::Extension}) {
    auto manager = open_with_pause(stage, true);
    expect(manager->set_text(punto::Selection::Clipboard, "deadline-test") ==
               punto::ClipboardResult::Ok,
           "own payload before idempotency and request deadline checks");
    const auto generation =
        manager->selection_generation(punto::Selection::Clipboard);
    expect(manager->open(Clock::now() - 1ms) && manager->open() &&
               manager->locally_owns_generation(punto::Selection::Clipboard,
                                                generation),
           "already-open calls preserve ownership even with expired "
           "initialization deadline");
    expect(::kill(server.pid, SIGSTOP) == 0,
           "stop private server with owned payload");
    std::jthread rescue{[pid = server.pid] {
      std::this_thread::sleep_for(100ms);
      (void)::kill(pid, SIGCONT);
    }};
    const auto started = Clock::now();
    const bool owned = manager->verify_ownership();
    const auto elapsed = Clock::now() - started;
    (void)::kill(server.pid, SIGCONT);
    expect(!owned && !manager->is_open() && elapsed < 50ms,
           "generous initialization does not replace the 10ms request timeout");
  }
  (void)open_with_pause(initialization_fault::Stage::Atom, false);

  punto::ClipboardManager short_budget{session, 100ms};
  initialization_fault::stage = initialization_fault::Stage::Atom;
  initialization_fault::connection = nullptr;
  initialization_fault::stopped = false;
  const bool expired_opened = short_budget.open(Clock::now() - 1ms);
  const bool expired_initialized = initialization_fault::stopped ||
                                   initialization_fault::connection != nullptr;
  (void)::kill(server.pid, SIGCONT);
  initialization_fault::stage = initialization_fault::Stage::None;
  initialization_fault::connection = nullptr;
  expect(!expired_opened && !short_budget.is_open() && !expired_initialized,
         "expired caller deadline fails closed before initialization");
  expect(::kill(server.pid, SIGSTOP) == 0,
         "stop private server before short deadline");
  std::jthread rescue{[pid = server.pid] {
    std::this_thread::sleep_for(100ms);
    (void)::kill(pid, SIGCONT);
  }};
  const auto started = Clock::now();
  const bool opened = short_budget.open(started + 5ms);
  const auto elapsed = Clock::now() - started;
  (void)::kill(server.pid, SIGCONT);
  expect(!opened && !short_budget.is_open() && elapsed < 50ms,
         "short caller deadline is not replaced by the 100ms request timeout");
}

void test_identity_check_keeps_caller_deadline_while_serving_request() {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  NestedXServer server = start_nested_xvfb();
  Connection manager_connection = connect_to(server.display.c_str());
  auto manager = punto::ClipboardManagerTestAccess::make(
      manager_connection.value, manager_connection.screen, 100ms);
  Connection source = connect_to(server.display.c_str());
  const xcb_window_t source_window = create_window(source.value, source.screen);
  auto error = xcb_ptr(xcb_request_check(
      source.value,
      xcb_set_selection_owner_checked(source.value, source_window,
                                      XCB_ATOM_PRIMARY, XCB_CURRENT_TIME)));
  expect(!error && xcb_flush(source.value) > 0,
         "publish identity deadline fixture");
  std::thread source_server{
      [&] { serve_one_text_request(source.value, "selected"); }};
  const auto selected = manager->get_text_with_owner(punto::Selection::Primary);
  source_server.join();
  expect(selected.has_value(), "capture identity before bounded request pump");
  expect(manager->set_text(punto::Selection::Clipboard, "owned") ==
             punto::ClipboardResult::Ok,
         "own clipboard before queued request");

  Connection requester = connect_to(server.display.c_str());
  const xcb_window_t requester_window =
      create_window(requester.value, requester.screen);
  const xcb_atom_t clipboard = intern_atom(requester.value, "CLIPBOARD");
  const xcb_atom_t utf8 = intern_atom(requester.value, "UTF8_STRING");
  const xcb_atom_t property =
      intern_atom(requester.value, "PUNTO_IDENTITY_DEADLINE");
  xcb_convert_selection(requester.value, requester_window, clipboard, utf8,
                        property, XCB_CURRENT_TIME);
  expect(xcb_flush(requester.value) > 0,
         "queue request before identity deadline check");
  pollfd manager_fd{xcb_get_file_descriptor(manager_connection.value), POLLIN,
                    0};
  expect(::poll(&manager_fd, 1, 1000) == 1,
         "selection request reaches manager before server stop");
  expect(::kill(server.pid, SIGSTOP) == 0,
         "stop X server while identity pump serves request");
  std::jthread rescue{[pid = server.pid] {
    std::this_thread::sleep_for(100ms);
    (void)::kill(pid, SIGCONT);
  }};

  const auto started = Clock::now();
  const bool matched = manager->selection_identity_matches(
      punto::Selection::Primary, *selected, started + 5ms);
  const auto elapsed = Clock::now() - started;
  (void)::kill(server.pid, SIGCONT);
  expect(!matched, "expired request service cannot confirm selection identity");
  expect(elapsed < 50ms,
         "identity request service does not replace the caller deadline");

  xcb_disconnect(requester.value);
  xcb_disconnect(source.value);
}

void test_stopped_server_large_payload_send_is_bounded() {
  NestedXServer server = start_nested_xvfb();
  Connection owner = connect_to(server.display.c_str());
  auto manager = punto::ClipboardManagerTestAccess::make(
      owner.value, owner.screen, std::chrono::milliseconds{50});
  const std::size_t maximum =
      punto::ClipboardManagerTestAccess::maximum_payload_bytes(*manager);
  expect(maximum > 0 && maximum <= 4096U,
         "large-send fixture uses the synchronous transport cap");
  expect(manager->set_text(punto::Selection::Clipboard,
                           std::string(maximum, 'x')) ==
             punto::ClipboardResult::Ok,
         "own maximum bounded payload");

  Connection requester = connect_to(server.display.c_str());
  const xcb_window_t window = create_window(requester.value, requester.screen);
  const xcb_atom_t clipboard = intern_atom(requester.value, "CLIPBOARD");
  const xcb_atom_t utf8 = intern_atom(requester.value, "UTF8_STRING");
  const xcb_atom_t property =
      intern_atom(requester.value, "PUNTO_TEST_LARGE_SEND");
  xcb_convert_selection(requester.value, window, clipboard, utf8, property,
                        XCB_CURRENT_TIME);
  expect(xcb_flush(requester.value) > 0, "queue maximum-payload request");

  pollfd owner_fd{xcb_get_file_descriptor(owner.value), POLLIN, 0};
  expect(::poll(&owner_fd, 1, 1000) == 1,
         "selection request reaches the owner before suspension");
  expect(::kill(server.pid, SIGSTOP) == 0, "suspend large-send X server");

  const auto started = std::chrono::steady_clock::now();
  (void)manager->pump_events();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  expect(::kill(server.pid, SIGCONT) == 0, "resume large-send X server");
  expect(elapsed < std::chrono::milliseconds{500},
         "maximum payload send is bounded when X stops reading");
  expect(!manager->is_open(),
         "timed-out maximum payload fails the connection closed");
  xcb_disconnect(requester.value);
}

std::unique_ptr<punto::ClipboardManager>
raw_manager(const char *display, unsigned int witness_depth = 1) {
  auto connection = connect_to(display);
  auto manager = punto::ClipboardManagerTestAccess::make(connection.value,
                                                         connection.screen);
  if (witness_depth)
    punto::ClipboardManagerTestAccess::attach_witness(
        *manager, raw_manager(display, witness_depth - 1));
  return manager;
}

punto::SelectionData
read_wire_payload(xcb_connection_t *reader, xcb_window_t window,
                  punto::ClipboardManager &manager, xcb_atom_t clipboard,
                  xcb_atom_t target, xcb_atom_t property,
                  const punto::PasteReceiptToken *receipt = nullptr,
                  bool withhold_terminator = false) {
  using namespace std::chrono_literals;
  const auto notify =
      request_selection(reader, window, manager, clipboard, target, property);
  expect(notify.property == property, "actual payload notify succeeds");
  // The header has not been deleted, so all queued NewValues are pre-chunk.
  for (std::size_t queued = 0;; ++queued) {
    auto event = xcb_ptr(xcb_poll_for_queued_event(reader));
    if (!event)
      break;
    expect(queued < 64, "bounded initial header notification drain");
  }
  const auto get = [&] {
    xcb_generic_error_t *raw_error = nullptr;
    auto value = xcb_ptr(xcb_get_property_reply(
        reader,
        xcb_get_property(reader, 0, window, property, XCB_GET_PROPERTY_TYPE_ANY,
                         0, 1024U * 1024U),
        &raw_error));
    auto error = xcb_ptr(raw_error);
    expect(value && !error && value->bytes_after == 0,
           "complete actual wire property");
    return value;
  };
  const auto acknowledge = [&] {
    auto error = xcb_ptr(xcb_request_check(
        reader, xcb_delete_property_checked(reader, window, property)));
    expect(!error && xcb_flush(reader) > 0,
           "actual requestor chunk acknowledgement");
  };
  auto value = get();
  const auto incr = intern_atom(reader, "INCR");
  const bool incremental = value->type == incr;
  std::uint32_t advertised_bytes = 0;
  if (incremental) {
    expect(value->format == 32 &&
               xcb_get_property_value_length(value.get()) == 4,
           "actual INCR header");
    std::memcpy(&advertised_bytes, xcb_get_property_value(value.get()),
                sizeof(advertised_bytes));
    if (receipt)
      expect(manager.paste_receipt_status(*receipt) ==
                     punto::PasteReceiptStatus::Pending &&
                 manager.paste_receipt_request_seq(*receipt) == 0,
             "header never confirms a completed receipt");
    acknowledge();
  }
  expect(!withhold_terminator ||
             (incremental && receipt && advertised_bytes != 0),
         "expiry control requires a real receipt-bound INCR");
  punto::SelectionData data{target};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for (std::size_t chunks = 0; chunks < 4096; ++chunks) {
    if (incremental) {
      bool changed = false;
      while (!changed && std::chrono::steady_clock::now() < deadline) {
        (void)manager.pump_events();
        auto event = xcb_ptr(xcb_poll_for_event(reader));
        if (event && (event->response_type & 0x7fU) == XCB_PROPERTY_NOTIFY) {
          const auto &change =
              *reinterpret_cast<xcb_property_notify_event_t *>(event.get());
          changed = change.window == window && change.atom == property &&
                    change.state == XCB_PROPERTY_NEW_VALUE;
        }
        if (!changed) {
          pollfd fd{xcb_get_file_descriptor(reader), POLLIN, 0};
          (void)::poll(&fd, 1, 1);
        }
      }
      expect(changed, "bounded real INCR progress");
      value = get();
    }
    const auto length = xcb_get_property_value_length(value.get());
    expect(length >= 0 && value->type != incr &&
               (value->format == 8 || value->format == 16 ||
                value->format == 32) &&
               static_cast<std::size_t>(length) % (value->format / 8U) == 0,
           "actual aligned data type and format");
    if (data.type == XCB_ATOM_NONE) {
      data.type = value->type;
      data.format = value->format;
    }
    expect(data.type == value->type && data.format == value->format,
           "constant INCR type/format");
    const auto *bytes =
        static_cast<const std::uint8_t *>(xcb_get_property_value(value.get()));
    if (length)
      data.bytes.insert(data.bytes.end(), bytes, bytes + length);
    if (receipt && incremental && length > 0)
      expect(
          manager.paste_receipt_status(*receipt) ==
                  punto::PasteReceiptStatus::Pending &&
              manager.paste_receipt_request_seq(*receipt) == 0,
          "EVERY nonempty chunk, including the last, leaves receipt Pending");
    if (withhold_terminator && data.bytes.size() == advertised_bytes &&
        length > 0)
      return data; // Leave the last data property intact: no ACK, therefore no
                   // zero terminator.
    if (!incremental || length == 0) {
      if (receipt)
        expect(manager.paste_receipt_status(*receipt) ==
                       punto::PasteReceiptStatus::Delivered &&
                   manager.paste_receipt_request_seq(*receipt) == 1,
               "Delivered only after complete transfer and actual zero "
               "terminator");
      acknowledge();
      return data;
    }
    acknowledge();
  }
  throw std::runtime_error{"bounded real INCR chunk count"};
}

void test_deferred_incr_lifetime(std::string_view scenario) {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str(), 2);
  std::array<std::unique_ptr<punto::ClipboardManager>, 3> publication_observers;
  for (auto &observer : publication_observers) {
    observer = raw_manager(server.display.c_str(), 0);
    expect(observer->is_open(),
           "pre-open real publication observer before PRIMARY ownership");
  }
  std::size_t publication_observer_index = 0;
  auto peer = connect_to(server.display.c_str());
  auto editor = connect_to(server.display.c_str());
  auto foreign = connect_to(server.display.c_str());
  const auto window = create_window(peer.value, peer.screen);
  const auto editor_window = create_window(editor.value, editor.screen);
  const auto foreign_window = create_window(foreign.value, foreign.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const auto incr = intern_atom(peer.value, "INCR");
  const auto property = intern_atom(peer.value, "PUNTO_DEFERRED_INCR");
  const auto editor_property =
      intern_atom(peer.value, "PUNTO_DEFERRED_INCR_EDITOR");
  std::string old_payload(8192, 'o');
  std::fill(old_payload.begin() + 4096, old_payload.end(), 'p');
  const std::string fresh_payload(8192, 'n');
  const std::string sentinel = "recreated window property";
  const auto barrier = [&](xcb_connection_t *connection) {
    auto reply = xcb_ptr(xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), nullptr));
    expect(reply != nullptr, "INCR operations reach actual X server");
  };
  const auto own = [&](xcb_connection_t *connection, xcb_window_t owner,
                       xcb_atom_t selection) {
    auto error = xcb_ptr(xcb_request_check(
        connection, xcb_set_selection_owner_checked(
                        connection, owner, selection, XCB_CURRENT_TIME)));
    expect(!error, "real INCR owner transition");
    barrier(connection);
  };
  const auto publish = [&](const std::string &payload) {
    expect(publication_observer_index < publication_observers.size(),
           "normal INCR scenario uses at most three real publications");
    Access::attach_witness(
        *manager,
        std::move(publication_observers[publication_observer_index++]));
    const auto origin =
        manager->get_selection_identity(punto::Selection::Clipboard);
    expect(origin.has_value(), "capture actual INCR publication origin");
    const auto result = manager->set_payload_if_owner(
        punto::Selection::Clipboard, *origin, manager->text_payload(payload),
        Clock::now() + 1s);
    expect(result.completed, "publish real large INCR payload");
    manager->cancel_publication_witness(result);
  };
  const auto value = [&] {
    xcb_generic_error_t *raw_error = nullptr;
    auto reply = xcb_ptr(xcb_get_property_reply(
        peer.value,
        xcb_get_property(peer.value, 0, window, property,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 4096),
        &raw_error));
    auto error = xcb_ptr(raw_error);
    expect(reply && !error && reply->bytes_after == 0,
           "observe complete actual INCR property without deleting it");
    return reply;
  };
  const auto acknowledge = [&] {
    auto error = xcb_ptr(xcb_request_check(
        peer.value, xcb_delete_property_checked(peer.value, window, property)));
    expect(!error, "queue actual INCR property-delete ACK");
    barrier(peer.value);
  };
  const auto header = [&](const auto &reply) {
    expect(reply->type == incr && reply->format == 32 &&
               xcb_get_property_value_length(reply.get()) == 4,
           "actual unchanged INCR header");
    std::uint32_t bytes = 0;
    std::memcpy(&bytes, xcb_get_property_value(reply.get()), sizeof(bytes));
    expect(bytes == old_payload.size(),
           "exact accepted INCR advertised length");
  };
  publish(old_payload);
  const auto old_generation =
      manager->selection_generation(punto::Selection::Clipboard);
  const auto initial = request_selection(peer.value, window, *manager,
                                         clipboard, utf8, property);
  expect(initial.requestor == window && initial.selection == clipboard &&
             initial.target == utf8 && initial.time == XCB_CURRENT_TIME &&
             initial.property == property,
         "real unarmed A conversion accepted with exact notification");
  header(value());
  const auto old_id = Access::outgoing_transfer_id(*manager, window, property);
  expect(old_id != 0, "real accepted A has immutable transfer identity");
  if (scenario == "replacement") {
    for (std::size_t offset = 0; offset < old_payload.size(); offset += 4096) {
      acknowledge();
      expect(manager->pump_events() <= 64,
             "prepare final nonzero A chunk boundedly");
      barrier(peer.value);
      const auto chunk = value();
      expect(chunk->type == utf8 && chunk->format == 8 &&
                 xcb_get_property_value_length(chunk.get()) == 4096 &&
                 std::memcmp(xcb_get_property_value(chunk.get()),
                             old_payload.data() + offset, 4096) == 0,
             "actual A reaches exact final nonzero chunk, not terminal ACK");
    }
  }
  own(editor.value, editor_window, XCB_ATOM_PRIMARY);
  set_active_window(editor.value, editor.screen, editor_window);
  const auto context =
      manager->get_selection_identity(punto::Selection::Primary);
  expect(context && context->owner == editor_window,
         "real editor fence for deferred ACK");
  const auto receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard, *context);
  expect(receipt.has_value(),
         "arm context-bound conversion behind accepted raw A");
  manager->activate_paste_receipt(*receipt, Clock::now() + 1s);
  xcb_convert_selection(editor.value, editor_window, clipboard, utf8,
                        editor_property, XCB_CURRENT_TIME);
  barrier(editor.value);
  acknowledge();
  if (scenario == "replacement") {
    xcb_convert_selection(peer.value, window, clipboard, utf8, property,
                          XCB_CURRENT_TIME);
    barrier(peer.value);
    auto error = xcb_ptr(xcb_request_check(
        peer.value,
        xcb_change_property_checked(peer.value, XCB_PROP_MODE_REPLACE, window,
                                    property, utf8, 8, 1, "x")));
    expect(!error, "real scratch property enables second actual old-A delete");
    acknowledge();
  }
  expect(manager->pump_events() <= 64,
         "context-fenced public pump remains bounded");
  barrier(editor.value);
  barrier(peer.value);
  std::optional<xcb_selection_notify_event_t> editor_notify;
  while (auto event = xcb_ptr(xcb_poll_for_event(editor.value))) {
    if ((event->response_type & 0x7fU) != XCB_SELECTION_NOTIFY)
      continue;
    expect(!editor_notify, "one first-pump editor notification");
    editor_notify =
        *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
  }
  expect(editor_notify && editor_notify->requestor == editor_window &&
             editor_notify->selection == clipboard &&
             editor_notify->target == utf8 &&
             editor_notify->time == XCB_CURRENT_TIME &&
             editor_notify->property == editor_property &&
             Access::outgoing_transfer_id(*manager, window, property) ==
                 old_id &&
             Access::deferred_ack_count(*manager, old_id) ==
                 (scenario == "replacement" ? 2U : 1U) &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Pending &&
             manager->paste_receipt_request_seq(*receipt) == 0,
         "first pump serves real editor header and actually parks ACKs on old "
         "A ID");

  if (scenario == "recreated-window") {
    auto error = xcb_ptr(xcb_request_check(
        peer.value, xcb_destroy_window_checked(peer.value, window)));
    expect(!error, "destroy actual raw requestor after its ACK is deferred");
    const auto *screen = screen_at(peer.value, peer.screen);
    const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    error = xcb_ptr(xcb_request_check(
        peer.value, xcb_create_window_checked(
                        peer.value, XCB_COPY_FROM_PARENT, window, screen->root,
                        0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                        screen->root_visual, XCB_CW_EVENT_MASK, &mask)));
    expect(!error, "recreate EXACT same XID with actual checked CreateWindow");
    error = xcb_ptr(xcb_request_check(
        peer.value,
        xcb_change_property_checked(
            peer.value, XCB_PROP_MODE_REPLACE, window, property, utf8, 8,
            static_cast<std::uint32_t>(sentinel.size()), sentinel.data())));
    expect(!error, "install actual replacement-window no-write sentinel");
    barrier(peer.value);
    expect(manager->pump_events() <= 64,
           "recreated requestor fence remains bounded");
    barrier(peer.value);
    expect(read_property(peer.value, window, property, utf8, false) ==
                   sentinel &&
               Access::outgoing_transfer_id(*manager, window, property) == 0 &&
               manager->is_open(),
           "old ACK cannot write into recreated same-XID requestor; publisher "
           "stays healthy");
  } else {
    std::optional<punto::PasteReceiptToken> newer_receipt;
    if (scenario == "foreign-owner") {
      own(foreign.value, foreign_window, clipboard);
    } else if (scenario == "new-generation") {
      publish(fresh_payload);
      expect(
          manager->selection_generation(punto::Selection::Clipboard) !=
              old_generation,
          "new local generation is actually published while raw A is accepted");
      set_active_window(peer.value, peer.screen, window);
      newer_receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
      expect(newer_receipt.has_value(),
             "arm new receipt before old A completion");
      manager->activate_paste_receipt(*newer_receipt, Clock::now() + 1s);
    } else {
      expect(scenario == "replacement", "known deferred INCR scenario");
      expect(manager->pump_events() <= 64, "A terminal ACK serviced once");
      barrier(peer.value);
      const auto terminal = value();
      expect(terminal->type == utf8 && terminal->format == 8 &&
                 xcb_get_property_value_length(terminal.get()) == 0 &&
                 Access::outgoing_transfer_id(*manager, window, property) == 0,
             "first old ACK completes A before same-property B is admitted");
      expect(manager->pump_events() <= 64,
             "queued same-property B serviced once");
      barrier(peer.value);
      std::optional<xcb_selection_notify_event_t> replacement;
      while (auto event = xcb_ptr(xcb_poll_for_event(peer.value))) {
        if ((event->response_type & 0x7fU) == XCB_SELECTION_NOTIFY) {
          const auto notify =
              *reinterpret_cast<xcb_selection_notify_event_t *>(event.get());
          if (notify.requestor == window && notify.property == property) {
            expect(!replacement && notify.selection == clipboard &&
                       notify.target == utf8 && notify.time == XCB_CURRENT_TIME,
                   "exact B notification, no duplicate");
            replacement = notify;
          }
        }
      }
      expect(replacement.has_value(),
             "real queued same-property B receives header");
      header(value());
      const auto replacement_id =
          Access::outgoing_transfer_id(*manager, window, property);
      expect(replacement_id != 0 && replacement_id != old_id &&
                 Access::deferred_ack_count(*manager, old_id) == 1,
             "B has new immutable ID while second real ACK still names A");
      expect(manager->pump_events() <= 64, "stale second A ACK serviced once");
      barrier(peer.value);
      header(value());
      expect(Access::outgoing_transfer_id(*manager, window, property) ==
                 replacement_id,
             "old A ACK cannot advance replacement B on same window/property");
      acknowledge();
    }
    const auto draining_id =
        Access::outgoing_transfer_id(*manager, window, property);
    std::vector<std::uint8_t> bytes;
    bool completed = false;
    const auto deadline = Clock::now() + 1s;
    for (std::size_t pumps = 0; pumps < 64 && Clock::now() < deadline;
         ++pumps) {
      expect(manager->pump_events() <= 64,
             "raw ACK continuation remains bounded");
      barrier(peer.value);
      const auto chunk = value();
      if (chunk->type == XCB_ATOM_NONE) {
        if (Access::outgoing_transfer_id(*manager, window, property) == 0)
          break;
        continue;
      }
      const int length = xcb_get_property_value_length(chunk.get());
      expect(chunk->type == utf8 && chunk->format == 8 && length >= 0 &&
                 bytes.size() + static_cast<std::size_t>(length) <=
                     old_payload.size() &&
                 (length == 0 ||
                  std::memcmp(xcb_get_property_value(chunk.get()),
                              old_payload.data() + bytes.size(),
                              static_cast<std::size_t>(length)) == 0),
             "EVERY raw chunk retains captured old bytes, never new "
             "publication payload");
      const auto *data = static_cast<const std::uint8_t *>(
          xcb_get_property_value(chunk.get()));
      if (length > 0)
        bytes.insert(bytes.end(), data, data + length);
      if (newer_receipt)
        expect(manager->paste_receipt_status(*newer_receipt) ==
                       punto::PasteReceiptStatus::Pending &&
                   manager->paste_receipt_request_seq(*newer_receipt) == 0,
               "old raw chunks and terminal ACK cannot confirm newer receipt");
      if (length == 0) {
        completed = true;
        break;
      }
      acknowledge();
    }
    expect(manager->is_open() && completed &&
               bytes == std::vector<std::uint8_t>(old_payload.begin(),
                                                  old_payload.end()),
           "accepted raw transfer completes exact immutable bytes and zero "
           "terminator without closing publisher");
    expect(draining_id != 0,
           "completion belongs to an actual accepted transfer");
    if (scenario == "replacement")
      expect(manager->paste_receipt_status(*receipt) ==
                     punto::PasteReceiptStatus::Pending &&
                 manager->paste_receipt_request_seq(*receipt) == 0,
             "foreign same-property B never confirms editor's pending receipt");
    if (scenario == "foreign-owner") {
      auto actual_owner = xcb_ptr(xcb_get_selection_owner_reply(
          peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
      expect(actual_owner && actual_owner->owner == foreign_window,
             "raw ACK service never reclaims actual foreign clipboard owner");
      std::exception_ptr provider_error;
      std::jthread provider{[&] {
        try {
          serve_one_text_request(foreign.value, "foreign INCR baseline");
        } catch (...) {
          provider_error = std::current_exception();
        }
      }};
      const auto text = manager->get_text(punto::Selection::Clipboard);
      provider.join();
      if (provider_error)
        std::rethrow_exception(provider_error);
      expect(text == std::optional<std::string>{"foreign INCR baseline"},
             "actual foreign clipboard bytes survive old raw ACK service");
      actual_owner = xcb_ptr(xcb_get_selection_owner_reply(
          peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
      expect(actual_owner && actual_owner->owner == foreign_window,
             "foreign owner remains untouched after exact byte readback");
      own(foreign.value, XCB_WINDOW_NONE, clipboard);
    }
  }
  manager->cancel_paste_receipt(*receipt);
  publish(fresh_payload);
  set_active_window(peer.value, peer.screen, window);
  const auto fresh_receipt =
      manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(fresh_receipt.has_value(), "arm same-manager fresh INCR recovery");
  manager->activate_paste_receipt(*fresh_receipt, Clock::now() + 2s);
  expect(manager->paste_receipt_status(*fresh_receipt) ==
                 punto::PasteReceiptStatus::Pending &&
             manager->paste_receipt_request_seq(*fresh_receipt) == 0,
         "fresh recovery receipt remains pending before new conversion");
  const auto recovered = read_wire_payload(
      peer.value, window, *manager, clipboard, utf8, property, &*fresh_receipt);
  expect(recovered.type == utf8 && recovered.format == 8 &&
             recovered.bytes ==
                 std::vector<std::uint8_t>(fresh_payload.begin(),
                                           fresh_payload.end()) &&
             manager->paste_receipt_status(*fresh_receipt) ==
                 punto::PasteReceiptStatus::Delivered &&
             manager->paste_receipt_request_seq(*fresh_receipt) == 1 &&
             manager->is_open(),
         "same-manager fresh real INCR conversion recovers exact bytes and "
         "only its receipt");
  xcb_disconnect(foreign.value);
  xcb_disconnect(editor.value);
  xcb_disconnect(peer.value);
}

void test_raw_payload_roundtrip_and_incr_receipt() {
  using namespace std::chrono_literals;
  NestedXServer server = start_nested_xvfb();
  auto source = raw_manager(server.display.c_str());
  auto manager = raw_manager(server.display.c_str(), 2);
  auto peer = connect_to(server.display.c_str());
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto property = intern_atom(peer.value, "PUNTO_RAW_CONTRACT");
  auto payload = std::make_shared<std::vector<punto::SelectionData>>();
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  payload->push_back({utf8, utf8, 8, {'A', 0, 'B'}});
  const auto html = intern_atom(peer.value, "text/html");
  payload->push_back({html, utf8, 8, {'<', 'b', '>', 0, 'X'}});
  const auto png = intern_atom(peer.value, "image/png");
  payload->push_back({png, png, 8, std::vector<std::uint8_t>(16384, 0xa5)});
  const auto jpeg = intern_atom(peer.value, "image/jpeg");
  payload->push_back({jpeg, jpeg, 8, {0xff, 0xd8, 0, 0xff}});
  const auto opaque = intern_atom(peer.value, "application/octet-stream");
  std::vector<std::uint8_t> words32(4116), words16(4102);
  for (std::size_t i = 0; i < words32.size(); ++i)
    words32[i] = static_cast<std::uint8_t>(i * 17U);
  for (std::size_t i = 0; i < words16.size(); ++i)
    words16[i] = static_cast<std::uint8_t>(i * 31U);
  payload->push_back({opaque, XCB_ATOM_INTEGER, 32, std::move(words32)});
  const auto custom =
      intern_atom(peer.value, "application/x-punto-contract-bytes;format=test");
  payload->push_back({custom, XCB_ATOM_INTEGER, 16, std::move(words16)});
  const auto compound = intern_atom(peer.value, "COMPOUND_TEXT");
  payload->push_back({compound, utf8, 8, {'C', 0, 'D'}});
  const auto empty =
      source->get_selection_identity(punto::Selection::Clipboard);
  expect(empty.has_value(), "real source origin");
  const auto source_publication =
      source->set_payload_if_owner(punto::Selection::Clipboard, *empty, payload,
                                   std::chrono::steady_clock::now() + 1s);
  expect(source_publication.completed, "publish real mixed source");
  std::exception_ptr source_error;
  std::jthread provider{[&](std::stop_token stop) {
    try {
      while (!stop.stop_requested()) {
        (void)source->pump_events();
        expect(source->is_open(), "source provider connection remains healthy");
        std::this_thread::sleep_for(1ms);
      }
    } catch (...) {
      source_error = std::current_exception();
    }
  }};
  const auto saved = manager->capture_payload(
      punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
  provider.request_stop();
  provider.join();
  if (source_error)
    std::rethrow_exception(source_error);
  expect(saved && saved->payload && saved->payload->size() == payload->size(),
         "complete real incoming snapshot, including INCR");
  for (std::size_t i = 0; i < payload->size(); ++i) {
    const auto &got = saved->payload->at(i);
    const auto &want = payload->at(i);
    expect(got.target == want.target && got.type == want.type &&
               got.format == want.format && got.bytes == want.bytes,
           "exact captured raw bytes");
  }
  const std::string replacement(8192, 'r');
  const auto publication =
      manager->set_payload_if_owner(punto::Selection::Clipboard, saved->origin,
                                    manager->text_payload(replacement),
                                    std::chrono::steady_clock::now() + 1s);
  expect(publication.submitted && publication.completed,
         "raw temporary publication");
  set_active_window(peer.value, peer.screen, window);
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "arm actual requestor receipt");
  manager->activate_paste_receipt(*receipt,
                                  std::chrono::steady_clock::now() + 2s);
  const auto delivered = read_wire_payload(
      peer.value, window, *manager, clipboard, utf8, property, &*receipt);
  expect(delivered.bytes == std::vector<std::uint8_t>(replacement.begin(),
                                                      replacement.end()) &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Delivered &&
             manager->paste_receipt_request_seq(*receipt) == 1,
         "one completed INCR text receipt, never header-only");
  // A slow application may finish after the publication-loss witness expires.
  // The live owner generation, not that expired recovery proof, admits restore.
  std::this_thread::sleep_for(2100ms);
  expect(manager->restore_payload_if_generation(punto::Selection::Clipboard,
                                                publication.generation, *saved),
         "restore complete raw baseline");
  const auto targets =
      read_wire_payload(peer.value, window, *manager, clipboard,
                        intern_atom(peer.value, "TARGETS"), property);
  std::vector<xcb_atom_t> actual(targets.bytes.size() / 4U), expected;
  std::memcpy(actual.data(), targets.bytes.data(), targets.bytes.size());
  expected.push_back(intern_atom(peer.value, "TARGETS"));
  expected.push_back(intern_atom(peer.value, "TIMESTAMP"));
  for (const auto &data : *payload)
    expected.push_back(data.target);
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  expect(actual == expected && targets.type == XCB_ATOM_ATOM &&
             targets.format == 32,
         "exact restored data-target set plus regenerated metadata");
  for (const auto &want : *payload) {
    const auto got = read_wire_payload(peer.value, window, *manager, clipboard,
                                       want.target, property);
    expect(got.type == want.type && got.format == want.format &&
               got.bytes == want.bytes,
           "exact restored independent wire bytes/type/format");
  }
  xcb_disconnect(peer.value);
}

void test_observational_owner_timeout_preserves_publisher() {
  using namespace std::chrono_literals;
  using Clock = std::chrono::steady_clock;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str());
  auto peer = connect_to(server.display.c_str());
  auto peer_owner = Access::adopt(peer.value, peer.screen);
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto property = intern_atom(peer.value, "PUNTO_OBSERVATION_TIMEOUT");
  auto payload = std::make_shared<std::vector<punto::SelectionData>>();
  for (const auto name :
       {"UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "STRING",
        "text/html", "application/x-punto-observation"}) {
    const auto target = intern_atom(peer.value, name);
    const auto index = static_cast<std::uint8_t>(payload->size());
    payload->push_back({target, target, 8, {index, 0, 'z'}});
  }
  const auto empty =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(empty && empty->owner == XCB_WINDOW_NONE, "private baseline is empty");
  const auto published = manager->set_payload_if_owner(
      punto::Selection::Clipboard, *empty, payload, Clock::now() + 1s);
  expect(published.completed,
         "publish all six actual baseline representations");
  manager->cancel_publication_witness(published);

  for (unsigned int operation = 0; operation < 3; ++operation) {
    Access::request_timeout(*manager, 500ms);
    expect(manager->set_text(punto::Selection::Primary, "selected") ==
               punto::ClipboardResult::Ok,
           "publish the independent selection being observed");
    const auto selected =
        manager->get_selection_identity(punto::Selection::Primary);
    expect(selected.has_value(), "capture exact observation authority");
    const auto generation =
        manager->selection_generation(punto::Selection::Primary);
    const auto state = Access::close_state(*manager);
    Access::request_timeout(*manager, 10ms);
    observational_timeout_fault::connection = Access::connection(*manager);
    observational_timeout_fault::selection = XCB_ATOM_PRIMARY;
    observational_timeout_fault::consumed = 0;
    observational_timeout_fault::armed = true;
    const auto started = Clock::now();
    bool unknown = false;
    if (operation == 0)
      unknown = !manager->get_selection_identity(punto::Selection::Primary,
                                                 started + 10ms);
    else if (operation == 1)
      unknown = !manager->selection_identity_matches(punto::Selection::Primary,
                                                     *selected, started + 10ms);
    else
      unknown =
          !manager->owns_generation(punto::Selection::Primary, generation);
    const auto elapsed = Clock::now() - started;
    const auto consumed = observational_timeout_fault::consumed;
    observational_timeout_fault::connection = nullptr;
    observational_timeout_fault::armed = false;
    Access::request_timeout(*manager, 500ms);
    expect(consumed == 1 && unknown && elapsed >= 40ms && elapsed < 1s,
           "real owner-query deadline exhaustion returns unknown");
    expect(
        manager->is_open() && Access::close_state(*manager) == state,
        "observational timeout preserves publisher connection and generations");
    for (const auto &want : *payload) {
      const auto got = read_wire_payload(peer.value, window, *manager,
                                         clipboard, want.target, property);
      expect(got.type == want.type && got.format == want.format &&
                 got.bytes == want.bytes,
             "observational timeout preserves every actual baseline wire byte");
    }
    const auto unchanged =
        manager->get_selection_identity(punto::Selection::Primary);
    expect(unchanged && unchanged->owner == selected->owner &&
               unchanged->connection_generation ==
                   selected->connection_generation &&
               manager->selection_identity_matches(punto::Selection::Primary,
                                                   *selected) &&
               manager->owns_generation(punto::Selection::Primary, generation),
           "fresh observations recover without publisher replacement");
    auto takeover_error = xcb_ptr(xcb_request_check(
        peer.value,
        xcb_set_selection_owner_checked(peer.value, window, XCB_ATOM_PRIMARY,
                                        XCB_CURRENT_TIME)));
    expect(!takeover_error, "change observed selection after discarded query");
    const auto fresh =
        manager->get_selection_identity(punto::Selection::Primary);
    expect(fresh && fresh->owner == window &&
               fresh->connection_generation ==
                   selected->connection_generation &&
               !manager->selection_identity_matches(punto::Selection::Primary,
                                                    *selected),
           "fresh query sees new owner rather than the expired buffered reply");
    const auto got = read_wire_payload(peer.value, window, *manager, clipboard,
                                       payload->front().target, property);
    expect(got.bytes == payload->front().bytes,
           "clipboard peer reads still recover after observed owner change");
  }
}

void test_dead_requestor_preserves_publisher() {
  using namespace std::chrono_literals;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  for (const auto target_name :
       {"UTF8_STRING", "TARGETS", "TIMESTAMP", "UNSUPPORTED", "INCR_HEADER",
        "INCR_CHUNK", "WRONG_RESOURCE", "WRONG_OPCODE", "WRONG_SEQUENCE",
        "WRONG_CODE"}) {
    auto manager = raw_manager(server.display.c_str(), 2);
    auto peer = connect_to(server.display.c_str());
    const auto window = create_window(peer.value, peer.screen);
    const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
    const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
    const auto property = intern_atom(peer.value, "PUNTO_DEAD_REQUESTOR");
    const std::string_view name{target_name};
    const bool incremental = name.starts_with("INCR_");
    const bool corrupted = name.starts_with("WRONG_");
    const auto target =
        incremental || corrupted ? utf8 : intern_atom(peer.value, name);
    expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
               punto::ClipboardResult::Ok,
           "publish baseline before real dead-window conversion");
    const auto saved = manager->capture_payload(
        punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
    expect(saved && saved->payload,
           "capture exact baseline before replacement");
    const std::string replacement(incremental ? 8192U : 7U, 'r');
    const auto publication = manager->set_payload_if_owner(
        punto::Selection::Clipboard, saved->origin,
        manager->text_payload(replacement),
        std::chrono::steady_clock::now() + 1s);
    expect(publication.completed, "publish replacement for peer-death control");
    const auto generation =
        manager->selection_generation(punto::Selection::Clipboard);
    const auto connection = Access::connection(*manager);
    const auto publisher = Access::window(*manager);
    set_active_window(peer.value, peer.screen, window);
    const auto receipt =
        manager->arm_paste_receipt(punto::Selection::Clipboard);
    expect(receipt.has_value(), "bind receipt to actual peer client");
    manager->activate_paste_receipt(*receipt,
                                    std::chrono::steady_clock::now() + 2s);
    const auto request_sequence =
        manager->selection_request_seq(punto::Selection::Clipboard);
    if (name == "INCR_CHUNK") {
      const auto notify = request_selection(peer.value, window, *manager,
                                            clipboard, target, property);
      expect(notify.property == property,
             "real INCR header precedes dead peer");
      const auto header = xcb_ptr(xcb_get_property_reply(
          peer.value,
          xcb_get_property(peer.value, 0, window, property,
                           XCB_GET_PROPERTY_TYPE_ANY, 0, 1),
          nullptr));
      expect(header && header->type == intern_atom(peer.value, "INCR"),
             "actual INCR handshake");
      auto error = xcb_ptr(xcb_request_check(
          peer.value,
          xcb_delete_property_checked(peer.value, window, property)));
      expect(!error, "actual INCR header ACK queued before destruction");
    } else {
      auto error = xcb_ptr(xcb_request_check(
          peer.value,
          xcb_convert_selection_checked(peer.value, window, clipboard, target,
                                        property, XCB_CURRENT_TIME)));
      expect(!error,
             "real queued conversion reaches the server before destruction");
    }
    auto destroy_error = xcb_ptr(xcb_request_check(
        peer.value, xcb_destroy_window_checked(peer.value, window)));
    expect(!destroy_error,
           "server confirms requestor destruction before response");
    peer_response_fault::connection = connection;
    peer_response_fault::requestor = window;
    peer_response_fault::actual_bad_windows = 0;
    using Corruption = peer_response_fault::Corruption;
    peer_response_fault::corruption =
        name == "WRONG_RESOURCE"   ? Corruption::Resource
        : name == "WRONG_OPCODE"   ? Corruption::Opcode
        : name == "WRONG_SEQUENCE" ? Corruption::Sequence
        : name == "WRONG_CODE"     ? Corruption::Code
                                   : Corruption::None;
    const auto close_state = Access::close_state(*manager);
    StderrCapture diagnostics;
    (void)manager->pump_events();
    const auto close_record = diagnostics.finish();
    const auto errors = peer_response_fault::actual_bad_windows;
    peer_response_fault::connection = nullptr;
    peer_response_fault::corruption = Corruption::None;
    expect(errors > 0,
           "actual server BadWindow is consumed, not a mocked disappearance");
    std::cout << "dead-requestor target=" << name << " actual_errors=" << errors
              << '\n';
    if (corrupted) {
      expect(
          !manager->is_open(),
          "unattributed response error cannot preserve publication authority");
      expect_close_diagnostic(close_record, true, close_state,
                              "complete_peer_requests");
      std::cout << "close-diagnostic target=" << name << ' ' << close_record;
      const auto witness_states = witness_close_states(*manager);
      StderrCapture repeated;
      manager->close();
      manager.reset();
      const auto cleanup_record = repeated.finish();
      expect_witness_close_diagnostics(cleanup_record, witness_states);
      xcb_disconnect(peer.value);
      continue;
    }
    expect(close_record.find("[punto] Clipboard close ") == std::string::npos,
           "attributed dead peer produces no publisher-close diagnostic");
    expect(manager->is_open() && Access::connection(*manager) == connection &&
               Access::window(*manager) == publisher &&
               manager->selection_generation(punto::Selection::Clipboard) ==
                   generation,
           "dead peer does not destroy or reconnect the publisher");
    if (name == "UTF8_STRING" || name == "TARGETS" || name == "TIMESTAMP") {
      expect(errors == 2,
             "both actual property and notify errors are consumed");
    }
    if (name == "INCR_CHUNK") {
      expect(errors == 1,
             "known-dead INCR requestor receives no further cleanup writes");
    }
    expect(manager->paste_receipt_status(*receipt) ==
                   punto::PasteReceiptStatus::Pending &&
               manager->paste_receipt_request_seq(*receipt) == 0 &&
               manager->selection_request_seq(punto::Selection::Clipboard) ==
                   request_sequence,
           "failed response never confirms complete delivery");
    const auto owner = xcb_ptr(xcb_get_selection_owner_reply(
        peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
    expect(owner && owner->owner == publisher,
           "actual publisher ownership survives peer death");
    const auto fresh_window = create_window(peer.value, peer.screen);
    const auto fresh = read_wire_payload(peer.value, fresh_window, *manager,
                                         clipboard, utf8, property);
    expect(fresh.type == utf8 && fresh.format == 8 &&
               fresh.bytes == std::vector<std::uint8_t>(replacement.begin(),
                                                        replacement.end()),
           "fresh requestor receives exact data without manager restart");
    expect(manager->restore_payload_if_generation(punto::Selection::Clipboard,
                                                  generation, *saved),
           "baseline remains restorable after failed peer response");
    for (const auto &want : *saved->payload) {
      const auto got = read_wire_payload(peer.value, fresh_window, *manager,
                                         clipboard, want.target, property);
      expect(got.type == want.type && got.format == want.format &&
                 got.bytes == want.bytes,
             "all baseline bytes and representations survive peer death");
    }
    (void)manager->pump_events();
    expect(manager->is_open(),
           "no unconsumed checked error closes the recovered publisher");
    xcb_disconnect(peer.value);
  }
}

void test_normal_close_diagnostic() {
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str());
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
                 punto::ClipboardResult::Ok &&
             manager->set_text(punto::Selection::Primary, "private-primary") ==
                 punto::ClipboardResult::Ok,
         "publish both real selections before explicit close");
  const auto state = Access::close_state(*manager);
  StderrCapture diagnostics;
  const auto line = std::source_location::current().line() + 1;
  manager->close();
  expect(!manager->is_open(), "normal close releases actual publisher");
  const auto record = diagnostics.finish();
  const auto witness_states = witness_close_states(*manager);
  StderrCapture cleanup;
  manager->close();
  manager.reset();
  const auto cleanup_record = cleanup.finish();
  expect_witness_close_diagnostics(cleanup_record, witness_states);
  expect_close_diagnostic(record, false, state, "test_normal_close_diagnostic",
                          line);
  expect(record.find("private-primary") == std::string::npos,
         "PRIMARY data never enters close diagnostics");
  std::cout << "close-diagnostic explicit " << record;
}

void test_expired_peer_ack_preserves_completed_publisher() {
  using namespace std::chrono_literals;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  for (const auto name :
       {"UTF8_STRING", "TARGETS", "TIMESTAMP", "UNSUPPORTED"}) {
    auto manager = raw_manager(server.display.c_str());
    auto peer = connect_to(server.display.c_str());
    const auto window = create_window(peer.value, peer.screen);
    const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
    const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
    const auto target = intern_atom(peer.value, name);
    const auto property = intern_atom(peer.value, "PUNTO_EXPIRED_PEER_ACK");
    expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
               punto::ClipboardResult::Ok,
           "publish actual baseline before expired peer ACK");
    const auto saved = manager->capture_payload(
        punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
    expect(saved && saved->payload,
           "save every actual baseline representation");
    const auto generation =
        manager->selection_generation(punto::Selection::Clipboard);
    const auto publisher = Access::window(*manager);
    const auto connection = Access::connection(*manager);
    set_active_window(peer.value, peer.screen, window);
    const auto receipt =
        manager->arm_paste_receipt(punto::Selection::Clipboard);
    expect(receipt.has_value(), "arm receipt for actual peer client");
    manager->activate_paste_receipt(*receipt,
                                    std::chrono::steady_clock::now() + 2s);
    const auto sequence =
        manager->selection_request_seq(punto::Selection::Clipboard);
    peer_response_fault::connection = connection;
    peer_response_fault::actual_delayed_barriers = 0;
    peer_response_fault::actual_completed_checks = 0;
    peer_response_fault::delay_next_barrier = true;
    Access::request_timeout(*manager, 10ms);
    StderrCapture diagnostics;
    const auto notify = request_selection(peer.value, window, *manager,
                                          clipboard, target, property);
    const auto close_record = diagnostics.finish();
    expect(close_record.find("[punto] Clipboard close ") == std::string::npos,
           "completed expired peer ACK does not emit a fail-close diagnostic");
    const bool unsupported = std::string_view{name} == "UNSUPPORTED";
    expect(
        notify.property ==
            (unsupported ? static_cast<xcb_atom_t>(XCB_ATOM_NONE) : property),
        "real peer receives actual response before delayed local ACK check");
    if (!unsupported) {
      auto *actual = xcb_get_property_reply(
          peer.value,
          xcb_get_property(peer.value, 0, window, property,
                           XCB_GET_PROPERTY_TYPE_ANY, 0, 1024),
          nullptr);
      expect(actual && actual->type != XCB_ATOM_NONE && actual->format != 0 &&
                 xcb_get_property_value_length(actual) > 0,
             "real response property exists on the actual peer");
      std::free(actual);
    }
    expect(
        peer_response_fault::actual_delayed_barriers == 1,
        "actual server barrier precedes deadline-exceeding scheduling delay");
    expect(
        manager->is_open() && Access::connection(*manager) == connection &&
            Access::window(*manager) == publisher &&
            manager->selection_generation(punto::Selection::Clipboard) ==
                generation,
        "expired response ACK must not destroy a completed healthy publisher");
    expect(peer_response_fault::actual_completed_checks ==
               (unsupported ? 1U : 2U),
           "every actual checked response result is consumed after ACK expiry");
    peer_response_fault::connection = nullptr;
    peer_response_fault::actual_delayed_barriers = 0;
    expect(
        manager->paste_receipt_status(*receipt) ==
                punto::PasteReceiptStatus::Pending &&
            manager->selection_request_seq(punto::Selection::Clipboard) ==
                sequence,
        "expired response cannot acknowledge delivery or increment sequence");
    (void)manager->pump_events();
    expect(manager->paste_receipt_status(*receipt) ==
               punto::PasteReceiptStatus::Pending,
           "later pump cannot backfill a late receipt");
    manager->cancel_paste_receipt(*receipt);
    const auto fresh_window = create_window(peer.value, peer.screen);
    const auto fresh = read_wire_payload(peer.value, fresh_window, *manager,
                                         clipboard, utf8, property);
    expect(fresh.type == utf8 && fresh.format == 8 &&
               fresh.bytes == std::vector<std::uint8_t>{'b', 'a', 's', 'e', 'l',
                                                        'i', 'n', 'e'},
           "fresh reader obtains exact baseline without publisher restart");
    expect(manager->restore_payload_if_generation(punto::Selection::Clipboard,
                                                  generation, *saved),
           "conditional restoration remains possible after expired peer ACK");
    for (const auto &want : *saved->payload) {
      const auto got = read_wire_payload(peer.value, fresh_window, *manager,
                                         clipboard, want.target, property);
      expect(got.type == want.type && got.format == want.format &&
                 got.bytes == want.bytes,
             "all baseline types, formats and bytes survive expired peer ACK");
    }
    xcb_disconnect(peer.value);
  }
}

void test_expired_incr_ack_retires_live_watch() {
  using namespace std::chrono_literals;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  for (const auto stage : {"WATCH", "HEADER", "CHUNK"}) {
    auto manager = raw_manager(server.display.c_str(), 2);
    auto peer = connect_to(server.display.c_str());
    const auto window = create_window(peer.value, peer.screen);
    const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
    const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
    const auto property = intern_atom(peer.value, "PUNTO_EXPIRED_INCR_ACK");
    const std::string payload(8192, 'r');
    expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
               punto::ClipboardResult::Ok,
           "publish real baseline before late ACK");
    const auto saved = manager->capture_payload(
        punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
    expect(saved && saved->payload,
           "capture actual baseline for INCR publication");
    const auto publication = manager->set_payload_if_owner(
        punto::Selection::Clipboard, saved->origin,
        manager->text_payload(payload), std::chrono::steady_clock::now() + 1s);
    expect(publication.completed, "publish real INCR payload before late ACK");
    const auto connection = Access::connection(*manager);
    const auto publisher = Access::window(*manager);
    const auto generation =
        manager->selection_generation(punto::Selection::Clipboard);
    set_active_window(peer.value, peer.screen, window);
    const auto receipt =
        manager->arm_paste_receipt(punto::Selection::Clipboard);
    expect(receipt.has_value(), "arm actual INCR peer receipt");
    manager->activate_paste_receipt(*receipt,
                                    std::chrono::steady_clock::now() + 2s);
    const auto sequence =
        manager->selection_request_seq(punto::Selection::Clipboard);
    const bool chunk = std::string_view{stage} == "CHUNK";
    const auto watch_mask = [&] {
      const auto attributes = xcb_ptr(xcb_get_window_attributes_reply(
          connection, xcb_get_window_attributes(connection, window), nullptr));
      expect(attributes != nullptr, "read actual manager-owned event mask");
      return attributes->your_event_mask;
    };
    expect(watch_mask() == 0, "peer starts without a manager subscription");
    if (chunk) {
      const auto notify = request_selection(peer.value, window, *manager,
                                            clipboard, utf8, property);
      expect(notify.property == property, "real INCR header precedes chunk");
      const auto header = xcb_ptr(xcb_get_property_reply(
          peer.value,
          xcb_get_property(peer.value, 0, window, property,
                           XCB_GET_PROPERTY_TYPE_ANY, 0, 1),
          nullptr));
      expect(header && header->type == intern_atom(peer.value, "INCR"),
             "actual incremental header, not an injected transfer");
    }
    peer_response_fault::connection = connection;
    peer_response_fault::actual_delayed_barriers = 0;
    peer_response_fault::actual_completed_checks = 0;
    peer_response_fault::barriers_before_delay =
        std::string_view{stage} == "HEADER" ? 1U : 0U;
    peer_response_fault::delay_next_barrier = true;
    Access::request_timeout(*manager, 10ms);
    const auto queued =
        chunk ? xcb_delete_property_checked(peer.value, window, property)
              : xcb_convert_selection_checked(peer.value, window, clipboard,
                                              utf8, property, XCB_CURRENT_TIME);
    expect(!xcb_ptr(xcb_request_check(peer.value, queued)),
           "actual peer request reaches X server before single response pump");
    (void)manager->pump_events();
    expect(peer_response_fault::actual_delayed_barriers == 1 &&
               peer_response_fault::actual_completed_checks ==
                   (std::string_view{stage} == "HEADER" ? 2U : 1U),
           "server-completed response precedes deadline-exceeding local ACK");
    peer_response_fault::connection = nullptr;
    peer_response_fault::actual_delayed_barriers = 0;
    expect(manager->is_open() && Access::connection(*manager) == connection &&
               Access::window(*manager) == publisher &&
               manager->selection_generation(punto::Selection::Clipboard) ==
                   generation,
           "expired INCR ACK preserves same healthy publication authority");
    expect(watch_mask() == (XCB_EVENT_MASK_PROPERTY_CHANGE |
                            XCB_EVENT_MASK_STRUCTURE_NOTIFY),
           "late ACK leaves an actual live watch pending bounded retirement");
    const auto before = xcb_ptr(xcb_get_property_reply(
        peer.value,
        xcb_get_property(peer.value, 0, window, property,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 4096),
        nullptr));
    expect(
        before && before->type == (std::string_view{stage} == "WATCH"
                                       ? static_cast<xcb_atom_t>(XCB_ATOM_NONE)
                                   : chunk ? utf8
                                           : intern_atom(peer.value, "INCR")),
        "watch expiry sends no header; header/chunk are real completed data");
    (void)manager->pump_events();
    expect(watch_mask() == 0,
           "next bounded idle pump must retire the expired live INCR watch");
    const auto after = xcb_ptr(xcb_get_property_reply(
        peer.value,
        xcb_get_property(peer.value, 0, window, property,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 4096),
        nullptr));
    expect(
        after && after->type == before->type &&
            after->format == before->format &&
            xcb_get_property_value_length(after.get()) ==
                xcb_get_property_value_length(before.get()) &&
            std::memcmp(xcb_get_property_value(after.get()),
                        xcb_get_property_value(before.get()),
                        static_cast<std::size_t>(
                            xcb_get_property_value_length(before.get()))) == 0,
        "watch retirement cannot resume payload writes or send a terminator");
    expect(!xcb_ptr(xcb_request_check(
               peer.value,
               xcb_delete_property_checked(peer.value, window, property))),
           "peer ACK after retirement is an actual delete");
    (void)manager->pump_events();
    const auto absent = xcb_ptr(xcb_get_property_reply(
        peer.value,
        xcb_get_property(peer.value, 0, window, property,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 1),
        nullptr));
    expect(absent && absent->type == XCB_ATOM_NONE &&
               manager->paste_receipt_status(*receipt) ==
                   punto::PasteReceiptStatus::Pending &&
               manager->selection_request_seq(punto::Selection::Clipboard) ==
                   sequence,
           "late peer ACK cannot backfill chunks, delivery or sequence");
    manager->cancel_paste_receipt(*receipt);
    Access::request_timeout(*manager, 500ms);
    const auto fresh_window = create_window(peer.value, peer.screen);
    const auto fresh = read_wire_payload(peer.value, fresh_window, *manager,
                                         clipboard, utf8, property);
    expect(fresh.type == utf8 && fresh.format == 8 &&
               fresh.bytes ==
                   std::vector<std::uint8_t>(payload.begin(), payload.end()),
           "fresh INCR reader recovers exact bytes without publisher restart");
    std::cout << "expired-incr-ack stage=" << stage << " retired-live-watch\n";
    xcb_disconnect(peer.value);
  }
}

void test_incr_watch_capacity_and_shared_peer() {
  using namespace std::chrono_literals;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str(), 2);
  auto peer = connect_to(server.display.c_str());
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const auto property = intern_atom(peer.value, "PUNTO_WATCH_CAPACITY");
  const auto second_property = intern_atom(peer.value, "PUNTO_SHARED_WATCH");
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
             punto::ClipboardResult::Ok,
         "baseline before bounded watches");
  const auto saved = manager->capture_payload(
      punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
  expect(saved && saved->payload, "baseline snapshot before bounded watches");
  const std::string payload(8192, 's');
  expect(manager
             ->set_payload_if_owner(punto::Selection::Clipboard, saved->origin,
                                    manager->text_payload(payload),
                                    std::chrono::steady_clock::now() + 1s)
             .completed,
         "publish bounded-watch INCR payload");
  const auto connection = Access::connection(*manager);
  const auto publisher = Access::window(*manager);
  const auto mask = [&](xcb_window_t window) {
    const auto value = xcb_ptr(xcb_get_window_attributes_reply(
        connection, xcb_get_window_attributes(connection, window), nullptr));
    expect(value != nullptr, "actual manager subscription query");
    return value->your_event_mask;
  };
  const auto window = create_window(peer.value, peer.screen);
  expect(
      request_selection(peer.value, window, *manager, clipboard, utf8, property)
                  .property == property &&
          request_selection(peer.value, window, *manager, clipboard, utf8,
                            second_property)
                  .property == second_property,
      "two actual INCR transfers share one native watch");
  const auto sequence =
      manager->selection_request_seq(punto::Selection::Clipboard);
  peer_response_fault::connection = connection;
  peer_response_fault::actual_delayed_barriers = 0;
  peer_response_fault::actual_completed_checks = 0;
  peer_response_fault::barriers_before_delay = 0;
  peer_response_fault::delay_next_barrier = true;
  Access::request_timeout(*manager, 10ms);
  expect(!xcb_ptr(xcb_request_check(
             peer.value,
             xcb_delete_property_checked(peer.value, window, property))),
         "actual first shared-peer chunk ACK");
  (void)manager->pump_events();
  expect(peer_response_fault::actual_delayed_barriers == 1 &&
             peer_response_fault::actual_completed_checks == 1,
         "actual expired chunk ACK for only first shared-peer transfer");
  peer_response_fault::connection = nullptr;
  peer_response_fault::actual_delayed_barriers = 0;
  Access::request_timeout(*manager, 500ms);
  (void)manager->pump_events();
  expect(mask(window) != 0,
         "other active same-peer transfer retains its subscription");
  std::vector<std::uint8_t> received;
  bool terminated = false;
  for (std::size_t chunk = 0; chunk < 8; ++chunk) {
    expect(!xcb_ptr(xcb_request_check(
               peer.value, xcb_delete_property_checked(peer.value, window,
                                                       second_property))),
           "real surviving shared-peer ACK");
    (void)manager->pump_events();
    const auto value = xcb_ptr(xcb_get_property_reply(
        peer.value,
        xcb_get_property(peer.value, 0, window, second_property,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 4096),
        nullptr));
    expect(value && value->type == utf8 && value->format == 8 &&
               value->bytes_after == 0,
           "surviving shared-peer transfer makes actual typed progress");
    const auto length = xcb_get_property_value_length(value.get());
    expect(length >= 0, "valid shared-peer chunk length");
    if (length == 0) {
      terminated = true;
      break;
    }
    const auto *bytes =
        static_cast<const std::uint8_t *>(xcb_get_property_value(value.get()));
    received.insert(received.end(), bytes, bytes + length);
  }
  expect(
      terminated &&
          received ==
              std::vector<std::uint8_t>(payload.begin(), payload.end()) &&
          manager->selection_request_seq(punto::Selection::Clipboard) ==
              sequence + 1 &&
          mask(window) == 0,
      "only surviving shared-peer transfer completes, then last watch retires");

  std::array<xcb_window_t, 5> windows{};
  for (auto &peer_window : windows)
    peer_window = create_window(peer.value, peer.screen);
  set_active_window(peer.value, peer.screen, windows.front());
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "receipt binds four actual same-client peers");
  const auto deadline = std::chrono::steady_clock::now() + 150ms;
  manager->activate_paste_receipt(*receipt, deadline);
  for (std::size_t index = 0; index < windows.size(); ++index) {
    const auto notify = request_selection(peer.value, windows[index], *manager,
                                          clipboard, utf8, property);
    expect(notify.property == (index < 4
                                   ? property
                                   : static_cast<xcb_atom_t>(XCB_ATOM_NONE)) &&
               (mask(windows[index]) != 0) == (index < 4),
           "fifth transfer rejected before installing an unreserved watch");
  }
  std::this_thread::sleep_until(deadline + 1ms);
  (void)manager->pump_events();
  for (std::size_t index = 0; index < 4; ++index)
    expect(mask(windows[index]) != 0,
           "expired transfer retains reserved cleanup duty");
  for (std::size_t remaining = 4; remaining > 0; --remaining) {
    (void)manager->pump_events();
    const auto watched = std::count_if(
        windows.begin(), windows.end(),
        [&](const auto peer_window) { return mask(peer_window) != 0; });
    expect(static_cast<std::size_t>(watched) == remaining - 1,
           "fresh idle pump retires exactly one of the four reserved duties");
  }
  expect(manager->is_open() && Access::connection(*manager) == connection &&
             Access::window(*manager) == publisher &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Expired,
         "capacity retirement keeps publisher but cannot complete expired "
         "receipt");
  manager->cancel_paste_receipt(*receipt);
  const auto fresh = read_wire_payload(peer.value, windows.back(), *manager,
                                       clipboard, utf8, property);
  expect(fresh.type == utf8 && fresh.format == 8 &&
             fresh.bytes ==
                 std::vector<std::uint8_t>(payload.begin(), payload.end()),
         "capacity reclaimed for fresh exact reader without restart");
  xcb_disconnect(peer.value);
}

void test_revoked_incr_cleanup_keeps_request_budget() {
  using namespace std::chrono_literals;
  using Access = punto::ClipboardManagerTestAccess;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str(), 2);
  auto peer = connect_to(server.display.c_str());
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const auto property = intern_atom(peer.value, "PUNTO_REVOKED_INCR_WATCH");
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
             punto::ClipboardResult::Ok,
         "baseline before revoked INCR");
  const auto saved = manager->capture_payload(
      punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
  expect(saved && saved->payload, "capture baseline before revoked INCR");
  const auto publication = manager->set_payload_if_owner(
      punto::Selection::Clipboard, saved->origin,
      manager->text_payload(std::string(8192, 'r')),
      std::chrono::steady_clock::now() + 1s);
  expect(publication.completed, "publish actual revoked INCR payload");
  set_active_window(peer.value, peer.screen, window);
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "arm real revoked INCR receipt");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  manager->activate_paste_receipt(*receipt, deadline);
  expect(
      request_selection(peer.value, window, *manager, clipboard, utf8, property)
              .property == property,
      "actual INCR header installs native watch before revocation");
  const auto connection = Access::connection(*manager);
  expect(!xcb_ptr(xcb_request_check(
             peer.value, xcb_set_selection_owner_checked(
                             peer.value, window, clipboard, XCB_CURRENT_TIME))),
         "actual foreign owner revokes publisher generation");
  (void)manager->pump_events();
  const auto attributes = xcb_ptr(xcb_get_window_attributes_reply(
      connection, xcb_get_window_attributes(connection, window), nullptr));
  expect(attributes && attributes->your_event_mask != 0 &&
             !manager->locally_owns_generation(punto::Selection::Clipboard,
                                               publication.generation) &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Invalid &&
             std::chrono::steady_clock::now() < deadline,
         "real revocation leaves live watch and future original transfer "
         "deadline");
  Access::request_timeout(*manager, 10ms);
  expect(::kill(server.pid, SIGSTOP) == 0,
         "stop only private Xvfb after revocation");
  const auto started = std::chrono::steady_clock::now();
  (void)manager->pump_events();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(::kill(server.pid, SIGCONT) == 0,
         "resume private Xvfb before assertions");
  expect(!manager->is_open(), "unknown cleanup ACK still fails closed");
  expect(elapsed < 150ms,
         "revoked transfer cleanup must retain short request budget");
  xcb_disconnect(peer.value);
}

void test_raw_incr_without_terminator_expires_at_original_deadline() {
  using namespace std::chrono_literals;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str(), 2);
  auto peer = connect_to(server.display.c_str());
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const auto property = intern_atom(peer.value, "PUNTO_RAW_EXPIRY");
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
             punto::ClipboardResult::Ok,
         "original baseline before expired transfer");
  const auto saved = manager->capture_payload(
      punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
  expect(saved && saved->payload, "save original bytes before transfer");
  const std::string replacement(8192, 'r');
  const auto publication =
      manager->set_payload_if_owner(punto::Selection::Clipboard, saved->origin,
                                    manager->text_payload(replacement),
                                    std::chrono::steady_clock::now() + 1s);
  expect(publication.completed, "temporary publication before expired receipt");
  set_active_window(peer.value, peer.screen, window);
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "real incomplete transfer receipt");
  const auto deadline = std::chrono::steady_clock::now() + 200ms;
  manager->activate_paste_receipt(*receipt, deadline);
  const auto epoch = saved->origin.connection_generation;
  const auto bytes = read_wire_payload(peer.value, window, *manager, clipboard,
                                       utf8, property, &*receipt, true);
  expect(bytes.bytes == std::vector<std::uint8_t>(replacement.begin(),
                                                  replacement.end()) &&
             std::chrono::steady_clock::now() < deadline,
         "all nonempty bytes arrived before original receipt deadline, no "
         "terminator");
  while (std::chrono::steady_clock::now() < deadline) {
    (void)manager->pump_events();
    std::this_thread::sleep_for(1ms);
  }
  (void)manager->pump_events();
  const auto current =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(current &&
             current->owner ==
                 punto::ClipboardManagerTestAccess::window(*manager) &&
             current->connection_generation == epoch &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Expired &&
             manager->paste_receipt_request_seq(*receipt) == 0 &&
             manager->is_open(),
         "unterminated transfer expires under original deadline without false "
         "delivery/connection loss");
  auto final_data = xcb_ptr(xcb_get_property_reply(
      peer.value,
      xcb_get_property(peer.value, 0, window, property,
                       XCB_GET_PROPERTY_TYPE_ANY, 0, 1024),
      nullptr));
  expect(final_data && final_data->type == utf8 && final_data->format == 8 &&
             xcb_get_property_value_length(final_data.get()) == 4096,
         "expiry did not manufacture a zero terminator");
  auto error = xcb_ptr(xcb_request_check(
      peer.value, xcb_delete_property_checked(peer.value, window, property)));
  expect(!error, "late real requestor ACK");
  auto barrier = xcb_ptr(xcb_get_input_focus_reply(
      punto::ClipboardManagerTestAccess::connection(*manager),
      xcb_get_input_focus(
          punto::ClipboardManagerTestAccess::connection(*manager)),
      nullptr));
  expect(barrier != nullptr, "manager-side event-order barrier after late ACK");
  (void)manager->pump_events();
  auto late = xcb_ptr(xcb_get_property_reply(
      peer.value,
      xcb_get_property(peer.value, 0, window, property,
                       XCB_GET_PROPERTY_TYPE_ANY, 0, 1024),
      nullptr));
  expect(late && late->type == XCB_ATOM_NONE &&
             manager->paste_receipt_status(*receipt) ==
                 punto::PasteReceiptStatus::Expired &&
             manager->paste_receipt_request_seq(*receipt) == 0,
         "late ACK never revives expired transfer or emits a terminator");
  manager->cancel_paste_receipt(*receipt);
  const auto fresh = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(fresh.has_value(), "same-manager fresh receipt after expiry");
  manager->activate_paste_receipt(*fresh,
                                  std::chrono::steady_clock::now() + 1s);
  const auto delivered = read_wire_payload(peer.value, window, *manager,
                                           clipboard, utf8, property, &*fresh);
  expect(delivered.bytes == bytes.bytes &&
             manager->paste_receipt_status(*fresh) ==
                 punto::PasteReceiptStatus::Delivered,
         "fresh complete same-manager transfer recovers");
  expect(manager->restore_payload_if_generation(punto::Selection::Clipboard,
                                                publication.generation, *saved),
         "restore saved original bytes after fresh completion");
  const auto restored = read_wire_payload(peer.value, window, *manager,
                                          clipboard, utf8, property);
  expect(restored.bytes == saved->payload->at(0).bytes,
         "exact baseline restoration after expiry");
  xcb_disconnect(peer.value);
}

void test_raw_snapshot_never_probes_action_targets() {
  using namespace std::chrono_literals;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str(), 2);
  auto peer = connect_to(server.display.c_str());
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  const auto targets = intern_atom(peer.value, "TARGETS");
  const auto timestamp = intern_atom(peer.value, "TIMESTAMP");
  const auto utf8 = intern_atom(peer.value, "UTF8_STRING");
  const std::array forbidden{intern_atom(peer.value, "DELETE"),
                             static_cast<xcb_atom_t>(XCB_ATOM_PIXMAP),
                             intern_atom(peer.value, "PUNTO_PRIVATE_BYTES"),
                             intern_atom(peer.value, "application/")};
  auto error = xcb_ptr(xcb_request_check(
      peer.value, xcb_set_selection_owner_checked(peer.value, window, clipboard,
                                                  XCB_CURRENT_TIME)));
  expect(!error, "real action-advertising owner");
  const auto initial =
      manager->get_selection_identity(punto::Selection::Clipboard);
  expect(initial.has_value(), "fenced actual owner before refusal");
  std::atomic<xcb_atom_t> advertised_forbidden{forbidden.front()};
  std::atomic<unsigned int> action_requests{0}, data_requests{0};
  std::exception_ptr failure;
  std::jthread provider{[&](std::stop_token stop) {
    try {
      while (!stop.stop_requested()) {
        auto event = xcb_ptr(xcb_poll_for_event(peer.value));
        if (!event) {
          pollfd fd{xcb_get_file_descriptor(peer.value), POLLIN, 0};
          (void)::poll(&fd, 1, 1);
          continue;
        }
        if ((event->response_type & 0x7fU) != XCB_SELECTION_REQUEST)
          continue;
        const auto &request =
            *reinterpret_cast<xcb_selection_request_event_t *>(event.get());
        const auto property = request.property == XCB_ATOM_NONE
                                  ? request.target
                                  : request.property;
        xcb_selection_notify_event_t notify{};
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.time = request.time;
        notify.requestor = request.requestor;
        notify.selection = request.selection;
        notify.target = request.target;
        notify.property = XCB_ATOM_NONE;
        if (request.target == targets) {
          std::vector<xcb_atom_t> catalogue{targets, timestamp, utf8};
          if (const auto bad = advertised_forbidden.load();
              bad != XCB_ATOM_NONE)
            catalogue.push_back(bad);
          xcb_change_property(peer.value, XCB_PROP_MODE_REPLACE,
                              request.requestor, property, XCB_ATOM_ATOM, 32,
                              static_cast<std::uint32_t>(catalogue.size()),
                              catalogue.data());
          notify.property = property;
        } else if (std::find(forbidden.begin(), forbidden.end(),
                             request.target) != forbidden.end()) {
          ++action_requests;
        } else if (request.target == utf8) {
          ++data_requests;
          constexpr char text[] = "fresh";
          xcb_change_property(peer.value, XCB_PROP_MODE_REPLACE,
                              request.requestor, property, utf8, 8, 5, text);
          notify.property = property;
        }
        xcb_send_event(peer.value, 0, request.requestor,
                       XCB_EVENT_MASK_NO_EVENT,
                       reinterpret_cast<const char *>(&notify));
        expect(xcb_flush(peer.value) > 0, "real action fixture response");
      }
    } catch (...) {
      failure = std::current_exception();
    }
  }};
  for (const auto bad : forbidden) {
    advertised_forbidden = bad;
    const auto before =
        manager->get_selection_identity(punto::Selection::Clipboard);
    expect(before.has_value(),
           "current fenced owner before each unsupported catalogue");
    const auto refused = manager->capture_payload(
        punto::Selection::Clipboard, std::chrono::steady_clock::now() + 200ms);
    const auto after =
        manager->get_selection_identity(punto::Selection::Clipboard);
    expect(!refused && action_requests == 0 && data_requests == 0 && after &&
               after->owner == window &&
               after->owner_generation == before->owner_generation &&
               after->connection_generation == initial->connection_generation,
           "action/resource/non-MIME private/malformed MIME rejects without "
           "data/action probes");
  }
  advertised_forbidden = XCB_ATOM_NONE;
  auto reassert = xcb_ptr(xcb_request_check(
      peer.value, xcb_set_selection_owner_checked(peer.value, window, clipboard,
                                                  XCB_CURRENT_TIME)));
  expect(!reassert, "real fresh compatible copy");
  const auto fresh = manager->capture_payload(
      punto::Selection::Clipboard, std::chrono::steady_clock::now() + 200ms);
  provider.request_stop();
  provider.join();
  if (failure)
    std::rethrow_exception(failure);
  expect(fresh && fresh->payload && fresh->payload->size() == 1 &&
             fresh->payload->front().bytes ==
                 std::vector<std::uint8_t>({'f', 'r', 'e', 's', 'h'}) &&
             action_requests == 0,
         "fresh compatible snapshot succeeds on the same manager");
  const auto expired =
      manager->set_payload_if_owner(punto::Selection::Clipboard, fresh->origin,
                                    manager->text_payload("forbidden"),
                                    std::chrono::steady_clock::now() - 1ms);
  expect(!expired.submitted && !expired.completed,
         "expired caller queues no publication");
  xcb_disconnect(peer.value);
}

enum class ForeignDisposition { Absent, Owned, Released };

void test_raw_uncertain_publication_preserves_baseline(
    publication_guard_fault::Stage stage, ForeignDisposition foreign) {
  using namespace std::chrono_literals;
  NestedXServer server = start_nested_xvfb();
  auto manager = raw_manager(server.display.c_str(), 2);
  auto probe = raw_manager(server.display.c_str(), 0);
  auto peer = connect_to(server.display.c_str());
  const auto window = create_window(peer.value, peer.screen);
  const auto clipboard = intern_atom(peer.value, "CLIPBOARD");
  expect(manager->set_text(punto::Selection::Clipboard, "baseline") ==
             punto::ClipboardResult::Ok,
         "baseline before fault");
  const auto saved = manager->capture_payload(
      punto::Selection::Clipboard, std::chrono::steady_clock::now() + 1s);
  expect(saved && saved->payload,
         "retain complete baseline before possible Set");
  const auto before_generation =
      manager->selection_generation(punto::Selection::Clipboard);
  publication_guard_fault::reset(
      punto::ClipboardManagerTestAccess::connection(*manager));
  publication_guard_fault::stage = stage;
  publication_guard_fault::server = server.pid;
  std::jthread rescue{[pid = server.pid] {
    std::this_thread::sleep_for(150ms);
    (void)::kill(pid, SIGCONT);
  }};
  const auto started = std::chrono::steady_clock::now();
  const auto publication = manager->set_payload_if_owner(
      punto::Selection::Clipboard, saved->origin,
      manager->text_payload("uncertain"), started + 50ms);
  (void)::kill(server.pid, SIGCONT);
  expect(
      publication_guard_fault::consumed &&
          publication_guard_fault::writes == 1 && publication.submitted &&
          !publication.completed && !manager->is_open() &&
          !publication_guard_fault::held &&
          std::chrono::steady_clock::now() - started < 120ms,
      "accepted real Set/Release closes only publisher under original budget");
  publication_guard_fault::reset();
  auto *observer = punto::ClipboardManagerTestAccess::witness(*manager);
  expect(observer && observer->is_open(),
         "independent real native owner witness survived");
  const auto own_release =
      observer->get_selection_identity(punto::Selection::Clipboard);
  expect(
      own_release && own_release->owner == XCB_WINDOW_NONE,
      "real server observed publishing client release before any foreign copy");
  if (foreign != ForeignDisposition::Absent) {
    auto error = xcb_ptr(xcb_request_check(
        peer.value, xcb_set_selection_owner_checked(
                        peer.value, window, clipboard, XCB_CURRENT_TIME)));
    auto owner = xcb_ptr(xcb_get_selection_owner_reply(
        peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
    expect(!error && owner && owner->owner == window,
           "real checked foreign server ownership");
    const auto copied =
        observer->get_selection_identity(punto::Selection::Clipboard);
    expect(copied && copied->owner == window &&
               copied->connection_generation ==
                   own_release->connection_generation &&
               copied->owner_generation != own_release->owner_generation,
           "independent XFixes witness observed actual foreign copy history");
    if (foreign == ForeignDisposition::Released) {
      auto destroyed = xcb_ptr(xcb_request_check(
          peer.value, xcb_destroy_window_checked(peer.value, window)));
      auto absent = xcb_ptr(xcb_get_selection_owner_reply(
          peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
      const auto released =
          observer->get_selection_identity(punto::Selection::Clipboard);
      expect(!destroyed && absent && absent->owner == XCB_WINDOW_NONE &&
                 released && released->owner == XCB_WINDOW_NONE &&
                 released->owner_generation != copied->owner_generation &&
                 released->connection_generation ==
                     copied->connection_generation,
             "actual foreign window destruction returns NONE with witnessed "
             "newer history");
    }
  }
  publication_guard_fault::reset();
  publication_guard_fault::count_all_writes = true;
  const bool restored = manager->restore_payload_after_uncertain(
      punto::Selection::Clipboard, publication, *saved,
      std::chrono::steady_clock::now() + 500ms);
  const auto restore_writes = publication_guard_fault::writes;
  publication_guard_fault::reset();
  expect(restored == (foreign == ForeignDisposition::Absent),
         "continuous own-release proof required; foreign copy/release never "
         "rehabilitates NONE");
  if (foreign != ForeignDisposition::Absent) {
    auto owner = xcb_ptr(xcb_get_selection_owner_reply(
        peer.value, xcb_get_selection_owner(peer.value, clipboard), nullptr));
    expect(restore_writes == 0 && !manager->is_open() && owner &&
               owner->owner == (foreign == ForeignDisposition::Owned
                                    ? window
                                    : xcb_window_t{XCB_WINDOW_NONE}),
           "foreign history denies restoration with ZERO Set calls, preserving "
           "actual owner/NONE");
    if (foreign == ForeignDisposition::Owned) {
      std::exception_ptr failure;
      std::jthread provider{[&] {
        try {
          serve_one_text_request(peer.value, "foreign");
        } catch (...) {
          failure = std::current_exception();
        }
      }};
      const auto actual = probe->get_text(punto::Selection::Clipboard);
      provider.join();
      if (failure)
        std::rethrow_exception(failure);
      expect(actual == std::optional<std::string>{"foreign"},
             "fresh peer bytes survive refusal");
    } else {
      const auto actual =
          probe->get_text_with_owner(punto::Selection::Clipboard);
      expect(actual && actual->owner == XCB_WINDOW_NONE && actual->text.empty(),
             "foreign copy then release remains real known-empty, no baseline "
             "republish");
    }
  } else {
    expect(restore_writes == 1 && manager->is_open() &&
               manager->get_text(punto::Selection::Clipboard) ==
                   std::optional<std::string>{"baseline"},
           "same manager restores baseline exactly once, never retries paste");
    publication_guard_fault::count_all_writes = true;
    expect(!manager->restore_payload_if_generation(punto::Selection::Clipboard,
                                                   before_generation, *saved) &&
               publication_guard_fault::writes == 0,
           "pre-adoption generation never gains restore authority over the new "
           "publisher");
    publication_guard_fault::reset();
  }
  expect(
      saved->payload->at(0).bytes ==
          std::vector<std::uint8_t>({'b', 'a', 's', 'e', 'l', 'i', 'n', 'e'}),
      "saved data survives publishing connection loss and all foreign-history "
      "refusals");
  xcb_disconnect(peer.value);
}

} // namespace

int main(int argc, char **argv) {
  try {
    expect(std::signal(SIGPIPE, SIG_IGN) != SIG_ERR,
           "ignore SIGPIPE like punto-daemon");
    const auto run = [](const char *name, auto test) {
      try {
        test();
      } catch (const std::exception &error) {
        throw std::runtime_error{std::string{name} + ": " + error.what()};
      }
    };
    if (argc == 2) {
      const std::string_view selected{argv[1]};
      if (selected == "initialization-atom") {
        test_initialization_stall(initialization_fault::Stage::Atom);
      } else if (selected == "initialization-extension") {
        test_initialization_stall(initialization_fault::Stage::Extension);
      } else if (selected == "initialization-deadline") {
        test_caller_initialization_deadline();
      } else if (selected == "identity-pump-close") {
        test_identity_check_stops_after_initial_pump_closes_connection();
      } else if (selected == "identity-reconnect") {
        test_selection_identity_does_not_survive_reconnect();
      } else if (selected == "identity-backlog") {
        test_identity_check_rejects_incomplete_event_backlog();
      } else if (selected == "identity-deadline") {
        test_identity_check_keeps_caller_deadline_while_serving_request();
      } else if (selected == "observation-owner-timeout") {
        test_observational_owner_timeout_preserves_publisher();
      } else if (selected == "receipt-request-budget") {
        test_context_bound_request_flood_does_not_reenter_payload();
      } else if (selected == "receipt-concurrent-clients") {
        test_context_bound_concurrent_requests_deliver_to_both_clients();
      } else if (selected == "deferred-request-lifetime") {
        test_deferred_request_lifetime_and_fresh_recovery();
      } else if (selected == "baseline-without-timestamp") {
        test_preexisting_owner_without_timestamp_uses_local_fence();
      } else if (selected == "identity-without-payload") {
        test_selection_identity_does_not_require_payload();
      } else if (selected == "text-read-classification") {
        test_text_read_classifies_owner_rejection_and_timeout();
      } else if (selected == "read-lifetime") {
        test_expired_read_cannot_supply_the_next_payload();
      } else if (selected == "read-aba") {
        test_read_rejects_owner_aba_and_recovers();
      } else if (selected == "read-default-deadline") {
        test_default_read_keeps_its_own_deadline();
      } else if (selected == "read-request-prefix") {
        test_read_services_bounded_requests_without_dropping_owned_payload();
      } else if (selected == "read-notify-prefix") {
        test_read_services_bounded_requests_without_dropping_owned_payload(
            true);
      } else if (selected == "atomic-property-read") {
        test_text_read_deletes_private_property_atomically();
      } else if (selected == "restore-publication") {
        test_generation_guard_and_oversize_rejection();
        test_restore_never_reclaims_external_ownership();
        test_publication_rejects_obsolete_authority();
      } else if (selected == "restore-boundary-read") {
        test_restore_publication_boundary(
            publication_fault::Stage::BoundaryRead);
      } else if (selected == "restore-timestamp-timeout") {
        test_restore_publication_boundary(
            publication_fault::Stage::TimestampTimeout);
      } else if (selected == "restore-timestamp-takeover") {
        test_restore_publication_boundary(
            publication_fault::Stage::ForeignTakeover);
      } else if (selected == "same-owner-fence") {
        test_xfixes_timestamp_fences_same_owner_new_copy();
      } else if (selected == "raw-payload") {
        test_raw_payload_roundtrip_and_incr_receipt();
      } else if (selected == "deferred-incr-owner") {
        test_deferred_incr_lifetime("foreign-owner");
      } else if (selected == "deferred-incr-generation") {
        test_deferred_incr_lifetime("new-generation");
      } else if (selected == "deferred-incr-replacement") {
        test_deferred_incr_lifetime("replacement");
      } else if (selected == "deferred-incr-recreated-window") {
        test_deferred_incr_lifetime("recreated-window");
      } else if (selected == "dead-requestor") {
        test_dead_requestor_preserves_publisher();
      } else if (selected == "close-diagnostics") {
        test_normal_close_diagnostic();
      } else if (selected == "expired-peer-ack") {
        test_expired_peer_ack_preserves_completed_publisher();
      } else if (selected == "expired-incr-ack") {
        test_expired_incr_ack_retires_live_watch();
      } else if (selected == "incr-watch-capacity") {
        test_incr_watch_capacity_and_shared_peer();
      } else if (selected == "incr-watch-revoked-budget") {
        test_revoked_incr_cleanup_keeps_request_budget();
      } else if (selected == "raw-incr-expiry") {
        test_raw_incr_without_terminator_expires_at_original_deadline();
      } else if (selected == "raw-action") {
        test_raw_snapshot_never_probes_action_targets();
      } else if (selected == "raw-uncertainty") {
        for (const auto stage : {publication_guard_fault::Stage::SetStop,
                                 publication_guard_fault::Stage::ReleaseStop})
          for (const auto foreign :
               {ForeignDisposition::Absent, ForeignDisposition::Owned,
                ForeignDisposition::Released})
            test_raw_uncertain_publication_preserves_baseline(stage, foreign);
      } else if (selected == "publication-guard") {
        test_conditional_publication_from_absent_owner();
        test_conditional_publication_preserves_queued_foreign_copy();
        test_conditional_publication_releases_before_serving_request();
        test_conditional_publication_readmission_refusals();
        test_conditional_publication_timestamp_services_other_selection();
        for (const auto stage : {publication_guard_fault::Stage::GrabStop,
                                 publication_guard_fault::Stage::SetStop,
                                 publication_guard_fault::Stage::ReleaseStop}) {
          test_conditional_publication_uncertainty_releases_server(stage);
        }
      } else if (selected == "publication-readmission") {
        test_conditional_publication_releases_before_serving_request();
      } else if (selected == "publication-readmission-refusals") {
        test_conditional_publication_readmission_refusals();
      } else if (selected == "publication-timestamp-reader") {
        test_conditional_publication_timestamp_services_other_selection();
      } else {
        throw std::runtime_error{"unknown focused test"};
      }
      std::cout << "PASS: " << selected << '\n';
      return 0;
    }
    run("cold-initialization", test_cold_initialization);
    run("initialization-deadline", test_caller_initialization_deadline);
    run("initialization-atom",
        [] { test_initialization_stall(initialization_fault::Stage::Atom); });
    run("initialization-extension", [] {
      test_initialization_stall(initialization_fault::Stage::Extension);
    });
    run("delivery", test_delivery_receipts_and_targets);
    run("preexisting-baseline",
        test_preexisting_owner_has_timestamp_fenced_baseline);
    run("baseline-without-timestamp",
        test_preexisting_owner_without_timestamp_uses_local_fence);
    run("identity-without-payload",
        test_selection_identity_does_not_require_payload);
    run("text-read-classification",
        test_text_read_classifies_owner_rejection_and_timeout);
    run("read-lifetime", test_expired_read_cannot_supply_the_next_payload);
    run("read-aba", test_read_rejects_owner_aba_and_recovers);
    run("read-default-deadline", test_default_read_keeps_its_own_deadline);
    run("read-request-prefix", [] {
      test_read_services_bounded_requests_without_dropping_owned_payload();
    });
    run("read-notify-prefix", [] {
      test_read_services_bounded_requests_without_dropping_owned_payload(true);
    });
    run("atomic-property-read",
        test_text_read_deletes_private_property_atomically);
    run("owner-transition",
        test_owner_transition_token_tracks_exact_identity_history);
    run("identity-pump-close",
        test_identity_check_stops_after_initial_pump_closes_connection);
    run("identity-backlog",
        test_identity_check_rejects_incomplete_event_backlog);
    run("identity-deadline",
        test_identity_check_keeps_caller_deadline_while_serving_request);
    run("identity-reconnect",
        test_selection_identity_does_not_survive_reconnect);
    run("observation-owner-timeout",
        test_observational_owner_timeout_preserves_publisher);
    run("byte-payloads", test_empty_and_embedded_nul_payloads);
    run("property-validation",
        test_strict_property_validation_and_incr_rejection);
    run("live-incr", test_live_oversize_incr_offer_refuses_and_recovers);
    run("generation", test_generation_guard_and_oversize_rejection);
    run("publication-history", test_publication_rejects_obsolete_authority);
    run("external-takeover", test_restore_never_reclaims_external_ownership);
    run("restore-boundary-read", [] {
      test_restore_publication_boundary(publication_fault::Stage::BoundaryRead);
    });
    run("restore-timestamp-timeout", [] {
      test_restore_publication_boundary(
          publication_fault::Stage::TimestampTimeout);
    });
    run("restore-timestamp-takeover", [] {
      test_restore_publication_boundary(
          publication_fault::Stage::ForeignTakeover);
    });
    run("same-owner-fence", test_xfixes_timestamp_fences_same_owner_new_copy);
    run("publication-absent", test_conditional_publication_from_absent_owner);
    run("publication-peer-copy",
        test_conditional_publication_preserves_queued_foreign_copy);
    run("publication-queued-request",
        test_conditional_publication_releases_before_serving_request);
    run("publication-readmission-refusals",
        test_conditional_publication_readmission_refusals);
    run("publication-timestamp-reader",
        test_conditional_publication_timestamp_services_other_selection);
    run("publication-grab-uncertain", [] {
      test_conditional_publication_uncertainty_releases_server(
          publication_guard_fault::Stage::GrabStop);
    });
    run("publication-write-uncertain", [] {
      test_conditional_publication_uncertainty_releases_server(
          publication_guard_fault::Stage::SetStop);
    });
    run("publication-release-uncertain", [] {
      test_conditional_publication_uncertainty_releases_server(
          publication_guard_fault::Stage::ReleaseStop);
    });
    run("raw-payload", test_raw_payload_roundtrip_and_incr_receipt);
    run("deferred-incr-owner",
        [] { test_deferred_incr_lifetime("foreign-owner"); });
    run("deferred-incr-generation",
        [] { test_deferred_incr_lifetime("new-generation"); });
    run("deferred-incr-replacement",
        [] { test_deferred_incr_lifetime("replacement"); });
    run("deferred-incr-recreated-window",
        [] { test_deferred_incr_lifetime("recreated-window"); });
    run("dead-requestor", test_dead_requestor_preserves_publisher);
    run("close-diagnostics", test_normal_close_diagnostic);
    run("expired-peer-ack",
        test_expired_peer_ack_preserves_completed_publisher);
    run("expired-incr-ack", test_expired_incr_ack_retires_live_watch);
    run("incr-watch-capacity", test_incr_watch_capacity_and_shared_peer);
    run("incr-watch-revoked-budget",
        test_revoked_incr_cleanup_keeps_request_budget);
    run("raw-incr-expiry",
        test_raw_incr_without_terminator_expires_at_original_deadline);
    run("raw-action", test_raw_snapshot_never_probes_action_targets);
    run("raw-uncertainty", [] {
      for (const auto stage : {publication_guard_fault::Stage::SetStop,
                               publication_guard_fault::Stage::ReleaseStop})
        for (const auto foreign :
             {ForeignDisposition::Absent, ForeignDisposition::Owned,
              ForeignDisposition::Released})
          test_raw_uncertain_publication_preserves_baseline(stage, foreign);
    });
    run("rich-target-detection",
        test_rich_clipboard_is_detected_before_mutation);
    run("released-primary-restore",
        test_primary_restore_after_exact_client_release);
    run("active-window-kind",
        test_active_window_kind_is_tristate_and_boundary_safe);
    run("pump-budget", test_pump_has_a_hard_event_budget);
    run("receipt-request-budget",
        test_context_bound_request_flood_does_not_reenter_payload);
    run("receipt-concurrent-clients",
        test_context_bound_concurrent_requests_deliver_to_both_clients);
    run("deferred-request-lifetime",
        test_deferred_request_lifetime_and_fresh_recovery);
    run("disconnect", test_disconnect_is_fail_closed_not_process_fatal);
    run("absent-owner", test_absent_owner_is_a_known_empty_selection);
    run("stopped-server", test_stopped_server_is_bounded_and_fail_closed);
    run("stopped-large-send",
        test_stopped_server_large_payload_send_is_bounded);
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "PASS: clipboard manager contract\n";
  return 0;
}
