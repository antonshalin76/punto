#include "punto/clipboard_manager.hpp"
#include "punto/x11_session.hpp"

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace punto {

struct ClipboardManagerTestAccess {
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
request_selection(xcb_connection_t *requester, xcb_window_t window,
                  punto::ClipboardManager &manager, xcb_atom_t selection,
                  xcb_atom_t target, xcb_atom_t property) {
  const auto cookie = xcb_convert_selection_checked(
      requester, window, selection, target, property, XCB_CURRENT_TIME);
  auto error = xcb_ptr(xcb_request_check(requester, cookie));
  expect(!error && xcb_flush(requester) > 0, "queue selection conversion");

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

void serve_one_text_request(xcb_connection_t *owner, std::string_view text) {
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
      const xcb_atom_t property =
          request.property == XCB_ATOM_NONE ? request.target : request.property;
      xcb_change_property(owner, XCB_PROP_MODE_REPLACE, request.requestor,
                          property, request.target, 8,
                          static_cast<std::uint32_t>(text.size()), text.data());
      xcb_selection_notify_event_t notify{};
      notify.response_type = XCB_SELECTION_NOTIFY;
      notify.time = request.time;
      notify.requestor = request.requestor;
      notify.selection = request.selection;
      notify.target = request.target;
      notify.property = property;
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
                          xcb_atom_t property, xcb_atom_t expected_type) {
  const auto cookie = xcb_get_property(connection, 1, window, property,
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
  const xcb_window_t same_client_helper =
      create_window(requester.value, requester.screen);
  (void)request_selection(requester.value, same_client_helper, *manager,
                          clipboard, utf8, XCB_ATOM_NONE);
  expect(manager->paste_receipt_seen(*receipt),
         "same-client helper window acknowledges active application paste");
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  const auto delayed_active_request = request_selection(
      requester.value, window, *manager, clipboard, utf8, XCB_ATOM_NONE);
  expect(delayed_active_request.property == utf8,
         "delayed active-window paste request succeeds after early receipt");
  expect(read_property(requester.value, window, utf8, utf8) == "payload",
         "replacement ownership retained for delayed active-window paste");
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

void test_live_incr_offer_is_aborted_fail_closed() {
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
  expect(!manager->is_open(),
         "unsupported INCR destroys the private request window");
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
  expect(manager->restore_text_if_generation(punto::Selection::Clipboard,
                                             newer_generation, "old"),
         "matching generation may be restored");
  expect(manager->selection_generation(punto::Selection::Clipboard) !=
             newer_generation,
         "successful restore creates a new generation");

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
  expect(manager->active_window_kind() == punto::ActiveWindowKind::Unknown,
         "nonempty unknown WM_CLASS is not assumed to be GUI-safe");

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

struct NestedXServer {
  pid_t pid = -1;
  std::string display;

  ~NestedXServer() {
    if (pid > 0) {
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
            static_cast<char *>(nullptr));
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

} // namespace

int main() {
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
    run("delivery", test_delivery_receipts_and_targets);
    run("preexisting-baseline",
        test_preexisting_owner_has_timestamp_fenced_baseline);
    run("owner-transition",
        test_owner_transition_token_tracks_exact_identity_history);
    run("byte-payloads", test_empty_and_embedded_nul_payloads);
    run("property-validation",
        test_strict_property_validation_and_incr_rejection);
    run("live-incr", test_live_incr_offer_is_aborted_fail_closed);
    run("generation", test_generation_guard_and_oversize_rejection);
    run("external-takeover", test_restore_never_reclaims_external_ownership);
    run("same-owner-fence", test_xfixes_timestamp_fences_same_owner_new_copy);
    run("rich-target-detection",
        test_rich_clipboard_is_detected_before_mutation);
    run("released-primary-restore",
        test_primary_restore_after_exact_client_release);
    run("active-window-kind",
        test_active_window_kind_is_tristate_and_boundary_safe);
    run("pump-budget", test_pump_has_a_hard_event_budget);
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
