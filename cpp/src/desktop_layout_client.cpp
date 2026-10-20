#include "punto/desktop_layout_client.hpp"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <systemd/sd-bus.h>

#include <fcntl.h>
#include <linux/limits.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace punto {
namespace {

#if defined(PUNTO_LAYOUT_SWITCH_E2E_TESTING)
extern "C" int punto_e2e_desktop_layout_switch(int source_layout,
                                               int target_layout);
#endif
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
extern "C" void punto_e2e_after_layout_applied(std::int64_t deadline_ns);
#endif
#if defined(PUNTO_DESKTOP_LAYOUT_PUMP_TESTING)
extern "C" void punto_test_pump_started();
extern "C" void punto_test_pending_call_completed();
#endif

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kState = 0;
constexpr std::uint32_t kApplied = 1;
constexpr std::uint32_t kNotFound = 18;
constexpr std::uint32_t kHopUnknown = 22;
constexpr auto kCallBudget = std::chrono::milliseconds{900};
constexpr auto kPumpStep = std::chrono::milliseconds{1};
constexpr std::size_t kMaxPumpMessagesPerSlice = 32;
constexpr std::size_t kMaxDescriptorBytes = 1024;
constexpr char kPath[] = "/org/punto/Switcher/SessionBridge1";
constexpr char kInterface[] = "org.punto.Switcher.SessionBridge1";

struct BusDeleter {
  void operator()(sd_bus *bus) const noexcept { sd_bus_unref(bus); }
};
struct MessageDeleter {
  void operator()(sd_bus_message *message) const noexcept {
    sd_bus_message_unref(message);
  }
};
struct SlotDeleter {
  void operator()(sd_bus_slot *slot) const noexcept { sd_bus_slot_unref(slot); }
};

using Bus = std::unique_ptr<sd_bus, BusDeleter>;
using Message = std::unique_ptr<sd_bus_message, MessageDeleter>;
using Slot = std::unique_ptr<sd_bus_slot, SlotDeleter>;

bool lower_hex(std::string_view value, std::size_t length) {
  if (value.size() != length)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f')))
      return false;
  }
  return true;
}

std::string random_id() {
  std::array<unsigned char, 16> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return {};
    offset += static_cast<std::size_t>(count);
  }
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result;
  result.resize(32);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result[index * 2] = alphabet[bytes[index] >> 4U];
    result[index * 2 + 1] = alphabet[bytes[index] & 0x0fU];
  }
  return result;
}

std::uint64_t monotonic_us(Deadline value = Clock::now()) {
  const auto count = std::chrono::duration_cast<std::chrono::microseconds>(
                         value.time_since_epoch())
                         .count();
  return count <= 0 ? 0 : static_cast<std::uint64_t>(count);
}

std::uint64_t timeout_us(Deadline deadline) {
  const auto now = Clock::now();
  if (now >= deadline)
    return 0;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(deadline - now)
          .count());
}

std::optional<std::string> read_descriptor(const std::string &path,
                                           std::uint32_t uid) {
  const int fd =
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return std::nullopt;
  struct stat metadata {};
  if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != uid || (metadata.st_mode & 0777) != 0600 ||
      metadata.st_nlink != 1 || metadata.st_size <= 0 ||
      metadata.st_size > static_cast<off_t>(kMaxDescriptorBytes)) {
    close(fd);
    return std::nullopt;
  }
  std::string contents(static_cast<std::size_t>(metadata.st_size), '\0');
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count =
        read(fd, contents.data() + offset, contents.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      close(fd);
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(count);
  }
  const bool stable = fstat(fd, &metadata) == 0 &&
                      metadata.st_size == static_cast<off_t>(contents.size());
  close(fd);
  return stable ? std::optional{std::move(contents)} : std::nullopt;
}

struct Descriptor {
  std::string socket_path;
  std::string helper_instance;
  pid_t pid = 0;
  std::uint64_t start_time = 0;
};

std::optional<std::uint64_t> process_start_time(pid_t pid) {
  if (pid <= 0)
    return std::nullopt;
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return std::nullopt;
  std::array<char, 4096> buffer{};
  ssize_t count = 0;
  do {
    count = read(fd, buffer.data(), buffer.size() - 1);
  } while (count < 0 && errno == EINTR);
  close(fd);
  if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size() - 1)
    return std::nullopt;
  std::string_view contents{buffer.data(), static_cast<std::size_t>(count)};
  const auto close_paren = contents.rfind(')');
  if (close_paren == std::string_view::npos)
    return std::nullopt;
  contents.remove_prefix(close_paren + 1);
  for (unsigned int field = 3; field <= 22; ++field) {
    const auto start = contents.find_first_not_of(' ');
    if (start == std::string_view::npos)
      return std::nullopt;
    contents.remove_prefix(start);
    const auto end = contents.find(' ');
    const auto token = contents.substr(0, end);
    if (field == 22) {
      std::uint64_t value = 0;
      const auto parsed =
          std::from_chars(token.data(), token.data() + token.size(), value);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != token.data() + token.size() || value == 0)
        return std::nullopt;
      return value;
    }
    if (end == std::string_view::npos)
      return std::nullopt;
    contents.remove_prefix(end);
  }
  return std::nullopt;
}

std::optional<Descriptor> parse_descriptor(const X11SessionInfo &session) {
  if (
#if !defined(PUNTO_DESKTOP_LAYOUT_CLIENT_TESTING)
      session.uid == 0 ||
#endif
      session.xdg_runtime_dir.empty()
#if !defined(PUNTO_DESKTOP_LAYOUT_CLIENT_TESTING)
      || session.xdg_runtime_dir != "/run/user/" + std::to_string(session.uid)
#endif
  )
    return std::nullopt;
  const std::string runtime = session.xdg_runtime_dir + "/punto";
  struct stat directory {};
  if (lstat(runtime.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
      directory.st_uid != session.uid || (directory.st_mode & 0777) != 0700)
    return std::nullopt;
  const auto contents =
      read_descriptor(runtime + "/session-bridge.json", session.uid);
  if (!contents)
    return std::nullopt;
  JsonParser *raw_parser = json_parser_new();
  if (raw_parser == nullptr)
    return std::nullopt;
  std::unique_ptr<JsonParser, decltype(&g_object_unref)> parser{
      raw_parser, &g_object_unref};
  GError *raw_error = nullptr;
  const bool loaded = json_parser_load_from_data(
      parser.get(), contents->data(), static_cast<gssize>(contents->size()),
      &raw_error);
  std::unique_ptr<GError, decltype(&g_error_free)> error{raw_error,
                                                         &g_error_free};
  JsonNode *root = loaded ? json_parser_get_root(parser.get()) : nullptr;
  if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root))
    return std::nullopt;
  JsonObject *object = json_node_get_object(root);
  const auto member = [&](const char *name) {
    return json_object_has_member(object, name)
               ? json_object_get_member(object, name)
               : nullptr;
  };
  JsonNode *protocol = member("protocol_version");
  JsonNode *socket = member("socket_path");
  JsonNode *helper = member("helper_instance");
  JsonNode *pid = member("pid");
  JsonNode *start_time = member("start_time");
  const auto integer = [](JsonNode *node) {
    return node != nullptr && JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_INT64;
  };
  const auto string = [](JsonNode *node) {
    return node != nullptr && JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_STRING;
  };
  if (json_object_get_size(object) != 5 || !integer(protocol) ||
      json_node_get_int(protocol) != kProtocolVersion || !string(socket) ||
      !string(helper) || !integer(pid) || json_node_get_int(pid) <= 0 ||
      !integer(start_time) || json_node_get_int(start_time) <= 0)
    return std::nullopt;
  const auto parsed_pid = json_node_get_int(pid);
  const auto parsed_start_time = json_node_get_int(start_time);
  if (parsed_pid > std::numeric_limits<pid_t>::max())
    return std::nullopt;
  Descriptor result{json_node_get_string(socket), json_node_get_string(helper),
                    static_cast<pid_t>(parsed_pid),
                    static_cast<std::uint64_t>(parsed_start_time)};
  const std::string prefix = runtime + "/session-bridge-";
  if (!lower_hex(result.helper_instance, 32) ||
      result.socket_path != prefix + result.helper_instance + ".sock" ||
      result.socket_path.size() >= sizeof(sockaddr_un::sun_path))
    return std::nullopt;
  struct stat socket_metadata {};
  if (lstat(result.socket_path.c_str(), &socket_metadata) != 0 ||
      !S_ISSOCK(socket_metadata.st_mode) ||
      socket_metadata.st_uid != session.uid ||
      (socket_metadata.st_mode & 0777) != 0600)
    return std::nullopt;
  return result;
}

Bus connect_bus(const Descriptor &descriptor, std::uint32_t expected_uid,
                Deadline deadline,
                const DesktopLayoutClient::WaitFunction &wait) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return {};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, descriptor.socket_path.c_str(),
              descriptor.socket_path.size() + 1);
  int result =
      connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  if (result != 0 && errno == EINPROGRESS) {
    result = -1;
    while (Clock::now() < deadline) {
      pollfd item{fd, POLLOUT, 0};
      const int ready = poll(&item, 1, 0);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0)
        break;
      if (ready > 0) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
            error == 0)
          result = 0;
        break;
      }
      const auto until = std::min(deadline, Clock::now() + kPumpStep);
      if (wait) {
        if (!wait(until))
          break;
      } else if (until > Clock::now()) {
        std::this_thread::sleep_until(until);
      }
    }
  }
  ucred credentials{};
  socklen_t credential_size = sizeof(credentials);
  if (result != 0 ||
      getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) !=
          0 ||
      credential_size != sizeof(credentials) ||
      credentials.uid != expected_uid || credentials.pid != descriptor.pid ||
      process_start_time(credentials.pid) != descriptor.start_time) {
    close(fd);
    return {};
  }
  sd_bus *raw = nullptr;
  if (sd_bus_new(&raw) < 0) {
    close(fd);
    return {};
  }
  Bus bus{raw};
  if (sd_bus_set_fd(bus.get(), fd, fd) < 0) {
    close(fd);
    return {};
  }
  // sd_bus owns fd after sd_bus_set_fd succeeds.
  if (sd_bus_set_bus_client(bus.get(), 0) < 0 || sd_bus_start(bus.get()) < 0)
    return {};
  return bus;
}

struct StateReply {
  std::uint32_t outcome = 17;
  std::string helper;
  std::string extension;
  std::string lease;
  std::uint64_t lease_expiry = 0;
  std::uint64_t source_generation = 0;
  std::uint64_t state_generation = 0;
  std::uint8_t role = 255;
  std::string error_name;
};

struct OperationReply {
  std::uint32_t outcome = 17;
  std::string extension;
  std::string digest;
  std::uint64_t source_generation = 0;
  std::uint64_t state_generation = 0;
  std::uint8_t role = 255;
};

struct EditorReply {
  AccessibleObservation observation;
  std::string helper;
};

template <typename Reply> struct PendingCall {
  Reply reply;
  bool complete = false;
  bool valid = false;
  bool selection = false;
};

int state_callback(sd_bus_message *message, void *userdata, sd_bus_error *) {
  auto &call = *static_cast<PendingCall<StateReply> *>(userdata);
  if (sd_bus_message_is_method_error(message, nullptr) > 0) {
    const sd_bus_error *error = sd_bus_message_get_error(message);
    if (error != nullptr && error->name != nullptr)
      call.reply.error_name = error->name;
    call.complete = true;
    return 1;
  }
  const char *helper = nullptr;
  const char *extension = nullptr;
  const char *lease = nullptr;
  std::uint8_t role = 255;
  const int result = sd_bus_message_read(
      message, "usssttty", &call.reply.outcome, &helper, &extension, &lease,
      &call.reply.lease_expiry, &call.reply.source_generation,
      &call.reply.state_generation, &role);
  if (result > 0 && helper != nullptr && extension != nullptr &&
      lease != nullptr && (role == 0 || role == 1 || role == 255)) {
    call.reply.helper = helper;
    call.reply.extension = extension;
    call.reply.lease = lease;
    call.reply.role = role;
    call.valid = true;
  }
  call.complete = true;
#if defined(PUNTO_DESKTOP_LAYOUT_PUMP_TESTING)
  punto_test_pending_call_completed();
#endif
  return 1;
}

int operation_callback(sd_bus_message *message, void *userdata,
                       sd_bus_error *) {
  auto &call = *static_cast<PendingCall<OperationReply> *>(userdata);
  const char *extension = nullptr;
  const char *digest = nullptr;
  std::uint8_t role = 255;
  const int result = sd_bus_message_read(
      message, "usstty", &call.reply.outcome, &extension, &digest,
      &call.reply.source_generation, &call.reply.state_generation, &role);
  if (result > 0 && extension != nullptr && digest != nullptr &&
      (role == 0 || role == 1 || role == 255)) {
    call.reply.extension = extension;
    call.reply.digest = digest;
    call.reply.role = role;
    call.valid = true;
  }
  call.complete = true;
#if defined(PUNTO_DESKTOP_LAYOUT_PUMP_TESTING)
  punto_test_pending_call_completed();
#endif
  return 1;
}

enum class CallStatus {
  Completed,
  TimedOut,
  Cancelled,
  TransportError,
  InvalidReply,
};

int editor_callback(sd_bus_message *message, void *userdata, sd_bus_error *) {
  auto &call = *static_cast<PendingCall<EditorReply> *>(userdata);
  auto &value = call.reply.observation;
  std::uint32_t status = 4;
  const char *helper = nullptr, *registry = nullptr, *provider = nullptr;
  const char *object = nullptr, *sha = nullptr, *token = nullptr;
  const char *identity = "";
  const int read = sd_bus_message_read(
      message, "ustssottuiiiiiss", &status, &helper, &value.bus_epoch,
      &registry, &provider, &object, &value.focus_generation,
      &value.observed_us, &value.provider_pid, &value.caret,
      &value.selection_start, &value.selection_end, &value.range.start,
      &value.range.end, &sha, &token);
  const bool identity_read =
      !call.selection ||
      (read > 0 && sd_bus_message_read(message, "s", &identity) > 0);
  if (read > 0 && identity_read && sd_bus_message_at_end(message, 1) > 0 &&
      status <= 4 && helper && registry && provider && object && sha && token &&
      identity && std::strlen(identity) <= 64 && std::strlen(helper) <= 64 &&
      std::strlen(registry) <= 255 && std::strlen(provider) <= 255 &&
      std::strlen(object) <= 512 && std::strlen(sha) <= 64 &&
      std::strlen(token) <= 64) {
    value.status = static_cast<AccessibleStatus>(status);
    call.reply.helper = helper;
    value.registry_owner = registry;
    value.provider = provider;
    value.object = object;
    value.range.sha256 = sha;
    value.range.identity = identity;
    value.token = token;
    call.valid = true;
  }
  call.complete = true;
  return 1;
}

std::string_view state_call_stage(CallStatus status) {
  switch (status) {
  case CallStatus::Completed:
    return "layout_bridge_state_completed";
  case CallStatus::TimedOut:
    return "layout_bridge_state_timeout";
  case CallStatus::Cancelled:
    return "layout_bridge_state_cancelled";
  case CallStatus::TransportError:
    return "layout_bridge_state_transport";
  case CallStatus::InvalidReply:
    return "layout_bridge_state_invalid_reply";
  }
  return "layout_bridge_state_transport";
}

std::string_view state_outcome_stage(std::uint32_t outcome) {
  switch (outcome) {
  case 10:
    return "layout_bridge_state_unsupported";
  case 11:
    return "layout_bridge_state_stale";
  case 12:
    return "layout_bridge_state_conflict";
  case 13:
    return "layout_bridge_state_expired";
  case 14:
    return "layout_bridge_state_capacity";
  case 15:
    return "layout_bridge_state_locked";
  case 16:
    return "layout_bridge_state_diverged";
  case 18:
    return "layout_bridge_state_not_found";
  case 19:
    return "layout_bridge_state_version";
  case 20:
    return "layout_bridge_state_helper_stale";
  case 21:
    return "layout_bridge_state_deadline";
  case 22:
    return "layout_bridge_state_hop_unknown";
  default:
    return "layout_bridge_state_failed";
  }
}

template <typename Reply>
CallStatus pump_call(sd_bus *bus, PendingCall<Reply> &call, Deadline deadline,
                     const DesktopLayoutClient::WaitFunction &wait) {
#if defined(PUNTO_DESKTOP_LAYOUT_PUMP_TESTING)
  punto_test_pump_started();
#endif
  while (!call.complete && Clock::now() < deadline) {
    for (std::size_t count = 0; count < kMaxPumpMessagesPerSlice &&
                                !call.complete && Clock::now() < deadline;
         ++count) {
      const int processed = sd_bus_process(bus, nullptr);
      if (processed < 0)
        return CallStatus::TransportError;
      if (processed == 0)
        break;
    }
    if (call.complete)
      break;
    const auto until = std::min(deadline, Clock::now() + kPumpStep);
    if (wait) {
      if (!wait(until))
        return CallStatus::Cancelled;
    } else if (until > Clock::now()) {
      std::this_thread::sleep_until(until);
    }
  }
  if (!call.complete)
    return CallStatus::TimedOut;
  return call.valid ? CallStatus::Completed : CallStatus::InvalidReply;
}

std::string digest_request(std::string_view extension,
                           std::uint64_t source_generation,
                           std::uint64_t state_generation, int source,
                           int target) {
  const std::string canonical =
      "version=1\nextension_instance=" + std::string(extension) +
      "\nexpected_source_generation=" + std::to_string(source_generation) +
      "\nexpected_state_generation=" + std::to_string(state_generation) +
      "\nexpected_role=" + std::to_string(source) +
      "\ntarget_role=" + std::to_string(target) + "\n";
  gchar *value = g_compute_checksum_for_data(
      G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(canonical.data()),
      canonical.size());
  if (value == nullptr)
    return {};
  std::string result{value};
  g_free(value);
  return result;
}

} // namespace

struct DesktopLayoutClient::Impl {
  std::string client_id = random_id();
  std::uint64_t sequence = 0;
};

DesktopLayoutClient::DesktopLayoutClient() : impl_(std::make_unique<Impl>()) {}
DesktopLayoutClient::~DesktopLayoutClient() = default;

void DesktopLayoutClient::reset() noexcept {
  impl_->client_id = random_id();
  impl_->sequence = 0;
}

AccessibleObservation DesktopLayoutClient::observe_editor(
    const X11SessionInfo &session, std::uint32_t provider_pid,
    Deadline hard_deadline, const WaitFunction &wait,
    std::string_view expected_token,
    const std::optional<AccessibleRange> &expected_range, AccessibleScope scope,
    bool preparation) {
  const auto started = Clock::now();
  const bool selection = scope == AccessibleScope::Selection;
  if (provider_pid == 0 || started >= hard_deadline ||
      (scope == AccessibleScope::Child && expected_range) ||
      (preparation &&
       (scope != AccessibleScope::Child || !expected_token.empty())) ||
      (selection && expected_range &&
       !lower_hex(expected_range->identity, 64)) ||
      (!expected_token.empty() && !lower_hex(expected_token, 64)))
    return {};
  const auto deadline = std::min(hard_deadline, started + kCallBudget);
  const auto descriptor = parse_descriptor(session);
  if (!descriptor)
    return {};
  auto bus = connect_bus(*descriptor, session.uid, deadline, wait);
  if (!bus)
    return {};
  Message message;
  sd_bus_message *raw_message = nullptr;
  if (sd_bus_message_new_method_call(
          bus.get(), &raw_message, nullptr, kPath, kInterface,
          preparation ? "PrepareEditorOrigin"
                      : (scope == AccessibleScope::Child ? "ObserveEditorOrigin"
                         : selection                     ? "ObserveSelection"
                                     : "ObserveEditor")) < 0)
    return {};
  message.reset(raw_message);
  const AccessibleRange range = expected_range.value_or(AccessibleRange{});
  const std::string token(expected_token);
  const int appended =
      scope == AccessibleScope::Child
          ? sd_bus_message_append(
                message.get(), "suts", descriptor->helper_instance.c_str(),
                provider_pid, monotonic_us(deadline), token.c_str())
          : sd_bus_message_append(
                message.get(), "sutsiis", descriptor->helper_instance.c_str(),
                provider_pid, monotonic_us(deadline), token.c_str(),
                range.start, range.end, range.sha256.c_str());
  if (appended < 0)
    return {};
  if (selection &&
      sd_bus_message_append(message.get(), "s", range.identity.c_str()) < 0)
    return {};
  PendingCall<EditorReply> call;
  call.selection = selection;
  sd_bus_slot *raw_slot = nullptr;
  if (sd_bus_call_async(bus.get(), &raw_slot, message.get(), editor_callback,
                        &call, timeout_us(deadline)) < 0)
    return {};
  Slot slot{raw_slot};
  const auto status = pump_call(bus.get(), call, deadline, wait);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  if (status != CallStatus::Completed)
    g_printerr("[punto-e2e] editor_transport status=%u elapsed_us=%lld\n",
               static_cast<unsigned>(status),
               static_cast<long long>(
                   std::chrono::duration_cast<std::chrono::microseconds>(
                       Clock::now() - started)
                       .count()));
#endif
  if (status != CallStatus::Completed ||
      call.reply.helper != descriptor->helper_instance ||
      Clock::now() >= deadline)
    return {};
  auto &value = call.reply.observation;
  if (value.status != AccessibleStatus::Observed)
    return value;
  const bool range_valid =
      (value.range.start == -1 && value.range.end == -1 &&
       value.range.sha256.empty()) ||
      (value.range.start >= 0 && value.range.end >= value.range.start &&
       value.range.end - value.range.start <= kAccessibleRangeCharacters &&
       lower_hex(value.range.sha256, 64));
  if (value.provider_pid != provider_pid || value.bus_epoch == 0 ||
      value.focus_generation == 0 ||
      value.observed_us < monotonic_us(started) ||
      value.observed_us >= monotonic_us(deadline) || !range_valid ||
      value.registry_owner.empty() || value.registry_owner.front() != ':' ||
      value.provider.empty() || value.provider.front() != ':' ||
      value.object == "/" || !lower_hex(value.token, 64) ||
      (selection && !lower_hex(value.range.identity, 64)) ||
      (scope == AccessibleScope::Child &&
       (value.caret != -1 || value.selection_start != -1 ||
        value.selection_end != -1 || value.range.start != -1 ||
        value.range.end != -1 || !value.range.sha256.empty())) ||
      (!expected_token.empty() && expected_token != value.token) ||
      (expected_range &&
       (expected_range->start != value.range.start ||
        expected_range->end != value.range.end ||
        expected_range->sha256 != value.range.sha256 ||
        (selection && expected_range->identity != value.range.identity))))
    return {};
  return std::move(value);
}

DesktopLayoutResult
DesktopLayoutClient::switch_to(const X11SessionInfo &session, int source_layout,
                               int target_layout, Deadline hard_deadline,
                               const WaitFunction &wait) {
  DesktopLayoutResult result;
  result.observed_layout = source_layout;
  if ((source_layout != 0 && source_layout != 1) ||
      (target_layout != 0 && target_layout != 1) || impl_->client_id.empty() ||
      Clock::now() >= hard_deadline)
    return result;
#if defined(PUNTO_LAYOUT_SWITCH_E2E_TESTING)
  const int test_result =
      punto_e2e_desktop_layout_switch(source_layout, target_layout);
  if (test_result == 1) {
    result.status = DesktopLayoutStatus::Applied;
    result.observed_layout = target_layout;
    result.stage = "layout_bridge_applied";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_after_layout_applied(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            hard_deadline.time_since_epoch())
            .count());
#endif
  } else if (test_result == 2) {
    result.status = DesktopLayoutStatus::Verified;
    result.observed_layout = target_layout;
    result.stage = "layout_bridge_verified";
  } else if (test_result == 3) {
    result.status = DesktopLayoutStatus::RequestedUnresolved;
    result.stage = "layout_bridge_switch_unresolved";
  } else {
    result.stage = "layout_bridge_test_rejected";
  }
  return result;
#endif
  result.stage = "layout_bridge_descriptor";
  auto descriptor = parse_descriptor(session);
  if (!descriptor)
    return result;
  const auto connect_deadline =
      std::min(hard_deadline, Clock::now() + kCallBudget);
  result.stage = "layout_bridge_connect";
  Bus bus = connect_bus(*descriptor, session.uid, connect_deadline, wait);
  if (!bus)
    return result;

  auto new_call = [&](const char *method, Message &message) {
    sd_bus_message *raw = nullptr;
    const int call_result = sd_bus_message_new_method_call(
        bus.get(), &raw, nullptr, kPath, kInterface, method);
    message.reset(raw);
    return call_result >= 0;
  };

  const auto call_state = [&](Deadline deadline,
                              StateReply &reply) -> CallStatus {
    Message message;
    if (!new_call("GetState", message) ||
        sd_bus_message_append(message.get(), "st", impl_->client_id.c_str(),
                              monotonic_us(deadline)) < 0)
      return CallStatus::TransportError;
    PendingCall<StateReply> pending;
    sd_bus_slot *raw_slot = nullptr;
    if (sd_bus_call_async(bus.get(), &raw_slot, message.get(), state_callback,
                          &pending, timeout_us(deadline)) < 0)
      return CallStatus::TransportError;
    Slot slot{raw_slot};
    const auto status = pump_call(bus.get(), pending, deadline, wait);
    reply = std::move(pending.reply);
    return status;
  };

  StateReply state;
  Deadline call_deadline = std::min(hard_deadline, Clock::now() + kCallBudget);
  CallStatus state_status = call_state(call_deadline, state);
  while (state_status == CallStatus::TimedOut && Clock::now() < hard_deadline) {
    bus.reset();
    descriptor = parse_descriptor(session);
    call_deadline = std::min(hard_deadline, Clock::now() + kCallBudget);
    if (!descriptor) {
      state_status = CallStatus::TransportError;
      break;
    }
    bus = connect_bus(*descriptor, session.uid, call_deadline, wait);
    if (!bus) {
      state_status = CallStatus::TransportError;
      break;
    }
    state = {};
    state_status = call_state(call_deadline, state);
  }
  if (state_status != CallStatus::Completed) {
    if (state_status == CallStatus::InvalidReply && !state.error_name.empty()) {
      g_warning("[punto] Desktop layout GetState method error: %s",
                state.error_name.c_str());
      result.stage = "layout_bridge_state_method_error";
    } else {
      result.stage = state_call_stage(state_status);
    }
    return result;
  }
  if (state.outcome != kState) {
    result.stage = state_outcome_stage(state.outcome);
    return result;
  }
  if (!descriptor) {
    result.stage = "layout_bridge_descriptor";
    return result;
  }
  if (state.helper != descriptor->helper_instance) {
    result.stage = "layout_bridge_state_helper_identity";
    return result;
  }
  if (!lower_hex(state.extension, 32)) {
    result.stage = "layout_bridge_state_extension_identity";
    return result;
  }
  if (!lower_hex(state.lease, 32)) {
    result.stage = "layout_bridge_state_lease_identity";
    return result;
  }
  if (state.role != source_layout) {
    result.stage = "layout_bridge_state_role_mismatch";
    return result;
  }
  if (state.lease_expiry <= monotonic_us()) {
    result.stage = "layout_bridge_state_lease_expired";
    return result;
  }
  if (source_layout == target_layout) {
    result.status = DesktopLayoutStatus::Verified;
    result.observed_layout = target_layout;
    result.stage = "layout_bridge_verified";
    return result;
  }

  if (impl_->sequence == UINT64_MAX) {
    reset();
    result.stage = "layout_bridge_sequence";
    return result;
  }
  const std::uint64_t sequence = ++impl_->sequence;
  const std::string digest =
      digest_request(state.extension, state.source_generation,
                     state.state_generation, source_layout, target_layout);
  if (!lower_hex(digest, 64)) {
    result.stage = "layout_bridge_digest";
    return result;
  }
  const Deadline original_deadline =
      std::min(hard_deadline, Clock::now() + kCallBudget);
  Message message;
  if (!new_call("Switch", message) ||
      sd_bus_message_append(
          message.get(), "sssststttyy", descriptor->helper_instance.c_str(),
          state.extension.c_str(), impl_->client_id.c_str(),
          state.lease.c_str(), sequence, digest.c_str(),
          monotonic_us(original_deadline), state.source_generation,
          state.state_generation, source_layout, target_layout) < 0) {
    result.stage = "layout_bridge_switch_prepare";
    return result;
  }
  PendingCall<OperationReply> pending;
  sd_bus_slot *raw_slot = nullptr;
  if (sd_bus_call_async(bus.get(), &raw_slot, message.get(), operation_callback,
                        &pending, timeout_us(original_deadline)) < 0) {
    result.stage = "layout_bridge_switch_send";
    return result;
  }
  Slot slot{raw_slot};
  result.status = DesktopLayoutStatus::RequestedUnresolved;
  result.stage = "layout_bridge_switch_unresolved";
  const auto switch_status =
      pump_call(bus.get(), pending, original_deadline, wait);
  if (switch_status == CallStatus::Cancelled)
    return result;
  bool terminal = switch_status == CallStatus::Completed;
  OperationReply operation = std::move(pending.reply);
  slot.reset();
  const auto valid_identity = [&] {
    return operation.extension == state.extension && operation.digest == digest;
  };
  terminal = terminal && valid_identity() && operation.outcome != kHopUnknown &&
             operation.outcome != kNotFound;

  bool reconnect = switch_status == CallStatus::TransportError;
  while (!terminal && Clock::now() < hard_deadline) {
    const Deadline query_deadline =
        std::min(hard_deadline, Clock::now() + kCallBudget);
    if (reconnect || !bus) {
      bus.reset();
      descriptor = parse_descriptor(session);
      if (descriptor)
        bus = connect_bus(*descriptor, session.uid, query_deadline, wait);
      if (!bus) {
        if (wait) {
          if (!wait(std::min(hard_deadline, Clock::now() + kPumpStep)))
            return result;
        } else {
          std::this_thread::sleep_for(kPumpStep);
        }
        reconnect = true;
        continue;
      }
      reconnect = false;
    }
    Message query;
    if (!new_call("GetOperation", query) ||
        sd_bus_message_append(query.get(), "sssstst",
                              descriptor->helper_instance.c_str(),
                              state.extension.c_str(), impl_->client_id.c_str(),
                              state.lease.c_str(), sequence, digest.c_str(),
                              monotonic_us(query_deadline)) < 0) {
      reconnect = true;
      continue;
    }
    PendingCall<OperationReply> recovery;
    raw_slot = nullptr;
    if (sd_bus_call_async(bus.get(), &raw_slot, query.get(), operation_callback,
                          &recovery, timeout_us(query_deadline)) < 0) {
      reconnect = true;
      continue;
    }
    Slot recovery_slot{raw_slot};
    const auto recovery_status =
        pump_call(bus.get(), recovery, query_deadline, wait);
    if (recovery_status == CallStatus::Cancelled)
      return result;
    if (recovery_status != CallStatus::Completed) {
      reconnect = recovery_status == CallStatus::TransportError;
      continue;
    }
    operation = std::move(recovery.reply);
    if (!valid_identity())
      return result;
    if (operation.outcome == kHopUnknown || operation.outcome == kNotFound) {
      if (wait) {
        if (!wait(std::min(hard_deadline, Clock::now() + kPumpStep)))
          return result;
      } else {
        std::this_thread::sleep_for(kPumpStep);
      }
      continue;
    }
    terminal = true;
  }
  if (!terminal || operation.outcome != kApplied ||
      operation.role != target_layout) {
    result.stage = terminal ? "layout_bridge_switch_rejected"
                            : "layout_bridge_switch_unresolved";
    if (terminal)
      result.status = DesktopLayoutStatus::Rejected;
    return result;
  }

  StateReply fresh;
  call_deadline = std::min(hard_deadline, Clock::now() + kCallBudget);
  const auto fresh_budget_us = timeout_us(call_deadline);
  const auto fresh_status = call_state(call_deadline, fresh);
  if (fresh_status != CallStatus::Completed) {
    switch (fresh_status) {
    case CallStatus::TimedOut:
      result.stage = "layout_bridge_fresh_state_timeout";
      break;
    case CallStatus::Cancelled:
      result.stage = "layout_bridge_fresh_state_cancelled";
      break;
    case CallStatus::TransportError:
      result.stage = "layout_bridge_fresh_state_transport";
      break;
    case CallStatus::InvalidReply:
      result.stage = "layout_bridge_fresh_state_invalid_reply";
      break;
    case CallStatus::Completed:
      break;
    }
    g_warning("[punto] Desktop layout fresh state call failed stage=%s "
              "budget_us=%" G_GUINT64_FORMAT,
              result.stage.data(), fresh_budget_us);
    return result;
  }
  if (fresh.outcome != kState) {
    result.stage = "layout_bridge_fresh_state_outcome";
    return result;
  }
  if (fresh.helper != descriptor->helper_instance) {
    result.stage = "layout_bridge_fresh_state_helper";
    return result;
  }
  if (fresh.extension != operation.extension) {
    result.stage = "layout_bridge_fresh_state_extension";
    return result;
  }
  if (fresh.role != target_layout) {
    result.stage = "layout_bridge_fresh_state_role";
    return result;
  }
  if (fresh.source_generation != operation.source_generation) {
    g_warning("[punto] Desktop layout fresh source generation mismatch "
              "operation=%" G_GUINT64_FORMAT " fresh=%" G_GUINT64_FORMAT,
              operation.source_generation, fresh.source_generation);
    result.stage = "layout_bridge_fresh_state_source_generation";
    return result;
  }
  if (fresh.state_generation != operation.state_generation) {
    g_warning("[punto] Desktop layout fresh state generation mismatch "
              "operation=%" G_GUINT64_FORMAT " fresh=%" G_GUINT64_FORMAT,
              operation.state_generation, fresh.state_generation);
    result.stage = "layout_bridge_fresh_state_state_generation";
    return result;
  }
  result.status = DesktopLayoutStatus::Applied;
  result.observed_layout = target_layout;
  result.stage = "layout_bridge_applied";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  punto_e2e_after_layout_applied(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          hard_deadline.time_since_epoch())
          .count());
#endif
  return result;
}

} // namespace punto
