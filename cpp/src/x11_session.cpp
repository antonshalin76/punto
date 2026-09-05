/**
 * @file x11_session.cpp
 * @brief Safe systemd-logind discovery and XCB-backed X11 access.
 */

#include "punto/x11_session.hpp"

#include <X11/Xauth.h>
#include <systemd/sd-login.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
// Debian's generated xcb-xkb C header contains a field named `explicit`.
#define explicit explicit_value
#include <xcb/xcbext.h>
#include <xcb/xkb.h>
#undef explicit
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <pwd.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace punto {

struct X11Session::WriteGate {
  mutable std::recursive_mutex mutex;
  bool enabled = false;
  std::uint64_t generation = 0;
};

namespace {

using OwnedCString = std::unique_ptr<char, decltype(&std::free)>;
constexpr auto kXcbSocketSendTimeout = std::chrono::milliseconds{100};

struct DirectoryCloser {
  void operator()(DIR *directory) const noexcept {
    if (directory != nullptr) {
      (void)::closedir(directory);
    }
  }
};

struct FileCloser {
  void operator()(FILE *file) const noexcept {
    if (file != nullptr) {
      (void)::fclose(file);
    }
  }
};

[[nodiscard]] int
bounded_poll_timeout(std::chrono::steady_clock::time_point deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  if (remaining.count() >= std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return std::max(static_cast<int>(remaining.count()) + 1, 1);
}

class OwnedDescriptor final {
public:
  explicit OwnedDescriptor(int descriptor = -1) noexcept
      : descriptor_{descriptor} {}
  ~OwnedDescriptor() { reset(); }

  OwnedDescriptor(const OwnedDescriptor &) = delete;
  OwnedDescriptor &operator=(const OwnedDescriptor &) = delete;
  OwnedDescriptor(OwnedDescriptor &&other) noexcept
      : descriptor_{std::exchange(other.descriptor_, -1)} {}
  OwnedDescriptor &operator=(OwnedDescriptor &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.descriptor_, -1));
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return descriptor_ >= 0;
  }
  [[nodiscard]] int release() noexcept {
    return std::exchange(descriptor_, -1);
  }
  void reset(int descriptor = -1) noexcept {
    if (descriptor_ >= 0) {
      (void)::close(descriptor_);
    }
    descriptor_ = descriptor;
  }

private:
  int descriptor_ = -1;
};

[[nodiscard]] bool is_decimal(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return character >= '0' && character <= '9';
         });
}

[[nodiscard]] bool is_greeter(std::string_view username) noexcept {
  return username == "gdm" || username == "lightdm" || username == "sddm";
}

[[nodiscard]] bool path_is_beneath(std::string_view parent,
                                   std::string_view child) noexcept {
  if (parent.empty() || child.size() <= parent.size() ||
      child.compare(0, parent.size(), parent) != 0 ||
      child[parent.size()] != '/') {
    return false;
  }
  if (child.find("/../") != std::string_view::npos || child.ends_with("/..") ||
      child.find("/./") != std::string_view::npos || child.ends_with("/.")) {
    return false;
  }
  return true;
}

[[nodiscard]] int open_valid_xauthority(const X11SessionInfo &info) noexcept {
  if (info.xauthority_path.empty() || info.xauthority_path.front() != '/' ||
      info.xauthority_path.find('\0') != std::string::npos) {
    return -1;
  }

  int descriptor = -1;
  do {
    descriptor = ::open(info.xauthority_path.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return -1;
  }

  struct stat metadata {};
  if (::fstat(descriptor, &metadata) != 0 ||
      !x11_detail::xauthority_metadata_is_trusted(
          x11_detail::XauthorityMetadata{
              static_cast<std::uint32_t>(metadata.st_uid),
              static_cast<std::uint32_t>(metadata.st_mode),
              static_cast<std::int64_t>(metadata.st_size)},
          info.uid)) {
    (void)::close(descriptor);
    return -1;
  }
  return descriptor;
}

[[nodiscard]] bool validate_runtime_directory(const X11SessionInfo &info) {
  const std::string expected = "/run/user/" + std::to_string(info.uid);
  if (info.xdg_runtime_dir != expected) {
    return false;
  }

  int descriptor = -1;
  do {
    descriptor =
        ::open(expected.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return false;
  }

  struct stat metadata {};
  const bool valid = ::fstat(descriptor, &metadata) == 0 &&
                     S_ISDIR(metadata.st_mode) &&
                     metadata.st_uid == static_cast<uid_t>(info.uid) &&
                     (metadata.st_mode & 0022) == 0;
  (void)::close(descriptor);
  return valid;
}

struct LocalDisplayEndpoint {
  std::string number;
  int screen_number = -1;
};

[[nodiscard]] std::optional<LocalDisplayEndpoint>
parse_local_display(std::string_view display) {
  if (!x11_detail::is_valid_local_display(display)) {
    return std::nullopt;
  }
  char *host = nullptr;
  int display_number = -1;
  int screen_number = -1;
  const std::string terminated{display};
  const int parsed = ::xcb_parse_display(terminated.c_str(), &host,
                                         &display_number, &screen_number);
  OwnedCString owned_host{host, &std::free};
  if (parsed == 0 || display_number < 0 || screen_number < 0 ||
      (owned_host && owned_host.get()[0] != '\0')) {
    return std::nullopt;
  }
  return LocalDisplayEndpoint{std::to_string(display_number), screen_number};
}

struct XcbAuthorization {
  std::vector<char> name;
  std::vector<char> data;

  [[nodiscard]] bool present() const noexcept { return !name.empty(); }
};

[[nodiscard]] std::optional<XcbAuthorization>
read_xauthority(int descriptor, std::string_view display_number) noexcept {
  OwnedDescriptor source{descriptor};
  struct stat metadata {};
  if (!source || ::fstat(source.get(), &metadata) != 0 ||
      !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) >
          x11_detail::kMaxXauthorityBytes) {
    return std::nullopt;
  }
  const std::size_t snapshot_size = static_cast<std::size_t>(metadata.st_size);
  if (snapshot_size == 0) {
    return XcbAuthorization{};
  }
  std::vector<char> snapshot(snapshot_size);
  std::size_t offset = 0;
  while (offset < snapshot.size()) {
    const ssize_t count = ::read(source.get(), snapshot.data() + offset,
                                 snapshot.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return std::nullopt;
  }
  source.reset();

  FILE *raw_file = ::fmemopen(snapshot.data(), snapshot.size(), "rb");
  if (raw_file == nullptr) {
    return std::nullopt;
  }
  std::unique_ptr<FILE, FileCloser> file{raw_file};
  XcbAuthorization best;
  int best_priority = -1;
  try {
    std::array<char, HOST_NAME_MAX + 1U> hostname_buffer{};
    if (::gethostname(hostname_buffer.data(), hostname_buffer.size() - 1U) !=
        0) {
      return std::nullopt;
    }
    const std::string_view hostname{hostname_buffer.data()};
    const std::string_view short_hostname =
        hostname.substr(0, hostname.find('.'));
    const auto local_address_matches =
        [hostname, short_hostname](const Xauth &auth) noexcept {
          const std::string_view address{auth.address, auth.address_length};
          return address == hostname || address == short_hostname;
        };
    while (true) {
      const long record_offset = std::ftell(file.get());
      if (record_offset < 0 ||
          static_cast<std::size_t>(record_offset) > snapshot.size()) {
        return std::nullopt;
      }
      if (static_cast<std::size_t>(record_offset) == snapshot.size()) {
        break;
      }
      Xauth *raw_auth = ::XauReadAuth(file.get());
      if (raw_auth == nullptr) {
        return std::nullopt;
      }
      std::unique_ptr<Xauth, decltype(&::XauDisposeAuth)> auth{
          raw_auth, &::XauDisposeAuth};
      constexpr std::string_view kMitCookie{"MIT-MAGIC-COOKIE-1"};
      int number_priority = -1;
      if (auth->number_length == display_number.size() &&
          (display_number.empty() ||
           std::memcmp(auth->number, display_number.data(),
                       display_number.size()) == 0)) {
        number_priority = 1;
      } else if (auth->number_length == 0) {
        number_priority = 0;
      }
      const bool protocol_matches =
          auth->name_length == kMitCookie.size() &&
          std::memcmp(auth->name, kMitCookie.data(), kMitCookie.size()) == 0 &&
          auth->data_length == 16U;
      int family_priority = -1;
      if (auth->family == FamilyLocal && local_address_matches(*auth)) {
        family_priority = 2;
      } else if (auth->family == FamilyLocalHost &&
                 (auth->address_length == 0 || local_address_matches(*auth))) {
        family_priority = 1;
      } else if (auth->family == FamilyWild) {
        family_priority = 0;
      }
      if (number_priority < 0 || !protocol_matches || family_priority < 0) {
        continue;
      }
      const int priority = number_priority * 3 + family_priority;
      if (priority <= best_priority) {
        continue;
      }
      best.name.assign(auth->name, auth->name + auth->name_length);
      best.data.assign(auth->data, auth->data + auth->data_length);
      best_priority = priority;
    }
  } catch (...) {
    return std::nullopt;
  }
  if (::ferror(file.get()) != 0) {
    return std::nullopt;
  }
  return best;
}

[[nodiscard]] int connect_local_x_socket(
    const X11SessionInfo &info, const LocalDisplayEndpoint &endpoint,
    std::chrono::steady_clock::time_point deadline) noexcept {
  OwnedDescriptor directory;
  int directory_fd = -1;
  do {
    directory_fd =
        ::open("/tmp/.X11-unix", O_PATH | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
  } while (directory_fd < 0 && errno == EINTR);
  directory.reset(directory_fd);
  struct stat directory_metadata {};
  if (!directory || ::fstat(directory.get(), &directory_metadata) != 0 ||
      !S_ISDIR(directory_metadata.st_mode) || directory_metadata.st_uid != 0 ||
      ((directory_metadata.st_mode & 0022) != 0 &&
       (directory_metadata.st_mode & S_ISVTX) == 0)) {
    return -1;
  }

  const std::string socket_name = "X" + endpoint.number;
  struct stat socket_metadata {};
  if (::fstatat(directory.get(), socket_name.c_str(), &socket_metadata,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(socket_metadata.st_mode) ||
      (socket_metadata.st_uid != 0 &&
       socket_metadata.st_uid != static_cast<uid_t>(info.uid))) {
    return -1;
  }

  OwnedDescriptor socket{
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
  if (!socket) {
    return -1;
  }
  const std::string pinned_path =
      "/proc/self/fd/" + std::to_string(directory.get()) + "/" + socket_name;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (pinned_path.size() >= sizeof(address.sun_path)) {
    return -1;
  }
  std::memcpy(address.sun_path, pinned_path.c_str(), pinned_path.size() + 1U);

  int connect_result = -1;
  do {
    connect_result =
        ::connect(socket.get(), reinterpret_cast<const sockaddr *>(&address),
                  static_cast<socklen_t>(sizeof(address)));
  } while (connect_result < 0 && errno == EINTR &&
           std::chrono::steady_clock::now() < deadline);
  if (connect_result < 0) {
    if (errno != EINPROGRESS || std::chrono::steady_clock::now() >= deadline) {
      return -1;
    }
    pollfd descriptor{socket.get(), POLLOUT, 0};
    const int poll_result =
        ::poll(&descriptor, 1, bounded_poll_timeout(deadline));
    if (poll_result <= 0 ||
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      return -1;
    }
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                     &error_size) != 0 ||
        socket_error != 0) {
      return -1;
    }
  }

  struct PeerCredentials {
    pid_t pid;
    uid_t uid;
    gid_t gid;
  } peer{};
  socklen_t peer_size = sizeof(peer);
  struct stat current_socket_metadata {};
  if (::getsockopt(socket.get(), SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) !=
          0 ||
      peer_size != sizeof(peer) ||
      (peer.uid != 0 && peer.uid != static_cast<uid_t>(info.uid)) ||
      ::fstatat(directory.get(), socket_name.c_str(), &current_socket_metadata,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      current_socket_metadata.st_dev != socket_metadata.st_dev ||
      current_socket_metadata.st_ino != socket_metadata.st_ino) {
    return -1;
  }

  const int flags = ::fcntl(socket.get(), F_GETFL);
  if (flags < 0 || ::fcntl(socket.get(), F_SETFL, flags & ~O_NONBLOCK) != 0) {
    return -1;
  }
  return socket.release();
}

[[nodiscard]] bool screen_and_extensions_are_ready(xcb_connection_t *connection,
                                                   int screen_number) noexcept {
  if (connection == nullptr || ::xcb_connection_has_error(connection) != 0) {
    return false;
  }
  const xcb_setup_t *setup = ::xcb_get_setup(connection);
  if (setup == nullptr) {
    return false;
  }
  xcb_screen_iterator_t screen = ::xcb_setup_roots_iterator(setup);
  for (int current = 0; current < screen_number && screen.rem > 0; ++current) {
    ::xcb_screen_next(&screen);
  }
  if (screen.rem == 0) {
    return false;
  }
  const xcb_query_extension_reply_t *xkb =
      ::xcb_get_extension_data(connection, &xcb_xkb_id);
  return xkb != nullptr && xkb->present != 0 &&
         ::xcb_connection_has_error(connection) == 0;
}

std::timed_mutex &x_connection_admission_mutex() {
  static std::timed_mutex mutex;
  return mutex;
}

[[nodiscard]] xcb_connection_t *connect_xcb_from_snapshot(
    const X11SessionInfo &info, int *screen_number,
    std::chrono::steady_clock::time_point deadline) noexcept {
  struct HandshakeState {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    bool abandoned = false;
    xcb_connection_t *connection = nullptr;
    std::chrono::steady_clock::time_point completed_at{};
  } state;

  try {
    const auto endpoint = parse_local_display(info.display);
    if (!endpoint) {
      return nullptr;
    }
    std::unique_lock<std::timed_mutex> admission{x_connection_admission_mutex(),
                                                 std::defer_lock};
    if (!admission.try_lock_until(deadline)) {
      return nullptr;
    }

    OwnedDescriptor authority{open_valid_xauthority(info)};
    if (!authority) {
      return nullptr;
    }
    auto authorization = read_xauthority(authority.release(), endpoint->number);
    if (!authorization || std::chrono::steady_clock::now() >= deadline) {
      return nullptr;
    }

    OwnedDescriptor socket{connect_local_x_socket(info, *endpoint, deadline)};
    if (!socket || std::chrono::steady_clock::now() >= deadline) {
      return nullptr;
    }
    OwnedDescriptor cancel_socket;
    int duplicate = -1;
    do {
      duplicate = ::fcntl(socket.get(), F_DUPFD_CLOEXEC, 0);
    } while (duplicate < 0 && errno == EINTR);
    cancel_socket.reset(duplicate);
    if (!cancel_socket) {
      return nullptr;
    }

    const int xcb_socket = socket.get();
    std::thread handshake{[xcb_socket, endpoint = *endpoint,
                           authorization = std::move(*authorization),
                           &state]() mutable {
      xcb_auth_info_t auth_info{};
      xcb_auth_info_t *auth = nullptr;
      if (authorization.present()) {
        auth_info.namelen = static_cast<int>(authorization.name.size());
        auth_info.name = authorization.name.data();
        auth_info.datalen = static_cast<int>(authorization.data.size());
        auth_info.data = authorization.data.data();
        auth = &auth_info;
      }
      xcb_connection_t *connection = ::xcb_connect_to_fd(xcb_socket, auth);
      if (!screen_and_extensions_are_ready(connection,
                                           endpoint.screen_number)) {
        ::xcb_disconnect(connection);
        connection = nullptr;
      }

      bool abandoned = false;
      {
        std::lock_guard<std::mutex> lock{state.mutex};
        abandoned = state.abandoned;
        if (!abandoned) {
          state.connection = connection;
        }
        state.completed_at = std::chrono::steady_clock::now();
        state.done = true;
      }
      if (abandoned && connection != nullptr) {
        ::xcb_disconnect(connection);
      }
      state.condition.notify_all();
    }};
    (void)socket.release();

    bool completed_in_time = false;
    {
      std::unique_lock<std::mutex> lock{state.mutex};
      const bool completed = state.condition.wait_until(
          lock, deadline, [&state] { return state.done; });
      completed_in_time = completed && state.completed_at <= deadline;
      if (!completed_in_time) {
        state.abandoned = true;
      }
    }
    if (!completed_in_time) {
      (void)::shutdown(cancel_socket.get(), SHUT_RDWR);
    }
    handshake.join();
    if (!completed_in_time) {
      xcb_connection_t *late_connection = nullptr;
      {
        std::lock_guard<std::mutex> lock{state.mutex};
        late_connection = state.connection;
        state.connection = nullptr;
      }
      if (late_connection != nullptr) {
        ::xcb_disconnect(late_connection);
      }
      return nullptr;
    }

    xcb_connection_t *connection = nullptr;
    {
      std::lock_guard<std::mutex> lock{state.mutex};
      connection = state.connection;
      state.connection = nullptr;
    }
    if (connection != nullptr && screen_number != nullptr) {
      *screen_number = endpoint->screen_number;
    }
    return connection;
  } catch (...) {
    return nullptr;
  }
}

[[nodiscard]] bool
enable_xkb(BoundedXcbConnection &connection,
           std::chrono::steady_clock::time_point deadline) noexcept {
  if (!connection.is_open()) {
    return false;
  }
  const xcb_xkb_use_extension_cookie_t cookie = ::xcb_xkb_use_extension(
      connection.get(), XCB_XKB_MAJOR_VERSION, XCB_XKB_MINOR_VERSION);
  x11_detail::XcbOperationResult result{};
  auto *reply = static_cast<xcb_xkb_use_extension_reply_t *>(
      connection.wait_for_reply(cookie.sequence, deadline, result));
  const bool supported = result == x11_detail::XcbOperationResult::Success &&
                         reply != nullptr && reply->supported != 0 &&
                         connection.is_open();
  std::free(reply);
  return supported;
}

[[nodiscard]] int
get_xkb_group(BoundedXcbConnection &connection,
              std::chrono::steady_clock::time_point deadline,
              std::uint8_t *locked_mods = nullptr) noexcept {
  if (!enable_xkb(connection, deadline)) {
    return -1;
  }
  const auto cookie =
      ::xcb_xkb_get_state(connection.get(), XCB_XKB_ID_USE_CORE_KBD);
  x11_detail::XcbOperationResult result{};
  auto *reply = static_cast<xcb_xkb_get_state_reply_t *>(
      connection.wait_for_reply(cookie.sequence, deadline, result));
  const int group = result == x11_detail::XcbOperationResult::Success &&
                            reply != nullptr && connection.is_open()
                        ? static_cast<int>(reply->group)
                        : -1;
  if (group >= 0 && locked_mods != nullptr) {
    *locked_mods = reply->lockedMods;
  }
  std::free(reply);
  return group;
}

using Environment = std::unordered_map<std::string, std::string>;

[[nodiscard]] std::optional<std::vector<std::uint32_t>>
read_proc_groups(pid_t pid, uid_t expected_uid, std::uint32_t primary_gid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  const std::string path = "/proc/" + std::to_string(pid) + "/status";
  int descriptor = -1;
  do {
    descriptor =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return std::nullopt;
  }
  struct stat metadata {};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != expected_uid) {
    (void)::close(descriptor);
    return std::nullopt;
  }

  constexpr std::size_t maximum_status_bytes = std::size_t{64} * 1024U;
  std::string status;
  std::array<char, 4096> chunk{};
  while (status.size() <= maximum_status_bytes) {
    const ssize_t count = ::read(descriptor, chunk.data(), chunk.size());
    if (count > 0) {
      status.append(chunk.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      (void)::close(descriptor);
      return std::nullopt;
    }
    break;
  }
  (void)::close(descriptor);
  if (status.size() > maximum_status_bytes) {
    return std::nullopt;
  }

  const std::string_view bytes{status};
  std::size_t begin = bytes.find("Groups:");
  if (begin == std::string_view::npos ||
      (begin != 0 && bytes[begin - 1] != '\n')) {
    return std::nullopt;
  }
  begin += std::string_view{"Groups:"}.size();
  const std::size_t end = bytes.find('\n', begin);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }

  std::vector<std::uint32_t> groups;
  while (begin < end) {
    while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t')) {
      ++begin;
    }
    if (begin == end) {
      break;
    }
    const std::size_t token_end = bytes.find_first_of(" \t", begin);
    const std::size_t bounded_end = std::min(token_end, end);
    std::uint64_t parsed = 0;
    const auto [ptr, error] = std::from_chars(
        bytes.data() + begin, bytes.data() + bounded_end, parsed);
    if (error != std::errc{} || ptr != bytes.data() + bounded_end ||
        parsed > std::numeric_limits<std::uint32_t>::max() ||
        groups.size() >= 1024U) {
      return std::nullopt;
    }
    groups.push_back(static_cast<std::uint32_t>(parsed));
    begin = bounded_end;
  }
  groups.push_back(primary_gid);
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  return groups;
}

[[nodiscard]] std::optional<Environment>
read_proc_environment(pid_t pid, uid_t expected_uid) {
  if (pid <= 0) {
    return std::nullopt;
  }
  const std::string path = "/proc/" + std::to_string(pid) + "/environ";
  int descriptor = -1;
  do {
    descriptor =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return std::nullopt;
  }

  struct stat metadata {};
  if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != expected_uid) {
    (void)::close(descriptor);
    return std::nullopt;
  }

  std::string bytes;
  bytes.reserve(4096);
  std::array<char, 4096> chunk{};
  while (bytes.size() <= x11_detail::kMaxEnvironmentBytes) {
    const ssize_t count = ::read(descriptor, chunk.data(), chunk.size());
    if (count > 0) {
      bytes.append(chunk.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      (void)::close(descriptor);
      return std::nullopt;
    }
    break;
  }
  (void)::close(descriptor);
  if (bytes.size() > x11_detail::kMaxEnvironmentBytes ||
      (!bytes.empty() && bytes.back() != '\0')) {
    return std::nullopt;
  }

  Environment environment;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t end = bytes.find('\0', offset);
    if (end == std::string::npos) {
      return std::nullopt;
    }
    const std::string_view entry =
        std::string_view{bytes}.substr(offset, end - offset);
    const std::size_t equals = entry.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      return std::nullopt;
    }
    std::string key{entry.substr(0, equals)};
    std::string value{entry.substr(equals + 1)};
    if (!environment.emplace(std::move(key), std::move(value)).second) {
      return std::nullopt;
    }
    offset = end + 1;
  }
  return environment;
}

void apply_environment(const Environment &environment, X11SessionInfo &info) {
  const auto assign = [&environment](std::string_view key,
                                     std::string &destination) {
    const auto found = environment.find(std::string{key});
    if (found != environment.end()) {
      destination = found->second;
    }
  };
  assign("DISPLAY", info.display);
  assign("XAUTHORITY", info.xauthority_path);
  assign("XDG_RUNTIME_DIR", info.xdg_runtime_dir);
  assign("XDG_CONFIG_HOME", info.xdg_config_home);
  assign("WAYLAND_DISPLAY", info.wayland_display);
}

[[nodiscard]] bool validate_base_candidate(X11SessionInfo &info) {
  if (info.uid == 0 || info.username.empty() || is_greeter(info.username) ||
      info.home_dir.empty() || info.home_dir.front() != '/') {
    return false;
  }
  if (!x11_detail::is_valid_wayland_display(info.wayland_display)) {
    return false;
  }

  const std::string expected_runtime = "/run/user/" + std::to_string(info.uid);
  if (info.xdg_runtime_dir.empty()) {
    info.xdg_runtime_dir = expected_runtime;
  }
  if (info.xdg_config_home.empty()) {
    info.xdg_config_home = info.home_dir + "/.config";
  }
  if (!path_is_beneath(info.home_dir, info.xdg_config_home) ||
      !validate_runtime_directory(info)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool validate_candidate(X11SessionInfo &info) {
  if (!validate_base_candidate(info) ||
      !x11_detail::is_valid_local_display(info.display)) {
    return false;
  }
  if (info.xauthority_path.empty()) {
    info.xauthority_path = info.home_dir + "/.Xauthority";
  }
  const int authority_fd = open_valid_xauthority(info);
  if (authority_fd < 0) {
    return false;
  }
  (void)::close(authority_fd);
  return true;
}

[[nodiscard]] std::optional<X11SessionInfo>
resolve_account(uid_t uid, std::string session_id) {
  long hint = ::sysconf(_SC_GETPW_R_SIZE_MAX);
  std::size_t size = hint > 0 ? static_cast<std::size_t>(hint) : 16384U;
  size = std::min(size, x11_detail::kMaxPasswdBufferBytes);

  while (size <= x11_detail::kMaxPasswdBufferBytes) {
    std::vector<char> buffer(size);
    passwd value{};
    passwd *result = nullptr;
    const int status =
        ::getpwuid_r(uid, &value, buffer.data(), buffer.size(), &result);
    if (status == 0 && result != nullptr && value.pw_name != nullptr &&
        value.pw_dir != nullptr &&
        static_cast<std::uintmax_t>(value.pw_uid) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uintmax_t>(value.pw_gid) <=
            std::numeric_limits<std::uint32_t>::max()) {
      X11SessionInfo info;
      info.session_id = std::move(session_id);
      info.username = value.pw_name;
      info.uid = static_cast<std::uint32_t>(value.pw_uid);
      info.gid = static_cast<std::uint32_t>(value.pw_gid);
      info.supplementary_groups.clear();
      info.supplementary_groups.push_back(info.gid);
      info.home_dir = value.pw_dir;
      info.xdg_runtime_dir = "/run/user/" + std::to_string(info.uid);
      info.xdg_config_home = info.home_dir + "/.config";
      return info;
    }
    if (status != ERANGE || size == x11_detail::kMaxPasswdBufferBytes) {
      return std::nullopt;
    }
    size = std::min(size * 2U, x11_detail::kMaxPasswdBufferBytes);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<pid_t> parse_proc_pid(const char *name) noexcept {
  if (name == nullptr) {
    return std::nullopt;
  }
  const std::string_view text{name};
  if (!is_decimal(text)) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    value = value * 10U + static_cast<unsigned>(character - '0');
    if (value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
      return std::nullopt;
    }
  }
  if (value == 0) {
    return std::nullopt;
  }
  return static_cast<pid_t>(value);
}

[[nodiscard]] bool
pid_belongs_to_session(pid_t pid, std::string_view expected_session) noexcept {
  if (pid <= 0 || expected_session.empty()) {
    return false;
  }
  char *session_raw = nullptr;
  const int status = ::sd_pid_get_session(pid, &session_raw);
  OwnedCString session{session_raw, &std::free};
  return status >= 0 && session != nullptr &&
         std::string_view{session.get()} == expected_session;
}

[[nodiscard]] bool find_environment_for_user(uid_t uid,
                                             std::string_view expected_session,
                                             std::string_view expected_display,
                                             X11SessionInfo &info) {
  if (expected_session.empty()) {
    return false;
  }
  std::unique_ptr<DIR, DirectoryCloser> directory{::opendir("/proc")};
  if (!directory) {
    return false;
  }

  std::size_t inspected = 0;
  while (dirent *entry = ::readdir(directory.get())) {
    const auto pid = parse_proc_pid(entry->d_name);
    if (!pid) {
      continue;
    }
    if (++inspected > x11_detail::kMaxProcCandidates) {
      return false;
    }

    if (!pid_belongs_to_session(*pid, expected_session)) {
      continue;
    }

    const auto environment = read_proc_environment(*pid, uid);
    if (!environment || !pid_belongs_to_session(*pid, expected_session)) {
      continue;
    }
    const auto display = environment->find("DISPLAY");
    if (display == environment->end() ||
        !x11_detail::is_valid_local_display(display->second) ||
        (!expected_display.empty() && display->second != expected_display)) {
      continue;
    }
    X11SessionInfo next = info;
    apply_environment(*environment, next);
    if (validate_candidate(next)) {
      if (auto groups = read_proc_groups(*pid, uid, next.gid)) {
        next.supplementary_groups = std::move(*groups);
      }
      info = std::move(next);
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool same_session_info(const X11SessionInfo &left,
                                     const X11SessionInfo &right) noexcept {
  return left.session_id == right.session_id &&
         left.username == right.username && left.uid == right.uid &&
         left.gid == right.gid &&
         left.supplementary_groups == right.supplementary_groups &&
         left.display == right.display &&
         left.xauthority_path == right.xauthority_path &&
         left.home_dir == right.home_dir &&
         left.xdg_runtime_dir == right.xdg_runtime_dir &&
         left.xdg_config_home == right.xdg_config_home &&
         left.wayland_display == right.wayland_display;
}

[[nodiscard]] OwnedCString take_sd_string(char *value) noexcept {
  return OwnedCString{value, &std::free};
}

} // namespace

BoundedXcbConnection::BoundedXcbConnection(xcb_connection_t *connection,
                                           int screen_number) noexcept
    : connection_{connection}, screen_number_{screen_number} {
  if (connection_ == nullptr) {
    return;
  }
  const int descriptor = ::xcb_get_file_descriptor(connection_);
  const timeval timeout{
      .tv_sec = 0,
      .tv_usec = static_cast<suseconds_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              kXcbSocketSendTimeout)
              .count()),
  };
  if (descriptor < 0 || ::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO,
                                     &timeout, sizeof(timeout)) != 0) {
    close();
  }
}

BoundedXcbConnection::~BoundedXcbConnection() { close(); }

BoundedXcbConnection::BoundedXcbConnection(
    BoundedXcbConnection &&other) noexcept
    : connection_{std::exchange(other.connection_, nullptr)},
      screen_number_{std::exchange(other.screen_number_, -1)} {}

BoundedXcbConnection &
BoundedXcbConnection::operator=(BoundedXcbConnection &&other) noexcept {
  if (this != &other) {
    close();
    connection_ = std::exchange(other.connection_, nullptr);
    screen_number_ = std::exchange(other.screen_number_, -1);
  }
  return *this;
}

bool BoundedXcbConnection::is_open() const noexcept {
  return connection_ != nullptr && ::xcb_connection_has_error(connection_) == 0;
}

xcb_connection_t *BoundedXcbConnection::get() const noexcept {
  return connection_;
}

int BoundedXcbConnection::screen_number() const noexcept {
  return screen_number_;
}

void BoundedXcbConnection::close() noexcept {
  if (connection_ != nullptr) {
    ::xcb_disconnect(connection_);
  }
  connection_ = nullptr;
  screen_number_ = -1;
}

void *BoundedXcbConnection::poll_reply(
    std::uint32_t sequence, std::chrono::steady_clock::time_point deadline,
    bool allow_null_reply, x11_detail::XcbOperationResult &result) noexcept {
  result = x11_detail::XcbOperationResult::ConnectionFailed;
  if (!is_open() || sequence == 0) {
    close();
    return nullptr;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    result = x11_detail::XcbOperationResult::TimedOut;
    close();
    return nullptr;
  }
  if (::xcb_flush(connection_) <= 0) {
    close();
    return nullptr;
  }

  while (std::chrono::steady_clock::now() < deadline) {
    void *reply = nullptr;
    xcb_generic_error_t *error = nullptr;
    const int ready =
        ::xcb_poll_for_reply(connection_, sequence, &reply, &error);
    if (ready != 0) {
      if (error != nullptr || (!allow_null_reply && reply == nullptr)) {
        std::free(reply);
        std::free(error);
        result = x11_detail::XcbOperationResult::ProtocolError;
        close();
        return nullptr;
      }
      std::free(error);
      result = x11_detail::XcbOperationResult::Success;
      return reply;
    }
    if (!is_open()) {
      close();
      return nullptr;
    }

    pollfd descriptor{::xcb_get_file_descriptor(connection_), POLLIN, 0};
    const int poll_result =
        ::poll(&descriptor, 1, bounded_poll_timeout(deadline));
    if (poll_result < 0 && errno == EINTR) {
      continue;
    }
    if (poll_result < 0 ||
        (poll_result > 0 &&
         (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
      close();
      return nullptr;
    }
  }

  result = x11_detail::XcbOperationResult::TimedOut;
  close();
  return nullptr;
}

void *BoundedXcbConnection::wait_for_reply(
    std::uint32_t sequence, std::chrono::steady_clock::time_point deadline,
    x11_detail::XcbOperationResult &result) noexcept {
  return poll_reply(sequence, deadline, false, result);
}

bool BoundedXcbConnection::check_request(
    xcb_void_cookie_t cookie, std::chrono::steady_clock::time_point deadline,
    x11_detail::XcbOperationResult &result) noexcept {
  result = x11_detail::XcbOperationResult::ConnectionFailed;
  if (!is_open() || cookie.sequence == 0) {
    close();
    return false;
  }

  const auto barrier = ::xcb_get_input_focus(connection_);
  x11_detail::XcbOperationResult barrier_result{};
  void *barrier_reply =
      poll_reply(barrier.sequence, deadline, false, barrier_result);
  std::free(barrier_reply);
  if (barrier_result != x11_detail::XcbOperationResult::Success) {
    result = barrier_result;
    return false;
  }

  void *reply = nullptr;
  xcb_generic_error_t *error = nullptr;
  const int ready =
      ::xcb_poll_for_reply(connection_, cookie.sequence, &reply, &error);
  if (ready == 0 || reply != nullptr || error != nullptr) {
    std::free(reply);
    std::free(error);
    result = x11_detail::XcbOperationResult::ProtocolError;
    close();
    return false;
  }
  result = x11_detail::XcbOperationResult::Success;
  return true;
}

bool x11_detail::is_valid_local_display(std::string_view value) noexcept {
  if (value.size() < 2 || value.front() != ':' ||
      value.find('\0') != std::string_view::npos) {
    return false;
  }
  value.remove_prefix(1);
  const std::size_t dot = value.find('.');
  const std::string_view display = value.substr(0, dot);
  if (!is_decimal(display)) {
    return false;
  }
  return dot == std::string_view::npos || is_decimal(value.substr(dot + 1));
}

bool x11_detail::xauthority_metadata_is_trusted(
    const XauthorityMetadata &metadata,
    const std::uint32_t expected_uid) noexcept {
  return (metadata.mode & S_IFMT) == S_IFREG &&
         metadata.owner_uid == expected_uid && (metadata.mode & 0022U) == 0 &&
         metadata.size >= 0 &&
         static_cast<std::uint64_t>(metadata.size) <= kMaxXauthorityBytes;
}

bool x11_detail::is_valid_wayland_display(std::string_view value) noexcept {
  if (value.empty()) {
    return true;
  }
  if (value.size() > 128 || value == "." || value == ".." ||
      value.find('/') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char character) {
    const unsigned char byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || character == '-' ||
           character == '_' || character == '.';
  });
}

std::optional<std::chrono::milliseconds>
x11_detail::retry_delay_after_failure(std::size_t failure_count) noexcept {
  using namespace std::chrono_literals;
  constexpr std::array delays{250ms, 500ms, 1000ms};
  if (failure_count == 0 || failure_count > delays.size()) {
    return std::nullopt;
  }
  return delays[failure_count - 1];
}

X11Session::~X11Session() { (void)shutdown_background_refresh(); }

X11Session::X11Session(ProbeFunction probe_function,
                       RetryWaitFunction retry_wait_function)
    : generation_clock_{std::make_shared<std::atomic<std::uint64_t>>(0)},
      probe_function_{std::move(probe_function)},
      retry_wait_function_{std::move(retry_wait_function)},
      write_gate_{std::make_shared<WriteGate>()} {}

bool X11Session::initialize() {
  const RefreshResult result = refresh();
  return result == RefreshResult::HealthyUpdated ||
         result == RefreshResult::HealthyUnchanged;
}

X11Session::RefreshResult X11Session::refresh() {
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock{refresh_mutex_};
    generation = generation_clock_->fetch_add(1, std::memory_order_acq_rel) + 1;
  }
  try {
    return commit_probe(generation,
                        probe_function_ ? probe_function_() : probe_once());
  } catch (...) {
    return commit_probe(generation, {x11_detail::ProbeStatus::Failed, {}});
  }
}

x11_detail::ProbeResult X11Session::probe_once() {
  char *session_raw = nullptr;
  uid_t uid = 0;
  const int active_status = ::sd_seat_get_active("seat0", &session_raw, &uid);
  OwnedCString session = take_sd_string(session_raw);
  if (active_status < 0) {
    if (active_status == -ENODATA || active_status == -ENOENT ||
        active_status == -ENXIO) {
      return {x11_detail::ProbeStatus::SessionAbsent, {}};
    }
    return {x11_detail::ProbeStatus::Failed, {}};
  }
  if (!session || session.get()[0] == '\0' || uid == 0) {
    return {x11_detail::ProbeStatus::SessionAbsent, {}};
  }

  const int is_active = ::sd_session_is_active(session.get());
  const int is_remote = ::sd_session_is_remote(session.get());
  if (is_active < 0 || is_remote < 0) {
    return {x11_detail::ProbeStatus::Failed, {}};
  }
  if (is_active == 0 || is_remote != 0) {
    return {x11_detail::ProbeStatus::SessionAbsent, {}};
  }

  char *class_raw = nullptr;
  char *type_raw = nullptr;
  const int class_status = ::sd_session_get_class(session.get(), &class_raw);
  const int type_status = ::sd_session_get_type(session.get(), &type_raw);
  OwnedCString session_class = take_sd_string(class_raw);
  OwnedCString session_type = take_sd_string(type_raw);
  if (class_status < 0 || type_status < 0 || !session_class || !session_type) {
    return {x11_detail::ProbeStatus::Failed, {}};
  }
  if (std::string_view{session_class.get()} != "user") {
    return {x11_detail::ProbeStatus::SessionAbsent, {}};
  }
  const std::string_view type{session_type.get()};
  if (type != "x11" && type != "wayland") {
    return {x11_detail::ProbeStatus::SessionAbsent, {}};
  }

  auto resolved = resolve_account(uid, session.get());
  if (!resolved || is_greeter(resolved->username)) {
    return {x11_detail::ProbeStatus::Failed, {}};
  }
  X11SessionInfo candidate = std::move(*resolved);

  char *display_raw = nullptr;
  if (::sd_session_get_display(session.get(), &display_raw) >= 0 &&
      display_raw != nullptr) {
    candidate.display = display_raw;
  }
  OwnedCString display = take_sd_string(display_raw);

  pid_t leader = 0;
  bool environment_found = false;
  if (::sd_session_get_leader(session.get(), &leader) >= 0 && leader > 0 &&
      pid_belongs_to_session(leader, session.get())) {
    const auto environment = read_proc_environment(leader, uid);
    if (environment && pid_belongs_to_session(leader, session.get())) {
      apply_environment(*environment, candidate);
      if (auto groups = read_proc_groups(leader, uid, candidate.gid)) {
        candidate.supplementary_groups = std::move(*groups);
      }
      environment_found = true;
    }
  }

  if (!environment_found || !validate_candidate(candidate)) {
    X11SessionInfo scanned = candidate;
    if (!find_environment_for_user(uid, session.get(), candidate.display,
                                   scanned)) {
      if (type == "wayland" && candidate.display.empty()) {
        candidate.wayland_display = candidate.wayland_display.empty()
                                        ? "wayland-0"
                                        : candidate.wayland_display;
        if (validate_base_candidate(candidate)) {
          return {x11_detail::ProbeStatus::SessionAbsent, std::move(candidate)};
        }
      }
      return {x11_detail::ProbeStatus::Failed, {}};
    }
    candidate = std::move(scanned);
  }

  constexpr auto kProbeTimeout = std::chrono::milliseconds{500};
  const auto deadline = std::chrono::steady_clock::now() + kProbeTimeout;
  BoundedXcbConnection connection =
      open_bounded_connection_for(candidate, deadline);
  if (!connection.is_open()) {
    return {x11_detail::ProbeStatus::Failed, {}};
  }
  const int observed_layout = get_xkb_group(connection, deadline);
  const bool healthy = observed_layout >= 0;
  if (healthy) {
    candidate.observed_keyboard_layout = observed_layout;
  }
  return {healthy ? x11_detail::ProbeStatus::Healthy
                  : x11_detail::ProbeStatus::Failed,
          healthy ? std::move(candidate) : X11SessionInfo{}};
}

void X11Session::run_background_probe(
    const std::shared_ptr<BackgroundState> &state) noexcept {
  if (state->kind == BackgroundState::Kind::Keyboard) {
    int group = -1;
    std::uint8_t locked_mods = 0;
    std::uint32_t focus_window = 0;
    if (!state->cancel_requested.load(std::memory_order_acquire)) {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
      auto connection =
          open_bounded_connection_for(state->keyboard_session, deadline);
      if (connection.is_open()) {
        group = get_xkb_group(connection, deadline, &locked_mods);
        if (group >= 0) {
          const auto cookie = ::xcb_get_input_focus(connection.get());
          x11_detail::XcbOperationResult result{};
          auto *reply = static_cast<xcb_get_input_focus_reply_t *>(
              connection.wait_for_reply(cookie.sequence, deadline, result));
          if (result == x11_detail::XcbOperationResult::Success &&
              reply != nullptr && connection.is_open()) {
            focus_window = reply->focus;
          } else {
            group = -1;
          }
          std::free(reply);
        }
      }
    }
    std::lock_guard<std::mutex> lock{state->mu};
    state->keyboard.group = group;
    state->keyboard.focus_window = focus_window;
    state->keyboard.locked_mods = locked_mods;
    state->done = true;
    state->cv.notify_all();
    return;
  }
  std::size_t failures = 0;
  while (true) {
    x11_detail::ProbeResult result;
    try {
      result = state->probe_function ? state->probe_function() : probe_once();
    } catch (...) {
      result.status = x11_detail::ProbeStatus::Failed;
    }

    if (result.status != x11_detail::ProbeStatus::Failed) {
      std::lock_guard<std::mutex> lock{state->mu};
      state->probe = std::move(result);
      state->done = true;
      state->cv.notify_all();
      return;
    }

    {
      std::unique_lock<std::recursive_mutex> write_lock{
          state->write_gate->mutex};
      if (state->generation_clock->load(std::memory_order_acquire) !=
          state->generation) {
        std::lock_guard<std::mutex> lock{state->mu};
        state->probe.status = x11_detail::ProbeStatus::Failed;
        state->done = true;
        state->cv.notify_all();
        return;
      }
      if (state->write_gate->enabled) {
        ++state->write_gate->generation;
        state->write_gate->enabled = false;
      }
    }
    ++failures;
    const auto delay = x11_detail::retry_delay_after_failure(failures);
    if (!delay) {
      std::lock_guard<std::mutex> lock{state->mu};
      state->probe = std::move(result);
      state->done = true;
      state->cv.notify_all();
      return;
    }

    bool cancelled = false;
    if (state->retry_wait_function) {
      try {
        cancelled = state->retry_wait_function(*delay, state->cancel_requested);
      } catch (...) {
        cancelled = true;
      }
    } else {
      std::unique_lock<std::mutex> lock{state->mu};
      cancelled = state->cv.wait_for(lock, *delay, [&state] {
        return state->cancel_requested.load(std::memory_order_acquire);
      });
    }
    if (cancelled || state->cancel_requested.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock{state->mu};
      state->probe.status = x11_detail::ProbeStatus::Failed;
      state->done = true;
      lock.unlock();
      state->cv.notify_all();
      return;
    }
  }
}

bool X11Session::start_background_refresh() {
  std::lock_guard<std::mutex> lock{refresh_mutex_};
  if (pending_refresh_) {
    return false;
  }

  auto state = std::make_shared<BackgroundState>();
  state->generation =
      generation_clock_->fetch_add(1, std::memory_order_acq_rel) + 1;
  state->probe_function = probe_function_;
  state->retry_wait_function = retry_wait_function_;
  state->write_gate = write_gate_;
  state->generation_clock = generation_clock_;
  pending_refresh_ = state;
  refresh_thread_ =
      std::thread{[state] { X11Session::run_background_probe(state); }};
  return true;
}

bool X11Session::start_background_keyboard_observation(
    std::uint64_t request_id) {
  std::lock_guard<std::mutex> lock{refresh_mutex_};
  if (pending_refresh_) {
    return false;
  }
  auto lease = acquire_write_lease();
  if (!lease || !lease->info().wayland_display.empty()) {
    return false;
  }
  auto state = std::make_shared<BackgroundState>();
  state->kind = BackgroundState::Kind::Keyboard;
  state->generation =
      generation_clock_->fetch_add(1, std::memory_order_acq_rel) + 1;
  state->keyboard = KeyboardObservation{request_id, lease->generation(), -1};
  state->keyboard_session = lease->info();
  pending_refresh_ = state;
  // The immutable snapshot travels to the worker; no desktop write lease is
  // held during observation I/O, including a stalled connection handshake.
  lease.reset();
  refresh_thread_ =
      std::thread{[state] { X11Session::run_background_probe(state); }};
  return true;
}

std::optional<X11Session::KeyboardObservation>
X11Session::poll_keyboard_observation() {
  std::lock_guard<std::mutex> lock{refresh_mutex_};
  if (!pending_refresh_ ||
      pending_refresh_->kind != BackgroundState::Kind::Keyboard) {
    return std::nullopt;
  }
  KeyboardObservation observation;
  {
    std::lock_guard<std::mutex> state_lock{pending_refresh_->mu};
    if (!pending_refresh_->done) {
      return std::nullopt;
    }
    observation = pending_refresh_->keyboard;
  }
  if (refresh_thread_.joinable()) {
    refresh_thread_.join();
  }
  auto lease = acquire_write_lease();
  if (!lease || lease->generation() != observation.session_generation ||
      pending_refresh_->generation !=
          generation_clock_->load(std::memory_order_acquire)) {
    observation.group = -1;
  }
  if (observation.group >= 0) {
    std::lock_guard<std::mutex> snapshot_lock{mu_};
    info_.observed_keyboard_layout = observation.group;
  }
  pending_refresh_.reset();
  return observation;
}

std::optional<X11Session::RefreshResult> X11Session::poll_refresh_result() {
  std::lock_guard<std::mutex> lock{refresh_mutex_};
  if (!pending_refresh_ ||
      pending_refresh_->kind != BackgroundState::Kind::Discovery) {
    return std::nullopt;
  }
  x11_detail::ProbeResult probe;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> state_lock{pending_refresh_->mu};
    if (!pending_refresh_->done) {
      return std::nullopt;
    }
    generation = pending_refresh_->generation;
    probe = std::move(pending_refresh_->probe);
  }
  if (refresh_thread_.joinable()) {
    refresh_thread_.join();
  }
  pending_refresh_.reset();
  return commit_probe(generation, std::move(probe));
}

bool X11Session::shutdown_background_refresh(
    std::chrono::milliseconds timeout) noexcept {
  std::shared_ptr<BackgroundState> state;
  {
    std::lock_guard<std::mutex> lock{refresh_mutex_};
    state = pending_refresh_;
  }
  if (state) {
    {
      std::lock_guard<std::mutex> lock{state->mu};
      state->cancel_requested.store(true, std::memory_order_release);
    }
    state->cv.notify_all();
    std::unique_lock<std::mutex> lock{state->mu};
    (void)state->cv.wait_for(lock, timeout, [&state] { return state->done; });
  }

  std::lock_guard<std::mutex> lock{refresh_mutex_};
  bool completed = true;
  if (state) {
    std::lock_guard<std::mutex> state_lock{state->mu};
    completed = state->done;
  }
  if (refresh_thread_.joinable()) {
    if (completed) {
      refresh_thread_.join();
    } else {
      refresh_thread_.detach();
    }
  }
  pending_refresh_.reset();
  (void)generation_clock_->fetch_add(1, std::memory_order_acq_rel);
  return completed;
}

X11Session::RefreshResult
X11Session::commit_probe(std::uint64_t generation,
                         x11_detail::ProbeResult probe) {
  std::unique_lock<std::recursive_mutex> write_lock{write_gate_->mutex};
  if (generation != generation_clock_->load(std::memory_order_acquire)) {
    return RefreshResult::Failed;
  }
  if (probe.status == x11_detail::ProbeStatus::Healthy) {
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock{mu_};
      changed = !initialized_.load(std::memory_order_relaxed) ||
                !same_session_info(info_, probe.info);
      info_ = std::move(probe.info);
    }
    initialized_.store(true, std::memory_order_release);
    if (changed) {
      ++write_gate_->generation;
    }
    write_gate_->enabled = true;
    return changed ? RefreshResult::HealthyUpdated
                   : RefreshResult::HealthyUnchanged;
  }

  if (probe.status == x11_detail::ProbeStatus::SessionAbsent) {
    std::lock_guard<std::mutex> lock{mu_};
    info_ = std::move(probe.info);
  }
  initialized_.store(false, std::memory_order_release);
  if (write_gate_->enabled) {
    ++write_gate_->generation;
  }
  write_gate_->enabled = false;
  return probe.status == x11_detail::ProbeStatus::SessionAbsent
             ? RefreshResult::SessionAbsent
             : RefreshResult::Failed;
}

void X11Session::reset() noexcept {
  std::unique_lock<std::recursive_mutex> write_lock{write_gate_->mutex};
  (void)generation_clock_->fetch_add(1, std::memory_order_acq_rel);
  ++write_gate_->generation;
  initialized_.store(false, std::memory_order_release);
  write_gate_->enabled = false;
  std::lock_guard<std::mutex> lock{mu_};
  info_ = {};
}

bool X11Session::is_valid() const noexcept {
  std::unique_lock<std::recursive_mutex> write_lock{write_gate_->mutex};
  return write_gate_->enabled && initialized_.load(std::memory_order_acquire);
}

X11SessionInfo X11Session::info() const {
  std::lock_guard<std::mutex> lock{mu_};
  return info_;
}

bool X11Session::is_wayland_session() const {
  std::lock_guard<std::mutex> lock{mu_};
  return !info_.wayland_display.empty();
}

X11Session::WriteLease::WriteLease(std::shared_ptr<WriteGate> gate,
                                   std::unique_lock<std::recursive_mutex> lock,
                                   X11SessionInfo info,
                                   std::uint64_t generation) noexcept
    : gate_{std::move(gate)}, lock_{std::move(lock)}, info_{std::move(info)},
      generation_{generation} {}

bool X11Session::WriteLease::valid() const noexcept {
  return gate_ != nullptr && lock_.owns_lock();
}

const X11SessionInfo &X11Session::WriteLease::info() const noexcept {
  return info_;
}

std::uint64_t X11Session::WriteLease::generation() const noexcept {
  return generation_;
}

BoundedXcbConnection X11Session::WriteLease::open_bounded_connection(
    std::chrono::steady_clock::time_point deadline) const {
  if (!valid()) {
    return {};
  }
  return X11Session::open_bounded_connection_for(info_, deadline);
}

BoundedXcbConnection X11Session::WriteLease::open_bounded_connection(
    std::chrono::milliseconds timeout) const {
  if (timeout <= std::chrono::milliseconds::zero()) {
    return {};
  }
  return open_bounded_connection(std::chrono::steady_clock::now() + timeout);
}

std::optional<X11Session::WriteLease> X11Session::acquire_write_lease() const {
  std::unique_lock<std::recursive_mutex> write_lock{write_gate_->mutex};
  if (!write_gate_->enabled || !initialized_.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  X11SessionInfo snapshot;
  {
    std::lock_guard<std::mutex> lock{mu_};
    snapshot = info_;
  }
  return WriteLease{write_gate_, std::move(write_lock), std::move(snapshot),
                    write_gate_->generation};
}

BoundedXcbConnection X11Session::open_bounded_connection_for(
    const X11SessionInfo &info,
    std::chrono::steady_clock::time_point deadline) noexcept {
  if (std::chrono::steady_clock::now() >= deadline) {
    return {};
  }
  int screen_number = -1;
  xcb_connection_t *connection =
      connect_xcb_from_snapshot(info, &screen_number, deadline);
  return BoundedXcbConnection{connection, screen_number};
}

int X11Session::get_current_keyboard_layout() const {
  auto lease = acquire_write_lease();
  if (!lease) {
    return -1;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
  auto connection = lease->open_bounded_connection(deadline);
  return connection.is_open() ? get_xkb_group(connection, deadline) : -1;
}

} // namespace punto
