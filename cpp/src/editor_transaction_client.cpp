#include "punto/editor_transaction.hpp"

#include <json-glib/json-glib.h>
#include <xcb/xcbext.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <poll.h>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace punto {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxMessage = 16 * 1024;
constexpr std::string_view kSocketPrefix = "punto-editor-adapter-";
constexpr std::string_view kSocketSuffix = ".sock";

template <typename T>
using Owned = std::unique_ptr<T, decltype(&g_object_unref)>;
using XcbReply =
    std::unique_ptr<xcb_get_property_reply_t, decltype(&std::free)>;

std::int64_t monotonic_ms(Clock::time_point point) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             point.time_since_epoch())
      .count();
}

int poll_timeout(Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(
      std::clamp<std::int64_t>(remaining.count() + 1, 1, 1000));
}

bool wait_fd(int fd, short events, Clock::time_point deadline) {
  pollfd descriptor{fd, events, 0};
  for (;;) {
    const int result = ::poll(&descriptor, 1, poll_timeout(deadline));
    if (result > 0) {
      return (descriptor.revents & events) != 0 &&
             (descriptor.revents & (POLLERR | POLLNVAL)) == 0;
    }
    if (result == 0 || errno != EINTR) {
      return false;
    }
  }
}

std::string socket_name(const EditorTarget &target) {
  return std::string{kSocketPrefix} + std::to_string(target.uid) + "-" +
         std::to_string(target.pid) + std::string{kSocketSuffix};
}

class Socket final {
public:
  explicit Socket(int fd = -1) : fd_{fd} {}
  ~Socket() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
  int get() const { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }

private:
  int fd_;
};

std::optional<Socket> connect_verified(const EditorTarget &target,
                                       Clock::time_point deadline) {
  const std::string name = socket_name(target);
  if (name.size() + 1 >= sizeof(sockaddr_un::sun_path) ||
      name.find('\0') != std::string::npos) {
    std::fprintf(stderr,
                 "[punto] Editor transport unavailable stage=socket_name\n");
    return std::nullopt;
  }
  Socket socket{
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  if (!socket) {
    return std::nullopt;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  address.sun_path[0] = '\0';
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  const auto length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size() + 1);
  if (::connect(socket.get(), reinterpret_cast<sockaddr *>(&address), length) !=
          0 &&
      errno != EINPROGRESS) {
    std::fprintf(
        stderr, "[punto] Editor transport unavailable stage=connect errno=%d\n",
        errno);
    return std::nullopt;
  }
  if (!wait_fd(socket.get(), POLLOUT, deadline)) {
    std::fprintf(stderr,
                 "[punto] Editor transport unavailable stage=connect_wait\n");
    return std::nullopt;
  }
  int error = 0;
  socklen_t error_size = sizeof(error);
  if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &error_size) !=
          0 ||
      error != 0) {
    std::fprintf(stderr,
                 "[punto] Editor transport unavailable stage=connect_error "
                 "error=%d\n",
                 error);
    return std::nullopt;
  }
  ucred credentials{};
  socklen_t credentials_size = sizeof(credentials);
  if (::getsockopt(socket.get(), SOL_SOCKET, SO_PEERCRED, &credentials,
                   &credentials_size) != 0 ||
      credentials.pid <= 0 ||
      static_cast<std::uint32_t>(credentials.pid) != target.pid ||
      static_cast<std::uint32_t>(credentials.uid) != target.uid) {
    std::fprintf(
        stderr,
        "[punto] Editor transport unavailable stage=peer_identity "
        "expected_pid=%u actual_pid=%d expected_uid=%u actual_uid=%u\n",
        target.pid, credentials.pid, target.uid,
        static_cast<unsigned int>(credentials.uid));
    return std::nullopt;
  }
  return socket;
}

bool send_all(int fd, std::string_view data, Clock::time_point deadline) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    if (!wait_fd(fd, POLLOUT, deadline)) {
      return false;
    }
    const ssize_t count =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
    } else if (count < 0 && errno != EINTR && errno != EAGAIN &&
               errno != EWOULDBLOCK) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> receive_line(int fd, Clock::time_point deadline) {
  std::string response;
  response.reserve(1024);
  while (response.size() <= kMaxMessage) {
    if (!wait_fd(fd, POLLIN, deadline)) {
      return std::nullopt;
    }
    char buffer[1024];
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) {
      if (count < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      return std::nullopt;
    }
    response.append(buffer, static_cast<std::size_t>(count));
    const auto newline = response.find('\n');
    if (newline != std::string::npos) {
      if (newline + 1 != response.size()) {
        return std::nullopt;
      }
      response.resize(newline);
      return response;
    }
  }
  return std::nullopt;
}

std::optional<std::string> exchange(const EditorTarget &target,
                                    std::string request,
                                    Clock::time_point deadline,
                                    bool read_response = true,
                                    const std::function<void()> &on_sent = {}) {
  if (request.size() >= kMaxMessage || Clock::now() >= deadline) {
    return std::nullopt;
  }
  request.push_back('\n');
  auto socket = connect_verified(target, deadline);
  if (!socket || !send_all(socket->get(), request, deadline)) {
    return std::nullopt;
  }
  (void)::shutdown(socket->get(), SHUT_WR);
  if (on_sent) {
    on_sent();
  }
  if (!read_response) {
    return std::string{};
  }
  return receive_line(socket->get(), deadline);
}

std::string json_string(JsonBuilder *builder) {
  Owned<JsonGenerator> generator{json_generator_new(), &g_object_unref};
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator.get(), root);
  gchar *data = json_generator_to_data(generator.get(), nullptr);
  std::string result = data == nullptr ? std::string{} : std::string{data};
  g_free(data);
  json_node_free(root);
  return result;
}

std::string operation_id() {
  static std::atomic<std::uint64_t> sequence{0};
  // Uniqueness is process-local: the peer also keys operations by its own
  // editor instance.  Avoid random_device here because it may throw or block
  // in the input path on entropy-starved systems.
  static const std::uint64_t nonce = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
  return std::to_string(::getpid()) + "-" + std::to_string(nonce) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

void *wait_xcb_reply(xcb_connection_t *connection, unsigned int sequence,
                     Clock::time_point deadline) {
  void *reply = nullptr;
  for (;;) {
    xcb_generic_error_t *error = nullptr;
    if (xcb_poll_for_reply(connection, sequence, &reply, &error) != 0) {
      const bool failed = error != nullptr;
      std::free(error);
      if (failed) {
        std::free(reply);
        return nullptr;
      }
      return reply;
    }
    std::free(error);
    if (xcb_connection_has_error(connection) != 0 || Clock::now() >= deadline ||
        !wait_fd(xcb_get_file_descriptor(connection), POLLIN, deadline)) {
      return nullptr;
    }
  }
}

std::optional<std::uint32_t> atom_value(xcb_connection_t *connection,
                                        xcb_window_t window,
                                        xcb_atom_t property, xcb_atom_t type,
                                        Clock::time_point deadline) {
  const auto cookie =
      xcb_get_property(connection, 0, window, property, type, 0, 1);
  (void)xcb_flush(connection);
  XcbReply reply{static_cast<xcb_get_property_reply_t *>(
                     wait_xcb_reply(connection, cookie.sequence, deadline)),
                 &std::free};
  if (!reply || reply->format != 32 ||
      xcb_get_property_value_length(reply.get()) != 4) {
    return std::nullopt;
  }
  return *static_cast<const std::uint32_t *>(
      xcb_get_property_value(reply.get()));
}

xcb_atom_t intern(xcb_connection_t *connection, std::string_view name,
                  Clock::time_point deadline) {
  const auto cookie = xcb_intern_atom(
      connection, 0, static_cast<std::uint16_t>(name.size()), name.data());
  (void)xcb_flush(connection);
  std::unique_ptr<xcb_intern_atom_reply_t, decltype(&std::free)> reply{
      static_cast<xcb_intern_atom_reply_t *>(
          wait_xcb_reply(connection, cookie.sequence, deadline)),
      &std::free};
  return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
}

JsonObject *parse_object(const std::string &input, Owned<JsonParser> &parser) {
  parser.reset(json_parser_new());
  GError *error = nullptr;
  if (!json_parser_load_from_data(parser.get(), input.data(),
                                  static_cast<gssize>(input.size()), &error)) {
    g_clear_error(&error);
    return nullptr;
  }
  JsonNode *root = json_parser_get_root(parser.get());
  return root != nullptr && JSON_NODE_HOLDS_OBJECT(root)
             ? json_node_get_object(root)
             : nullptr;
}

bool member_string(JsonObject *object, const char *name, std::string &value,
                   std::size_t maximum = 4096) {
  JsonNode *node = json_object_get_member(object, name);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    return false;
  }
  const char *text = json_object_get_string_member(object, name);
  if (text == nullptr) {
    return false;
  }
  value = text;
  return value.size() <= maximum && value.find('\0') == std::string::npos;
}

bool member_bool(JsonObject *object, const char *name, bool &value) {
  JsonNode *node = json_object_get_member(object, name);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_BOOLEAN) {
    return false;
  }
  value = json_node_get_boolean(node);
  return true;
}

std::optional<std::uint64_t> member_u64(JsonObject *object, const char *name) {
  JsonNode *node = json_object_get_member(object, name);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_INT64) {
    return std::nullopt;
  }
  const gint64 value = json_node_get_int(node);
  return value < 0
             ? std::optional<std::uint64_t>{}
             : std::optional<std::uint64_t>{static_cast<std::uint64_t>(value)};
}

std::optional<std::uint32_t> member_u32(JsonObject *object, const char *name) {
  const auto value = member_u64(object, name);
  if (!value || *value > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*value);
}

void append_identity_field(std::string &value, std::string_view field) {
  value += std::to_string(field.size());
  value.push_back(':');
  value.append(field);
  value.push_back('|');
}

std::string command_identity(const PreparedEditorTransaction &prepared,
                             std::string_view replacement) {
  std::string canonical;
  canonical.reserve(prepared.fragment.size() + replacement.size() + 256);
  append_identity_field(canonical, prepared.editor_instance);
  append_identity_field(canonical, prepared.mode == EditorTransactionMode::Word
                                       ? "word"
                                       : "selection");
  append_identity_field(canonical, std::to_string(prepared.target.window));
  append_identity_field(canonical, std::to_string(prepared.widget_epoch));
  append_identity_field(canonical, std::to_string(prepared.context_generation));
  append_identity_field(canonical, std::to_string(prepared.revision));
  append_identity_field(canonical, std::to_string(prepared.start));
  append_identity_field(canonical, std::to_string(prepared.end));
  append_identity_field(canonical, prepared.fragment);
  append_identity_field(canonical, replacement);
  append_identity_field(canonical,
                        std::to_string(prepared.not_after_monotonic_ms));
  gchar *digest =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical.data(),
                                    static_cast<gssize>(canonical.size()));
  std::string result = digest == nullptr ? std::string{} : std::string{digest};
  g_free(digest);
  return result;
}

} // namespace

std::optional<EditorTarget> EditorTransactionClient::discover(
    xcb_connection_t *connection, int screen_number,
    xcb_window_t expected_focus, std::uint32_t session_uid,
    std::string_view runtime_directory, Clock::time_point deadline) {
  if (connection == nullptr || expected_focus == XCB_WINDOW_NONE ||
      runtime_directory.empty() || Clock::now() >= deadline) {
    return std::nullopt;
  }
  const xcb_setup_t *setup = xcb_get_setup(connection);
  auto screen = xcb_setup_roots_iterator(setup);
  for (int index = 0; index < screen_number && screen.rem > 0; ++index) {
    xcb_screen_next(&screen);
  }
  if (screen.rem == 0) {
    return std::nullopt;
  }
  const xcb_atom_t active_atom =
      intern(connection, "_NET_ACTIVE_WINDOW", deadline);
  const xcb_atom_t pid_atom = intern(connection, "_NET_WM_PID", deadline);
  if (active_atom == XCB_ATOM_NONE || pid_atom == XCB_ATOM_NONE) {
    return std::nullopt;
  }
  const auto active = atom_value(connection, screen.data->root, active_atom,
                                 XCB_GET_PROPERTY_TYPE_ANY, deadline);
  if (!active || *active == XCB_WINDOW_NONE) {
    return std::nullopt;
  }
  // GTK may use an input-method focus proxy rather than a descendant of the
  // EWMH top-level.  WordEditor has already pinned expected_focus; the adapter
  // authoritatively binds the focused widget to the active top-level XID.
  (void)expected_focus;
  const auto pid = atom_value(connection, *active, pid_atom,
                              XCB_GET_PROPERTY_TYPE_ANY, deadline);
  if (!pid || *pid == 0) {
    return std::nullopt;
  }
  // The focused child may differ from the EWMH top-level.  The adapter performs
  // the authoritative focused-widget/top-level check inside the GTK process.
  return EditorTarget{session_uid, *pid, *active,
                      std::string{runtime_directory}};
}

EditorTransactionClient::EditorTransactionClient(EditorTarget target)
    : target_{std::move(target)} {}

std::optional<PreparedEditorTransaction> EditorTransactionClient::prepare(
    EditorTransactionMode mode, std::string_view expected_fragment,
    Clock::time_point request_deadline, Clock::time_point operation_deadline,
    std::string_view request_key) {
  const bool valid_key =
      request_key.size() <= 128 &&
      std::all_of(request_key.begin(), request_key.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') || ch == ':' || ch == '-' ||
               ch == '_' || ch == '.';
      });
  if (expected_fragment.size() > 4096 || Clock::now() >= request_deadline ||
      request_deadline > operation_deadline || !valid_key) {
    return std::nullopt;
  }
  Owned<JsonBuilder> builder{json_builder_new(), &g_object_unref};
  json_builder_begin_object(builder.get());
  json_builder_set_member_name(builder.get(), "protocol");
  json_builder_add_int_value(builder.get(), 1);
  json_builder_set_member_name(builder.get(), "action");
  json_builder_add_string_value(builder.get(), "prepare");
  const std::string id =
      request_key.empty() ? operation_id() : std::string{request_key};
  json_builder_set_member_name(builder.get(), "operation_id");
  json_builder_add_string_value(builder.get(), id.c_str());
  json_builder_set_member_name(builder.get(), "mode");
  json_builder_add_string_value(
      builder.get(),
      mode == EditorTransactionMode::Word ? "word" : "selection");
  json_builder_set_member_name(builder.get(), "expected_fragment");
  json_builder_add_string_value(builder.get(),
                                std::string{expected_fragment}.c_str());
  json_builder_set_member_name(builder.get(), "window");
  json_builder_add_int_value(builder.get(), target_.window);
  json_builder_set_member_name(builder.get(), "not_after_ms");
  json_builder_add_int_value(builder.get(), monotonic_ms(operation_deadline));
  json_builder_end_object(builder.get());
  const auto response =
      exchange(target_, json_string(builder.get()), request_deadline);
  if (!response) {
    std::fprintf(
        stderr,
        "[punto] Editor prepare rejected stage=transport_unavailable\n");
    return std::nullopt;
  }
  Owned<JsonParser> parser{nullptr, &g_object_unref};
  JsonObject *object = parse_object(*response, parser);
  if (object == nullptr) {
    return std::nullopt;
  }
  bool ok = false;
  if (!member_bool(object, "ok", ok) || !ok) {
    std::string reason;
    if (member_string(object, "reason", reason)) {
      std::fprintf(stderr, "[punto] Editor prepare rejected stage=%s\n",
                   reason.c_str());
    }
    return std::nullopt;
  }
  PreparedEditorTransaction prepared;
  prepared.target = target_;
  prepared.operation_id = id;
  prepared.mode = mode;
  prepared.not_after_monotonic_ms = monotonic_ms(operation_deadline);
  if (!member_string(object, "operation_id", prepared.operation_id) ||
      prepared.operation_id != id) {
    std::fprintf(stderr,
                 "[punto] Editor prepare reply invalid stage=operation_id\n");
    return std::nullopt;
  }
  if (!member_string(object, "editor_instance", prepared.editor_instance,
                     128)) {
    std::fprintf(
        stderr, "[punto] Editor prepare reply invalid stage=editor_instance\n");
    return std::nullopt;
  }
  const auto widget_epoch = member_u64(object, "widget_epoch");
  const auto context_generation = member_u64(object, "context_generation");
  const auto revision = member_u64(object, "revision");
  const auto start = member_u32(object, "start");
  const auto end = member_u32(object, "end");
  if (!widget_epoch || !context_generation || !revision || !start || !end ||
      !member_string(object, "fragment", prepared.fragment)) {
    std::fprintf(stderr, "[punto] Editor prepare reply invalid stage=fields\n");
    return std::nullopt;
  }
  prepared.widget_epoch = *widget_epoch;
  prepared.context_generation = *context_generation;
  prepared.revision = *revision;
  prepared.start = *start;
  prepared.end = *end;
  if (prepared.widget_epoch == 0 || prepared.context_generation == 0 ||
      prepared.revision == 0 ||
      prepared.widget_epoch > static_cast<std::uint64_t>(INT64_MAX) ||
      prepared.context_generation > static_cast<std::uint64_t>(INT64_MAX) ||
      prepared.revision > static_cast<std::uint64_t>(INT64_MAX) ||
      prepared.start > prepared.end || prepared.fragment.size() > 4096 ||
      (mode == EditorTransactionMode::Word &&
       prepared.fragment != expected_fragment) ||
      (mode == EditorTransactionMode::Selection && prepared.fragment.empty())) {
    std::fprintf(stderr,
                 "[punto] Editor prepare reply invalid stage=contract\n");
    return std::nullopt;
  }
  return prepared;
}

namespace {
std::string apply_request(const PreparedEditorTransaction &prepared,
                          std::string_view replacement,
                          std::string_view action = "compare_apply",
                          std::string_view frame_id = {},
                          std::string_view frame_digest = {},
                          std::uint32_t trigger_index = 0,
                          std::string_view barrier_token = {}) {
  Owned<JsonBuilder> builder{json_builder_new(), &g_object_unref};
  json_builder_begin_object(builder.get());
  const auto add_string = [&](const char *name, std::string_view value) {
    json_builder_set_member_name(builder.get(), name);
    json_builder_add_string_value(builder.get(), std::string{value}.c_str());
  };
  const auto add_int = [&](const char *name, std::int64_t value) {
    json_builder_set_member_name(builder.get(), name);
    json_builder_add_int_value(builder.get(), value);
  };
  add_int("protocol", 1);
  add_string("action", action);
  add_string("operation_id", prepared.operation_id);
  add_string("editor_instance", prepared.editor_instance);
  add_string("mode", prepared.mode == EditorTransactionMode::Word
                         ? "word"
                         : "selection");
  add_int("window", prepared.target.window);
  add_int("widget_epoch", static_cast<std::int64_t>(prepared.widget_epoch));
  add_int("context_generation",
          static_cast<std::int64_t>(prepared.context_generation));
  add_int("expected_revision", static_cast<std::int64_t>(prepared.revision));
  add_int("start", prepared.start);
  add_int("end", prepared.end);
  add_string("expected_fragment", prepared.fragment);
  add_string("replacement", replacement);
  add_int("not_after_ms", prepared.not_after_monotonic_ms);
  add_string("command_identity", command_identity(prepared, replacement));
  if (!frame_id.empty()) {
    add_string("frame_id", frame_id);
    add_string("frame_digest", frame_digest);
    add_int("trigger_index", trigger_index);
  }
  if (!barrier_token.empty()) {
    add_string("barrier_token", barrier_token);
  }
  json_builder_end_object(builder.get());
  return json_string(builder.get());
}
} // namespace

EditorTransactionResult EditorTransactionClient::compare_apply(
    const PreparedEditorTransaction &prepared, std::string_view replacement,
    Clock::time_point deadline) {
  EditorTransactionResult result;
  result.operation_id = prepared.operation_id;
  const std::string expected_command_identity =
      command_identity(prepared, replacement);
  if (prepared.target.pid != target_.pid ||
      prepared.target.uid != target_.uid ||
      prepared.target.window != target_.window || prepared.widget_epoch == 0 ||
      prepared.context_generation == 0 || prepared.revision == 0 ||
      replacement.empty() || replacement.size() > 4096 ||
      expected_command_identity.size() != 64 || Clock::now() >= deadline) {
    result.status = EditorTransactionStatus::Rejected;
    result.reason = "invalid_client_request";
    return result;
  }
  result.execution = EditorTransactionExecution::Unknown;
  result.effect = EditorTransactionEffect::Unknown;
  const std::string request = apply_request(prepared, replacement);
  auto response = exchange(target_, request, deadline);
  if (!response && Clock::now() < deadline) {
    // The editor owns idempotency.  Retrying the exact operation id turns a
    // lost acknowledgement into a result query without risking a second edit.
    response = exchange(target_, request, deadline);
  }
  if (!response) {
    result.status = EditorTransactionStatus::Unknown;
    result.reason = "reply_unavailable";
    return result;
  }
  Owned<JsonParser> parser{nullptr, &g_object_unref};
  JsonObject *object = parse_object(*response, parser);
  std::string status;
  if (object == nullptr ||
      !member_string(object, "operation_id", result.operation_id) ||
      result.operation_id != prepared.operation_id ||
      !member_string(object, "status", status, 16) ||
      !member_string(object, "command_identity", result.command_identity, 64) ||
      result.command_identity != expected_command_identity) {
    result.status = EditorTransactionStatus::Unknown;
    result.reason = "invalid_reply";
    return result;
  }
  if (status == "APPLIED") {
    result.status = EditorTransactionStatus::Applied;
  } else if (status == "REJECTED") {
    result.status = EditorTransactionStatus::Rejected;
  } else {
    result.status = EditorTransactionStatus::Unknown;
  }
  const auto resulting_revision = member_u64(object, "resulting_revision");
  const auto resulting_start = member_u32(object, "resulting_start");
  const auto resulting_end = member_u32(object, "resulting_end");
  if (!resulting_revision || !resulting_start || !resulting_end) {
    result.status = EditorTransactionStatus::Unknown;
    result.reason = "invalid_result_fields";
    return result;
  }
  result.resulting_revision = *resulting_revision;
  result.resulting_start = *resulting_start;
  result.resulting_end = *resulting_end;
  (void)member_string(object, "reason", result.reason);
  if (result.status == EditorTransactionStatus::Applied) {
    const glong replacement_length = g_utf8_strlen(
        replacement.data(), static_cast<gssize>(replacement.size()));
    const std::uint64_t expected_end =
        static_cast<std::uint64_t>(prepared.start) +
        static_cast<std::uint64_t>(std::max<glong>(replacement_length, 0));
    if (replacement_length < 0 || expected_end > UINT32_MAX ||
        prepared.revision > UINT64_MAX - 2 ||
        result.resulting_revision != prepared.revision + 2 ||
        result.resulting_start != prepared.start ||
        result.resulting_end != expected_end) {
      result.status = EditorTransactionStatus::Unknown;
      result.reason = "invalid_applied_result";
    }
  }
  if (result.status == EditorTransactionStatus::Applied) {
    result.execution = EditorTransactionExecution::Quiescent;
    result.effect = EditorTransactionEffect::Applied;
  } else if (result.status == EditorTransactionStatus::Rejected) {
    result.execution = EditorTransactionExecution::Quiescent;
    result.effect = EditorTransactionEffect::Rejected;
  }
  return result;
}

EditorTransactionResult EditorTransactionClient::compare_apply_before_tab(
    const PreparedEditorTransaction &prepared, std::string_view replacement,
    std::string_view frame_id, std::string_view frame_digest,
    std::uint32_t trigger_index, Clock::time_point deadline,
    const std::function<bool()> &before_decide,
    const std::function<void()> &release_original_frame,
    const std::function<bool(Clock::time_point)> &buffer_input) {
  EditorTransactionResult result;
  result.operation_id = prepared.operation_id;
  result.command_identity = command_identity(prepared, replacement);
  result.status = EditorTransactionStatus::Rejected;
  result.reason = "invalid_tab_request";
  if (prepared.target.pid != target_.pid ||
      prepared.target.uid != target_.uid ||
      prepared.target.window != target_.window ||
      prepared.mode != EditorTransactionMode::Word ||
      prepared.widget_epoch == 0 || prepared.context_generation == 0 ||
      prepared.revision == 0 || replacement.empty() ||
      replacement.size() > 4096 || frame_id.empty() || frame_id.size() > 128 ||
      frame_digest.size() != 64 || result.command_identity.size() != 64 ||
      Clock::now() >= deadline ||
      monotonic_ms(deadline) != prepared.not_after_monotonic_ms) {
    return result;
  }
  struct GateReply {
    std::string status;
    std::string reason;
    std::string token;
    std::uint64_t revision = 0;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
  };
  const auto decode = [&](const std::optional<std::string> &wire)
      -> std::optional<GateReply> {
    if (!wire) {
      return std::nullopt;
    }
    Owned<JsonParser> parser{nullptr, &g_object_unref};
    JsonObject *object = parse_object(*wire, parser);
    if (!object) {
      return std::nullopt;
    }
    GateReply reply;
    std::string id;
    std::string identity;
    if (!member_string(object, "operation_id", id, 128) ||
        id != prepared.operation_id ||
        !member_string(object, "command_identity", identity, 64) ||
        identity != result.command_identity ||
        !member_string(object, "status", reply.status, 16)) {
      return std::nullopt;
    }
    (void)member_string(object, "reason", reply.reason);
    (void)member_string(object, "barrier_token", reply.token, 128);
    reply.revision = member_u64(object, "resulting_revision").value_or(0);
    reply.start = member_u32(object, "resulting_start").value_or(0);
    reply.end = member_u32(object, "resulting_end").value_or(0);
    return reply;
  };
  const auto request = [&](std::string_view action,
                           std::string_view token = {}) {
    return apply_request(prepared, replacement, action, frame_id, frame_digest,
                         trigger_index, token);
  };
  auto armed = decode(exchange(target_, request("arm_tab"), deadline));
  if (!armed || armed->status != "ARMED" || armed->token.empty()) {
    result.reason = armed ? armed->reason : "arm_unavailable";
    return result;
  }
  const std::string token = armed->token;
  for (;;) {
    if (Clock::now() >= deadline) {
      result.reason = "ready_deadline";
      return result;
    }
    auto ready = decode(exchange(target_, request("tab_status", token),
                                 deadline));
    if (!ready) {
      result.reason = "ready_unavailable";
      return result;
    }
    if (ready->status == "READY") {
      break;
    }
    if (ready->status != "ARMED") {
      result.reason = ready->reason;
      return result;
    }
    const auto next = std::min(deadline, Clock::now() +
                                            std::chrono::milliseconds{2});
    if (buffer_input) {
      if (!buffer_input(next)) {
        result.reason = "input_backpressure_or_cancelled";
        return result;
      }
    } else {
      std::this_thread::sleep_until(next);
    }
  }
  if (!before_decide || !before_decide() || Clock::now() >= deadline) {
    result.reason = "layout_or_context_before_decision";
    return result;
  }
  // A complete DECIDE send, not an ACK, is the point from which the editor
  // may still write. Keep its identity even when the response is lost.
  bool decision_sent = false;
  const auto decide_deadline =
      std::min(deadline, Clock::now() + std::chrono::milliseconds{20});
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  const bool drop_decide_ack =
      std::getenv("PUNTO_E2E_DROP_TAB_DECIDE_ACK") != nullptr;
#else
  constexpr bool drop_decide_ack = false;
#endif
  auto decided = decode(exchange(target_, request("decide_tab", token),
                                 decide_deadline, !drop_decide_ack, [&] {
                                   decision_sent = true;
                                   release_original_frame();
                                 }));
  if (!decision_sent) {
    result.reason = "decision_not_sent";
    return result;
  }
  result.status = EditorTransactionStatus::Unknown;
  result.execution = EditorTransactionExecution::Active;
  result.effect = EditorTransactionEffect::Unknown;
  result.reason = decided ? decided->reason : "decision_ack_unavailable";
  result.active_tab = EditorTabTransaction{
      prepared, std::string{replacement}, std::string{frame_id},
      std::string{frame_digest}, trigger_index, token,
      result.command_identity};
  return result;
}

EditorTransactionResult EditorTransactionClient::poll_tab_status(
    const EditorTabTransaction &transaction, Clock::time_point deadline) {
  EditorTransactionResult result;
  result.operation_id = transaction.prepared.operation_id;
  result.command_identity = transaction.command_identity;
  result.status = EditorTransactionStatus::Unknown;
  result.execution = EditorTransactionExecution::Unknown;
  result.effect = EditorTransactionEffect::Unknown;
  if (transaction.prepared.target.pid != target_.pid ||
      transaction.prepared.target.uid != target_.uid ||
      transaction.prepared.target.window != target_.window ||
      transaction.barrier_token.empty() ||
      transaction.command_identity !=
          command_identity(transaction.prepared, transaction.replacement)) {
    result.reason = "invalid_tab_handle";
    return result;
  }
  const auto wire = exchange(
      target_, apply_request(transaction.prepared, transaction.replacement,
                             "tab_status", transaction.frame_id,
                             transaction.frame_digest,
                             transaction.trigger_index,
                             transaction.barrier_token),
      deadline);
  Owned<JsonParser> parser{nullptr, &g_object_unref};
  JsonObject *object = wire ? parse_object(*wire, parser) : nullptr;
  std::string id, identity, status, lifecycle;
  if (object == nullptr ||
      !member_string(object, "operation_id", id, 128) ||
      id != result.operation_id ||
      !member_string(object, "command_identity", identity, 64) ||
      identity != result.command_identity ||
      !member_string(object, "status", status, 16)) {
    result.reason = "status_unavailable";
    return result;
  }
  (void)member_string(object, "reason", result.reason);
  if (!member_string(object, "lifecycle", lifecycle, 16) ||
      lifecycle != "QUIESCENT") {
    result.execution = status == "ACTIVE"
                           ? EditorTransactionExecution::Active
                           : EditorTransactionExecution::Unknown;
    return result;
  }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  static std::atomic<bool> terminal_reply_dropped{false};
  if (std::getenv("PUNTO_E2E_DROP_TAB_TERMINAL_REPLY") != nullptr &&
      !terminal_reply_dropped.exchange(true)) {
    result.reason = "test_terminal_reply_lost";
    return result;
  }
#endif
  result.execution = EditorTransactionExecution::Quiescent;
  result.resulting_revision =
      member_u64(object, "resulting_revision").value_or(0);
  result.resulting_start = member_u32(object, "resulting_start").value_or(0);
  result.resulting_end = member_u32(object, "resulting_end").value_or(0);
  if (status == "REJECTED") {
    result.status = EditorTransactionStatus::Rejected;
    result.effect = EditorTransactionEffect::Rejected;
  } else if (status == "APPLIED") {
    const glong length = g_utf8_strlen(
        transaction.replacement.data(),
        static_cast<gssize>(transaction.replacement.size()));
    const auto &prepared = transaction.prepared;
    if (length >= 0 && prepared.revision <= UINT64_MAX - 2 &&
        result.resulting_revision == prepared.revision + 2 &&
        result.resulting_start == prepared.start &&
        static_cast<std::uint64_t>(result.resulting_end) ==
            static_cast<std::uint64_t>(prepared.start) +
                static_cast<std::uint64_t>(length)) {
      result.status = EditorTransactionStatus::Applied;
      result.effect = EditorTransactionEffect::Applied;
    } else {
      result.reason = "invalid_applied_result";
    }
  }
  return result;
}

#if defined(PUNTO_EDITOR_TRANSACTION_TESTING)
bool EditorTransactionClient::compare_apply_without_read_for_test(
    const PreparedEditorTransaction &prepared, std::string_view replacement,
    Clock::time_point deadline) {
  return exchange(target_, apply_request(prepared, replacement), deadline,
                  false)
      .has_value();
}
#endif

} // namespace punto
