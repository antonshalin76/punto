#include "punto/event_loop.hpp"
#include "punto/ipc_server.hpp"

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <xcb/xcbext.h>
#include <xcb/xinput.h>
#include <xcb/xtest.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define explicit explicit_value
#include <xcb/xkb.h>
#undef explicit
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {
bool consume_private_fault_marker(const char *path) {
  const int marker =
      ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (marker < 0) {
    return false;
  }
  struct stat metadata {};
  const bool armed = ::fstat(marker, &metadata) == 0 &&
                     S_ISREG(metadata.st_mode) &&
                     metadata.st_uid == ::geteuid() && metadata.st_nlink == 1 &&
                     (metadata.st_mode & 0077) == 0;
  (void)::close(marker);
  return armed && ::unlink(path) == 0;
}

std::mutex keyboard_query_mutex;
std::optional<std::pair<xcb_connection_t *, unsigned int>> keyboard_query;
std::optional<std::pair<xcb_connection_t *, unsigned int>> failed_keymap_query;
std::optional<std::pair<xcb_connection_t *, unsigned int>> failed_xtest_request;
std::optional<std::pair<xcb_connection_t *, unsigned int>> failed_focus_query;
std::optional<std::pair<xcb_connection_t *, unsigned int>> failed_pointer_query;
std::optional<std::pair<xcb_connection_t *, unsigned int>>
    failed_xinput_version_query;
std::optional<std::pair<xcb_connection_t *, unsigned int>>
    injected_layout_query;
xcb_connection_t *injected_layout_connection = nullptr;
int injected_layout_state = 0;
std::optional<std::pair<xcb_connection_t *, unsigned int>>
    injected_keymap_query;
xcb_connection_t *injected_keymap_connection = nullptr;
std::atomic<std::int64_t> layout_settle_not_before_ns{0};
std::atomic<int> xtest_requests_until_failure{0};
std::atomic<bool> macro_ipc_hold{false};
std::atomic<bool> macro_ipc_admitted{false};
std::atomic<bool> fail_next_focus_query{false};
std::atomic<bool> fail_next_pointer_query{false};
std::atomic<bool> focus_failure_pending{false};
std::atomic<bool> observe_internal_restore_write{false};
std::atomic<bool> fail_internal_restore_barrier{false};
std::atomic<int> injected_raw_events{0};
std::atomic<bool> slow_injected_raw_events{false};
std::atomic<int> failed_raw_event_observation{0};
std::atomic<std::int64_t> ordinary_work_cutoff_ns{0};
std::atomic<xcb_connection_t *> restore_failed_connection{nullptr};
std::atomic<bool> restore_reprepare_pending{false};
std::atomic<bool> restore_xinput_pending{false};
std::atomic<int> pending_xinput_barrier_fault{0};
std::atomic<int> pending_xinput_version_fault{0};
int failed_focus_fault = 0;
int failed_xinput_version_fault = 0;

const char *xinput_fault_consumed_marker(int fault) noexcept {
  switch (fault) {
  case 1:
    return "/run/punto-e2e-initial-xinput-barrier-fault-consumed";
  case 2:
    return "/run/punto-e2e-initial-xinput-version-fault-consumed";
  case 3:
    return "/run/punto-e2e-reconnect-xinput-barrier-fault-consumed";
  case 4:
    return "/run/punto-e2e-reconnect-xinput-version-fault-consumed";
  default:
    return nullptr;
  }
}

void mark_private_macro_event(const char *path) noexcept {
  const int marker =
      ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (marker >= 0) {
    (void)::close(marker);
  }
}

void observe_admitted_macro_ipc() noexcept {
  if (macro_ipc_hold.load(std::memory_order_acquire)) {
    mark_private_macro_event("/run/punto-e2e-macro-ipc-admitted");
    macro_ipc_admitted.store(true, std::memory_order_release);
  }
}
} // namespace

extern "C" void punto_e2e_after_paste_receipt_arm() {
  if (!consume_private_fault_marker("/run/punto-e2e-arm-after-paste-receipt")) {
    return;
  }
  mark_private_macro_event("/run/punto-e2e-after-paste-receipt");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline &&
         !consume_private_fault_marker(
             "/run/punto-e2e-release-after-paste-receipt")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

extern "C" void punto_e2e_after_x11_session_commit(const char *session_id) {
  if (session_id != nullptr &&
      std::string_view{session_id} == "punto-event-loop-e2e-new") {
    mark_private_macro_event("/run/punto-e2e-new-session-committed");
  }
}

extern "C" void punto_e2e_after_word_dispatch() {
  if (consume_private_fault_marker(
          "/run/punto-e2e-expire-before-post-dispatch-context")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    return;
  }
  if (!consume_private_fault_marker("/run/punto-e2e-arm-after-word-dispatch")) {
    return;
  }
  mark_private_macro_event("/run/punto-e2e-after-word-dispatch");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline &&
         !consume_private_fault_marker(
             "/run/punto-e2e-release-after-word-dispatch")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

extern "C" void punto_e2e_before_post_dispatch_wait() {
  if (consume_private_fault_marker(
          "/run/punto-e2e-expire-in-post-dispatch-wait")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
  }
}

extern "C" void punto_e2e_after_internal_layout() {
  if (consume_private_fault_marker(
          "/run/punto-e2e-expire-after-internal-layout")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    return;
  }
  if (!consume_private_fault_marker(
          "/run/punto-e2e-arm-after-internal-layout")) {
    return;
  }
  macro_ipc_admitted.store(false, std::memory_order_relaxed);
  macro_ipc_hold.store(true, std::memory_order_release);
  mark_private_macro_event("/run/punto-e2e-after-internal-layout");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline &&
         !consume_private_fault_marker(
             "/run/punto-e2e-release-after-internal-layout")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  macro_ipc_hold.store(false, std::memory_order_release);
}

extern "C" void punto_e2e_before_internal_layout_change() {
  if (consume_private_fault_marker(
          "/run/punto-e2e-expire-before-internal-layout")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{3450});
  }
}

extern "C" void punto_e2e_before_internal_layout_restore() {
  if (consume_private_fault_marker("/run/punto-e2e-fail-cleanup-focus")) {
    fail_next_focus_query.store(true, std::memory_order_release);
    focus_failure_pending.store(true, std::memory_order_release);
  }
  if (consume_private_fault_marker("/run/punto-e2e-fail-cleanup-pointer")) {
    fail_next_pointer_query.store(true, std::memory_order_release);
  }
  observe_internal_restore_write.store(
      consume_private_fault_marker(
          "/run/punto-e2e-arm-observe-internal-restore"),
      std::memory_order_release);
  fail_internal_restore_barrier.store(
      consume_private_fault_marker(
          "/run/punto-e2e-fail-internal-restore-barrier"),
      std::memory_order_release);
  if (!consume_private_fault_marker(
          "/run/punto-e2e-arm-before-internal-restore")) {
    return;
  }
  mark_private_macro_event("/run/punto-e2e-before-internal-restore");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (std::chrono::steady_clock::now() < deadline &&
         !consume_private_fault_marker(
             "/run/punto-e2e-release-before-internal-restore")) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

extern "C" std::int64_t
punto_e2e_internal_layout_cleanup_start(std::int64_t cleanup_start_ns) {
  if (consume_private_fault_marker(
          "/run/punto-e2e-arm-cleanup-reserve-boundary")) {
    cleanup_start_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            (std::chrono::steady_clock::now() + std::chrono::milliseconds{50})
                .time_since_epoch())
            .count();
    ordinary_work_cutoff_ns.store(cleanup_start_ns, std::memory_order_release);
  }
  return cleanup_start_ns;
}

extern "C" void
punto_e2e_before_internal_layout_work(std::int64_t cleanup_start_ns) {
  if (ordinary_work_cutoff_ns.load(std::memory_order_acquire) == 0) {
    return;
  }
  const auto cutoff = std::chrono::steady_clock::time_point{
      std::chrono::nanoseconds{cleanup_start_ns}};
  const auto release_at = cutoff + std::chrono::milliseconds{2};
  while (std::chrono::steady_clock::now() < release_at) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  mark_private_macro_event("/run/punto-e2e-cleanup-reserve-boundary-reached");
}

extern "C" void punto_e2e_before_raw_key_observation() {
  if (consume_private_fault_marker("/run/punto-e2e-arm-overflow-raw-events")) {
    injected_raw_events.store(1024, std::memory_order_release);
    slow_injected_raw_events.store(false, std::memory_order_release);
  } else if (consume_private_fault_marker(
                 "/run/punto-e2e-arm-slow-raw-events")) {
    injected_raw_events.store(1024, std::memory_order_release);
    slow_injected_raw_events.store(true, std::memory_order_release);
  } else if (consume_private_fault_marker(
                 "/run/punto-e2e-arm-fail-raw-event-observation")) {
    failed_raw_event_observation.store(1, std::memory_order_release);
  }
}

extern "C" void punto_e2e_after_raw_key_observation() {
  if (injected_raw_events.load(std::memory_order_acquire) > 0) {
    mark_private_macro_event(
        "/run/punto-e2e-raw-observation-returned-before-drain");
  }
}

extern "C" void punto_e2e_before_word_replay() {
  const auto not_before =
      layout_settle_not_before_ns.exchange(0, std::memory_order_acq_rel);
  if (not_before == 0) {
    return;
  }
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  mark_private_macro_event(now < not_before
                               ? "/run/punto-e2e-layout-settle-too-early"
                               : "/run/punto-e2e-layout-settle-observed");
}

extern "C" decltype(xcb_intern_atom) __real_xcb_intern_atom;
extern "C" xcb_intern_atom_cookie_t
__wrap_xcb_intern_atom(xcb_connection_t *connection,
                       std::uint8_t only_if_exists, std::uint16_t length,
                       const char *name) {
  if (std::string_view{name, length} == "CLIPBOARD" &&
      consume_private_fault_marker("/run/punto-e2e-slow-clipboard-init")) {
    // A test-only scheduling delay, not proof of interruptible I/O expiry.
    std::cerr << "[fixture] Clipboard initialization delay reached\n";
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  if (std::string_view{name, length} == "CLIPBOARD" &&
      consume_private_fault_marker("/run/punto-e2e-hold-macro-ipc")) {
    macro_ipc_admitted.store(false, std::memory_order_relaxed);
    macro_ipc_hold.store(true, std::memory_order_release);
    mark_private_macro_event("/run/punto-e2e-macro-ipc-held");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{150};
    while (std::chrono::steady_clock::now() < deadline &&
           !macro_ipc_admitted.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (!macro_ipc_admitted.load(std::memory_order_acquire)) {
      mark_private_macro_event("/run/punto-e2e-macro-ipc-expired");
    }
    macro_ipc_hold.store(false, std::memory_order_release);
  }
  return __real_xcb_intern_atom(connection, only_if_exists, length, name);
}

extern "C" decltype(xcb_xkb_get_state) __real_xcb_xkb_get_state;
extern "C" decltype(xcb_query_keymap) __real_xcb_query_keymap;
extern "C" xcb_query_keymap_cookie_t
__wrap_xcb_query_keymap(xcb_connection_t *connection) {
  const auto cookie = __real_xcb_query_keymap(connection);
  {
    std::lock_guard lock{keyboard_query_mutex};
    if (injected_keymap_connection == connection) {
      injected_keymap_query = std::pair{connection, cookie.sequence};
    }
  }
  if (consume_private_fault_marker("/run/punto-e2e-arm-key-release-check")) {
    mark_private_macro_event("/run/punto-e2e-key-release-checked");
  }
  if (consume_private_fault_marker("/run/punto-e2e-fail-word-keymap")) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_keymap_query = std::pair{connection, cookie.sequence};
  }
  return cookie;
}

extern "C" decltype(xcb_test_fake_input_checked)
    __real_xcb_test_fake_input_checked;
extern "C" xcb_void_cookie_t __wrap_xcb_test_fake_input_checked(
    xcb_connection_t *connection, std::uint8_t type, std::uint8_t detail,
    std::uint32_t time, xcb_window_t root, std::int16_t root_x,
    std::int16_t root_y, std::uint8_t device_id) {
  const auto cookie = __real_xcb_test_fake_input_checked(
      connection, type, detail, time, root, root_x, root_y, device_id);
  const auto cutoff = ordinary_work_cutoff_ns.load(std::memory_order_acquire);
  if (cutoff != 0 && std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                             .count() >= cutoff) {
    mark_private_macro_event(
        "/run/punto-e2e-ordinary-write-in-cleanup-reserve");
  }
  const auto requests_until_failure =
      xtest_requests_until_failure.load(std::memory_order_acquire);
  if (requests_until_failure > 0 && xtest_requests_until_failure.fetch_sub(
                                        1, std::memory_order_acq_rel) == 1) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_xtest_request = std::pair{connection, cookie.sequence};
  }
  if (type == XCB_KEY_PRESS && detail == KEY_LEFTCTRL + 8U &&
      consume_private_fault_marker("/run/punto-e2e-arm-layout-settle-window")) {
    const auto not_before =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
    layout_settle_not_before_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            not_before.time_since_epoch())
            .count(),
        std::memory_order_release);
  }
  if (type == XCB_KEY_PRESS && detail == KEY_LEFTCTRL + 8U &&
      consume_private_fault_marker(
          "/run/punto-e2e-inject-own-layout-modifier-state")) {
    std::lock_guard lock{keyboard_query_mutex};
    injected_layout_connection = connection;
    injected_layout_state = 1;
  } else if (type == XCB_KEY_PRESS && detail == KEY_LEFTCTRL + 8U &&
             consume_private_fault_marker(
                 "/run/punto-e2e-inject-foreign-layout-modifier-state")) {
    std::lock_guard lock{keyboard_query_mutex};
    injected_layout_connection = connection;
    injected_layout_state = 2;
  }
  if (type == XCB_KEY_PRESS &&
      consume_private_fault_marker("/run/punto-e2e-fail-layout-hotkey-send")) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_xtest_request = std::pair{connection, cookie.sequence};
  }
  return cookie;
}

extern "C" decltype(xcb_xkb_latch_lock_state_checked)
    __real_xcb_xkb_latch_lock_state_checked;
extern "C" xcb_void_cookie_t __wrap_xcb_xkb_latch_lock_state_checked(
    xcb_connection_t *connection, xcb_xkb_device_spec_t device,
    std::uint8_t affect_mod_locks, std::uint8_t mod_locks,
    std::uint8_t lock_group, std::uint8_t group_lock,
    std::uint8_t affect_mod_latches, std::uint8_t latch_group,
    std::uint16_t group_latch) {
  const auto cookie = __real_xcb_xkb_latch_lock_state_checked(
      connection, device, affect_mod_locks, mod_locks, lock_group, group_lock,
      affect_mod_latches, latch_group, group_latch);
  if (consume_private_fault_marker(
          "/run/punto-e2e-fail-internal-layout-barrier")) {
    fail_next_focus_query.store(true, std::memory_order_release);
  }
  if (observe_internal_restore_write.exchange(false,
                                              std::memory_order_acq_rel)) {
    mark_private_macro_event("/run/punto-e2e-internal-restore-written");
  }
  if (fail_internal_restore_barrier.exchange(false,
                                             std::memory_order_acq_rel)) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_xtest_request = std::pair{connection, cookie.sequence};
    restore_failed_connection.store(connection, std::memory_order_release);
    mark_private_macro_event("/run/punto-e2e-internal-restore-barrier-failed");
  }
  if (consume_private_fault_marker(
          "/run/punto-e2e-fail-after-internal-layout")) {
    xtest_requests_until_failure.store(1, std::memory_order_release);
  } else if (consume_private_fault_marker(
                 "/run/punto-e2e-fail-after-first-replay-stroke")) {
    xtest_requests_until_failure.store(3, std::memory_order_release);
  }
  return cookie;
}

extern "C" decltype(xcb_poll_for_event) __real_xcb_poll_for_event;
extern "C" xcb_generic_event_t *
__wrap_xcb_poll_for_event(xcb_connection_t *connection) {
  int expected_failure = 1;
  if (failed_raw_event_observation.compare_exchange_strong(
          expected_failure, 2, std::memory_order_acq_rel)) {
    return nullptr;
  }
  const int remaining = injected_raw_events.load(std::memory_order_acquire);
  if (remaining > 0) {
    const int previous =
        injected_raw_events.fetch_sub(1, std::memory_order_acq_rel);
    auto *event = static_cast<xcb_generic_event_t *>(
        std::calloc(1, sizeof(xcb_generic_event_t)));
    if (event == nullptr) {
      std::abort();
    }
    event->response_type = XCB_EXPOSE;
    if (slow_injected_raw_events.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (previous == 1) {
      mark_private_macro_event("/run/punto-e2e-overflow-raw-events-drained");
    }
    return event;
  }
  return __real_xcb_poll_for_event(connection);
}

extern "C" decltype(xcb_connection_has_error) __real_xcb_connection_has_error;
extern "C" int __wrap_xcb_connection_has_error(xcb_connection_t *connection) {
  int expected_failure = 2;
  if (failed_raw_event_observation.compare_exchange_strong(
          expected_failure, 0, std::memory_order_acq_rel)) {
    return XCB_CONN_ERROR;
  }
  return __real_xcb_connection_has_error(connection);
}

extern "C" decltype(xcb_get_input_focus) __real_xcb_get_input_focus;
extern "C" xcb_get_input_focus_cookie_t
__wrap_xcb_get_input_focus(xcb_connection_t *connection) {
  const auto cookie = __real_xcb_get_input_focus(connection);
  const int xinput_fault =
      pending_xinput_barrier_fault.exchange(0, std::memory_order_acq_rel);
  if (xinput_fault != 0) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_focus_query = std::pair{connection, cookie.sequence};
    failed_focus_fault = xinput_fault;
    return cookie;
  }
  if (fail_next_focus_query.exchange(false, std::memory_order_acq_rel)) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_focus_query = std::pair{connection, cookie.sequence};
  }
  return cookie;
}

extern "C" decltype(xcb_prefetch_extension_data)
    __real_xcb_prefetch_extension_data;
extern "C" void __wrap_xcb_prefetch_extension_data(xcb_connection_t *connection,
                                                   xcb_extension_t *extension) {
  if (extension == &xcb_test_id &&
      restore_reprepare_pending.exchange(false, std::memory_order_acq_rel)) {
    restore_xinput_pending.store(true, std::memory_order_release);
    mark_private_macro_event("/run/punto-e2e-internal-restore-reconnected");
  }
  if (extension == &xcb_input_id) {
    const bool reconnect =
        restore_xinput_pending.exchange(false, std::memory_order_acq_rel);
    if (reconnect) {
      mark_private_macro_event(
          "/run/punto-e2e-reconnect-xinput-preparation-entered");
    }
    const char *barrier = reconnect
                              ? "/run/punto-e2e-fail-reconnect-xinput-barrier"
                              : "/run/punto-e2e-fail-initial-xinput-barrier";
    const char *version = reconnect
                              ? "/run/punto-e2e-fail-reconnect-xinput-version"
                              : "/run/punto-e2e-fail-initial-xinput-version";
    if (consume_private_fault_marker(barrier)) {
      pending_xinput_barrier_fault.store(reconnect ? 3 : 1,
                                         std::memory_order_release);
    } else if (consume_private_fault_marker(version)) {
      pending_xinput_version_fault.store(reconnect ? 4 : 2,
                                         std::memory_order_release);
    }
  }
  __real_xcb_prefetch_extension_data(connection, extension);
}

extern "C" decltype(xcb_input_xi_query_version)
    __real_xcb_input_xi_query_version;
extern "C" xcb_input_xi_query_version_cookie_t
__wrap_xcb_input_xi_query_version(xcb_connection_t *connection,
                                  std::uint16_t major_version,
                                  std::uint16_t minor_version) {
  const auto cookie = __real_xcb_input_xi_query_version(
      connection, major_version, minor_version);
  const int xinput_fault =
      pending_xinput_version_fault.exchange(0, std::memory_order_acq_rel);
  if (xinput_fault != 0) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_xinput_version_query = std::pair{connection, cookie.sequence};
    failed_xinput_version_fault = xinput_fault;
  }
  return cookie;
}

extern "C" decltype(xcb_get_setup) __real_xcb_get_setup;
extern "C" const xcb_setup_t *
__wrap_xcb_get_setup(xcb_connection_t *connection) {
  if (connection == nullptr) {
    mark_private_macro_event(
        "/run/punto-e2e-unsafe-xcb-get-setup-after-xinput-failure");
    std::abort();
  }
  return __real_xcb_get_setup(connection);
}

extern "C" decltype(xcb_query_pointer) __real_xcb_query_pointer;
extern "C" xcb_query_pointer_cookie_t
__wrap_xcb_query_pointer(xcb_connection_t *connection, xcb_window_t window) {
  if (focus_failure_pending.exchange(false, std::memory_order_acq_rel)) {
    mark_private_macro_event(
        "/run/punto-e2e-unsafe-pointer-after-focus-failure");
  }
  const auto cookie = __real_xcb_query_pointer(connection, window);
  if (fail_next_pointer_query.exchange(false, std::memory_order_acq_rel)) {
    std::lock_guard lock{keyboard_query_mutex};
    failed_pointer_query = std::pair{connection, cookie.sequence};
  }
  return cookie;
}

extern "C" xcb_xkb_get_state_cookie_t
__wrap_xcb_xkb_get_state(xcb_connection_t *connection,
                         xcb_xkb_device_spec_t device) {
  const auto cookie = __real_xcb_xkb_get_state(connection, device);
  std::lock_guard lock{keyboard_query_mutex};
  if (consume_private_fault_marker("/run/punto-e2e-arm-keyboard-observation")) {
    keyboard_query = std::pair{connection, cookie.sequence};
  }
  if (injected_layout_state != 0 && injected_layout_connection == connection) {
    injected_layout_query = std::pair{connection, cookie.sequence};
  }
  return cookie;
}

extern "C" decltype(xcb_poll_for_reply) __real_xcb_poll_for_reply;
extern "C" int __wrap_xcb_poll_for_reply(xcb_connection_t *connection,
                                         unsigned int sequence, void **reply,
                                         xcb_generic_error_t **error) {
  bool inject_protocol_error = false;
  int consumed_xinput_fault = 0;
  {
    std::lock_guard lock{keyboard_query_mutex};
    const auto request = std::optional{std::pair{connection, sequence}};
    if (failed_keymap_query == request) {
      failed_keymap_query.reset();
      inject_protocol_error = true;
    } else if (failed_xtest_request == request) {
      failed_xtest_request.reset();
      inject_protocol_error = true;
    } else if (failed_focus_query == request) {
      failed_focus_query.reset();
      focus_failure_pending.store(false, std::memory_order_release);
      consumed_xinput_fault = failed_focus_fault;
      failed_focus_fault = 0;
      inject_protocol_error = true;
    } else if (failed_pointer_query == request) {
      failed_pointer_query.reset();
      inject_protocol_error = true;
    } else if (failed_xinput_version_query == request) {
      failed_xinput_version_query.reset();
      consumed_xinput_fault = failed_xinput_version_fault;
      failed_xinput_version_fault = 0;
      inject_protocol_error = true;
    }
  }
  if (inject_protocol_error) {
    if (const auto *marker =
            xinput_fault_consumed_marker(consumed_xinput_fault)) {
      mark_private_macro_event(marker);
    }
    *reply = nullptr;
    *error = static_cast<xcb_generic_error_t *>(
        std::calloc(1, sizeof(xcb_generic_error_t)));
    if (*error == nullptr) {
      std::abort();
    }
    (*error)->error_code = XCB_VALUE;
    (*error)->sequence = static_cast<std::uint16_t>(sequence);
    return 1;
  }
  const int result =
      __real_xcb_poll_for_reply(connection, sequence, reply, error);
  bool observed = false;
  int injected = 0;
  bool inject_foreign_key = false;
  if (result != 0 && reply != nullptr && *reply != nullptr) {
    std::lock_guard lock{keyboard_query_mutex};
    if (keyboard_query == std::optional{std::pair{connection, sequence}}) {
      keyboard_query.reset();
      observed = true;
    }
    if (injected_layout_query ==
        std::optional{std::pair{connection, sequence}}) {
      injected_layout_query.reset();
      injected_layout_connection = nullptr;
      injected = injected_layout_state;
      injected_layout_state = 0;
    }
    if (injected_keymap_query ==
        std::optional{std::pair{connection, sequence}}) {
      injected_keymap_query.reset();
      injected_keymap_connection = nullptr;
      inject_foreign_key = true;
    }
  }
  if (injected != 0) {
    auto *state = static_cast<xcb_xkb_get_state_reply_t *>(*reply);
    const std::uint8_t modifier =
        injected == 1 ? XCB_MOD_MASK_CONTROL : XCB_MOD_MASK_SHIFT;
    state->mods = static_cast<std::uint8_t>(state->lockedMods | modifier);
    state->baseMods = modifier;
    if (consume_private_fault_marker(
            "/run/punto-e2e-inject-foreign-key-after-layout-state")) {
      std::lock_guard lock{keyboard_query_mutex};
      injected_keymap_connection = connection;
    }
  }
  if (inject_foreign_key) {
    auto *keys = static_cast<xcb_query_keymap_reply_t *>(*reply);
    constexpr unsigned int key = KEY_A + 8U;
    keys->keys[key / 8U] |= static_cast<std::uint8_t>(1U << (key % 8U));
  }
  if (observed) {
    const bool hold = consume_private_fault_marker(
        "/run/punto-e2e-hold-keyboard-observation");
    const int marker =
        ::open("/run/punto-e2e-keyboard-observed",
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      (void)::close(marker);
    }
    // Test-only delayed delivery of an already captured reply, not a server
    // delay or a production timeout. Never retain the mutex while waiting.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (hold && std::chrono::steady_clock::now() < deadline &&
           !consume_private_fault_marker(
               "/run/punto-e2e-release-keyboard-observation")) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
  return result;
}

extern "C" decltype(xcb_disconnect) __real_xcb_disconnect;
extern "C" void __wrap_xcb_disconnect(xcb_connection_t *connection) {
  if (restore_failed_connection.exchange(nullptr, std::memory_order_acq_rel) ==
      connection) {
    restore_reprepare_pending.store(true, std::memory_order_release);
  }
  {
    std::lock_guard lock{keyboard_query_mutex};
    if (keyboard_query && keyboard_query->first == connection) {
      keyboard_query.reset();
    }
    if (failed_keymap_query && failed_keymap_query->first == connection) {
      failed_keymap_query.reset();
    }
    if (failed_xtest_request && failed_xtest_request->first == connection) {
      failed_xtest_request.reset();
    }
    if (failed_focus_query && failed_focus_query->first == connection) {
      failed_focus_query.reset();
      failed_focus_fault = 0;
      focus_failure_pending.store(false, std::memory_order_release);
    }
    if (failed_pointer_query && failed_pointer_query->first == connection) {
      failed_pointer_query.reset();
    }
    if (failed_xinput_version_query &&
        failed_xinput_version_query->first == connection) {
      failed_xinput_version_query.reset();
      failed_xinput_version_fault = 0;
    }
    xtest_requests_until_failure.store(0, std::memory_order_release);
    if (injected_layout_connection == connection) {
      injected_layout_connection = nullptr;
      injected_layout_query.reset();
      injected_layout_state = 0;
    }
    if (injected_keymap_connection == connection) {
      injected_keymap_connection = nullptr;
      injected_keymap_query.reset();
    }
  }
  __real_xcb_disconnect(connection);
}

extern "C" int __real_fsync(int fd);
extern "C" int __wrap_fsync(int fd) {
  constexpr const char *arm = "/run/punto-e2e-fail-directory-fsync";
  struct stat target {
  }, runtime{};
  if (::fstat(fd, &target) == 0 && S_ISDIR(target.st_mode) &&
      ::stat("/run", &runtime) == 0 && target.st_dev == runtime.st_dev &&
      target.st_ino == runtime.st_ino && consume_private_fault_marker(arm)) {
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}

extern "C" decltype(xcb_xkb_get_map) __real_xcb_xkb_get_map;
extern "C" xcb_xkb_get_map_cookie_t __wrap_xcb_xkb_get_map(
    xcb_connection_t *connection, xcb_xkb_device_spec_t device,
    std::uint16_t full, std::uint16_t partial, std::uint8_t first_type,
    std::uint8_t types, xcb_keycode_t first_symbol, std::uint8_t symbols,
    xcb_keycode_t first_action, std::uint8_t actions,
    xcb_keycode_t first_behavior, std::uint8_t behaviors,
    std::uint16_t virtual_modifiers, xcb_keycode_t first_explicit,
    std::uint8_t explicit_count, xcb_keycode_t first_modifier,
    std::uint8_t modifiers, xcb_keycode_t first_virtual_modifier,
    std::uint8_t virtual_modifier_count) {
  if (consume_private_fault_marker("/run/punto-e2e-fail-xkb-map")) {
    return {};
  }
  return __real_xcb_xkb_get_map(
      connection, device, full, partial, first_type, types, first_symbol,
      symbols, first_action, actions, first_behavior, behaviors,
      virtual_modifiers, first_explicit, explicit_count, first_modifier,
      modifiers, first_virtual_modifier, virtual_modifier_count);
}

namespace {

volatile sig_atomic_t g_stop_pipe_write_fd = -1;

void stop_signal_handler(int signal) {
  if (signal != SIGINT && signal != SIGTERM) {
    return;
  }
  const int saved_errno = errno;
  const int fd = static_cast<int>(g_stop_pipe_write_fd);
  if (fd >= 0) {
    constexpr char kStopByte = 'x';
    const ssize_t ignored = ::write(fd, &kStopByte, sizeof(kStopByte));
    (void)ignored;
  }
  errno = saved_errno;
}

const char *required_environment(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    std::cerr << "missing required environment: " << name << '\n';
    std::exit(2);
  }
  return value;
}

punto::x11_detail::ProbeResult probe_test_session() {
  if (std::getenv("PUNTO_E2E_STUCK_PROBE") != nullptr) {
    const int marker =
        ::open("/run/punto-e2e-stuck-probe-ready",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
    for (;;) {
      std::this_thread::sleep_for(std::chrono::hours{1});
    }
  }

  if (std::filesystem::exists("/run/punto-e2e-block-refresh")) {
    const int marker =
        ::open("/run/punto-e2e-refresh-blocked",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
    while (std::filesystem::exists("/run/punto-e2e-block-refresh")) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  const bool session_race =
      std::getenv("PUNTO_E2E_CONFIG_SESSION_RACE") != nullptr;
  const bool new_session =
      session_race && std::filesystem::exists("/run/punto-e2e-switch-session");

  punto::X11SessionInfo info;
  info.session_id =
      new_session ? "punto-event-loop-e2e-new" : "punto-event-loop-e2e";
  info.username = "punto-e2e";
  info.uid = static_cast<std::uint32_t>(::getuid());
  info.gid = static_cast<std::uint32_t>(::getgid());
  const char *probe_display = std::getenv("PUNTO_E2E_PROBE_DISPLAY");
  info.display = probe_display != nullptr && probe_display[0] != '\0'
                     ? probe_display
                     : required_environment("DISPLAY");
  info.xauthority_path = required_environment("XAUTHORITY");
  info.home_dir = session_race ? (new_session ? "/tmp/punto-new-home"
                                              : "/tmp/punto-old-home")
                               : required_environment("HOME");
  info.xdg_runtime_dir = required_environment("XDG_RUNTIME_DIR");
  info.xdg_config_home = session_race ? (new_session ? "/tmp/punto-new-config"
                                                     : "/tmp/punto-old-config")
                                      : required_environment("XDG_CONFIG_HOME");
  info.observed_keyboard_layout = 0;
  if (std::getenv("PUNTO_E2E_DYNAMIC_LAYOUT") != nullptr &&
      std::filesystem::exists("/run/punto-e2e-layout-ru")) {
    info.observed_keyboard_layout = 1;
  }
  if (const char *layout = std::getenv("PUNTO_E2E_OBSERVED_LAYOUT");
      layout != nullptr && (layout[0] == '0' || layout[0] == '1') &&
      layout[1] == '\0') {
    info.observed_keyboard_layout = layout[0] - '0';
  }

  if (new_session) {
    const int marker =
        ::open("/run/punto-e2e-new-session-observed",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
  }

  const int group_count = ::getgroups(0, nullptr);
  if (group_count > 0) {
    std::vector<gid_t> groups(static_cast<std::size_t>(group_count));
    if (::getgroups(group_count, groups.data()) == group_count) {
      info.supplementary_groups.reserve(groups.size());
      for (const gid_t group : groups) {
        info.supplementary_groups.push_back(static_cast<std::uint32_t>(group));
      }
    }
  }

  return {punto::x11_detail::ProbeStatus::Healthy, std::move(info)};
}

punto::ConfigLoadOutcome
blocking_config_loader(const std::filesystem::path &,
                       const std::optional<std::filesystem::path> &,
                       const std::string &) {
  const int marker = ::open("/run/punto-e2e-stuck-config-ready",
                            O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (marker >= 0) {
    ::close(marker);
  }
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours{1});
  }
}

punto::ConfigLoadOutcome session_switch_config_loader(
    const std::filesystem::path &,
    const std::optional<std::filesystem::path> &user_root,
    const std::string &) {
  punto::ConfigLoadOutcome outcome;
  if (!user_root) {
    outcome.result = punto::ConfigResult::IoError;
    outcome.error = "missing session config root";
    return outcome;
  }

  const bool old_session =
      user_root->string().find("punto-old-config") != std::string::npos;
  if (old_session) {
    const int marker =
        ::open("/run/punto-e2e-old-config-ready",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
    while (!std::filesystem::exists("/run/punto-e2e-release-old-config")) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  outcome.config.auto_switch.enabled = old_session;
  outcome.config.sound.enabled = false;
  outcome.config.logging.level = punto::LogLevel::Error;
  outcome.config.runtime.analysis_threads = 1;
  outcome.config.runtime.max_analysis_threads_per_daemon = 1;
  outcome.used_path = *user_root / "config.yaml";
  outcome.config.config_path = outcome.used_path;
  outcome.result = punto::ConfigResult::Ok;

  if (!old_session) {
    const int marker =
        ::open("/run/punto-e2e-new-config-loaded",
               O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (marker >= 0) {
      ::close(marker);
    }
  }
  return outcome;
}

bool write_fixture(const std::filesystem::path &path,
                   const std::string &contents) {
  const int fd =
      ::open(path.c_str(),
             O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        ::write(fd, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return ::close(fd) == 0;
}

struct ScopedFixturePath {
  std::filesystem::path path;
  ~ScopedFixturePath() { (void)::unlink(path.c_str()); }
};

punto::DictionaryLoadOutcome deterministic_dictionary_loader() {
  const std::string suffix = "-" + std::to_string(::getpid()) + ".dic";
  const ScopedFixturePath english{"/tmp/punto-e2e-en" + suffix};
  const ScopedFixturePath russian{"/tmp/punto-e2e-ru" + suffix};
  const ScopedFixturePath affix{"/tmp/punto-e2e" + suffix + ".aff"};
  punto::DictionaryLoadOutcome outcome;
  if (!write_fixture(english.path, "2\nhello\nworld\n") ||
      !write_fixture(russian.path, "2\nпривет\nжест\n") ||
      !write_fixture(affix.path,
                     "SET UTF-8\nTRY esiarntolcdugmphbyfvkwzxjq\n")) {
    return outcome;
  }

  punto::DictionaryLoadSpec spec;
  spec.english_paths = {english.path};
  spec.russian_paths = {russian.path};
  spec.english_affix = affix.path;
  spec.english_hunspell_dictionary = english.path;
  spec.russian_affix = affix.path;
  spec.russian_hunspell_dictionary = russian.path;
  auto dictionary = std::make_unique<punto::Dictionary>();
  outcome.result = dictionary->initialize_bounded(spec);
  if (outcome.result == punto::DictionaryLoadResult::Ok) {
    outcome.dictionary = std::move(dictionary);
  }
  return outcome;
}

punto::DictionaryLoadOutcome oversize_dictionary_loader() {
  const ScopedFixturePath path{"/tmp/punto-e2e-oversize-" +
                               std::to_string(::getpid()) + ".dic"};
  punto::DictionaryLoadOutcome outcome;
  if (!write_fixture(path.path, "1\n" + std::string(128, 'a') + "\n")) {
    return outcome;
  }
  punto::DictionaryLoadSpec spec;
  spec.english_paths = {path.path};
  spec.limits.max_file_bytes = 64;
  auto dictionary = std::make_unique<punto::Dictionary>();
  outcome.result = dictionary->initialize_bounded(spec);
  if (outcome.result == punto::DictionaryLoadResult::Ok) {
    outcome.dictionary = std::move(dictionary);
  }
  return outcome;
}

punto::DictionaryLoadOutcome blocking_dictionary_loader() {
  const int marker = ::open("/run/punto-e2e-stuck-dictionary-ready",
                            O_CREAT | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (marker >= 0) {
    ::close(marker);
  }
  for (;;) {
    std::this_thread::sleep_for(std::chrono::hours{1});
  }
}

} // namespace

int main() {
  punto::set_ipc_mailbox_admitted_test_hook(observe_admitted_macro_ipc);
  int stop_pipe[2] = {-1, -1};
  if (::pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
    std::cerr << "failed to create stop pipe\n";
    return 2;
  }

  struct sigaction stop_action {};
  stop_action.sa_handler = stop_signal_handler;
  ::sigemptyset(&stop_action.sa_mask);

  struct sigaction ignore_sigpipe {};
  ignore_sigpipe.sa_handler = SIG_IGN;
  ::sigemptyset(&ignore_sigpipe.sa_mask);

  g_stop_pipe_write_fd = stop_pipe[1];
  if (::sigaction(SIGPIPE, &ignore_sigpipe, nullptr) != 0 ||
      ::sigaction(SIGINT, &stop_action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &stop_action, nullptr) != 0) {
    g_stop_pipe_write_fd = -1;
    (void)::close(stop_pipe[0]);
    (void)::close(stop_pipe[1]);
    std::cerr << "failed to install signal handlers\n";
    return 2;
  }

  if (const char *proc_root = std::getenv("PUNTO_E2E_PROC_SCAN_ROOT");
      proc_root != nullptr && proc_root[0] != '\0') {
    const std::size_t observed =
        punto::event_loop_detail::count_running_punto_daemons(
            proc_root, 1, std::chrono::milliseconds{100});
    const std::size_t conservative = std::max<std::size_t>(
        std::thread::hardware_concurrency(), static_cast<unsigned int>(1));
    return observed >= conservative ? 0 : 1;
  }

  punto::Config config;
  config.auto_switch.enabled = true;
  config.sound.enabled = false;
  config.logging.level = punto::LogLevel::Error;
  config.runtime.analysis_threads = 1;
  config.runtime.max_analysis_threads_per_daemon = 1;

  punto::EventLoop::ConfigLoaderFunction config_loader;
  if (std::getenv("PUNTO_E2E_STUCK_CONFIG") != nullptr) {
    config_loader = blocking_config_loader;
  } else if (std::getenv("PUNTO_E2E_CONFIG_SESSION_RACE") != nullptr) {
    config_loader = session_switch_config_loader;
  }

  punto::EventLoop::DictionaryLoaderFunction dictionary_loader =
      deterministic_dictionary_loader;
  if (std::getenv("PUNTO_E2E_STUCK_DICTIONARY") != nullptr) {
    dictionary_loader = blocking_dictionary_loader;
  } else if (std::getenv("PUNTO_E2E_OVERSIZE_DICTIONARY") != nullptr) {
    dictionary_loader = oversize_dictionary_loader;
  }

  punto::EventLoop loop{std::move(config), probe_test_session,
                        std::move(config_loader), std::move(dictionary_loader)};
  loop.set_stop_signal_fd(stop_pipe[0]);
  const int result = loop.run();
  g_stop_pipe_write_fd = -1;
  (void)::close(stop_pipe[0]);
  (void)::close(stop_pipe[1]);
  return result;
}
