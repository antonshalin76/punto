/**
 * @file ipc_client.cpp
 * @brief Реализация IPC клиента для связи с punto сервисом
 */

#include "punto/ipc_client.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace punto {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxRequestBytes = 254U;

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_{fd} {}

  ~UniqueFd() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
  }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) {
        (void)::close(fd_);
      }
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

private:
  int fd_;
};

IpcClientResult failure(IpcClientError error) {
  return IpcClientResult{error, ServiceStatus::Unknown, {}};
}

IpcClientError classify_errno(int error) noexcept {
  switch (error) {
  case EACCES:
  case EPERM:
    return IpcClientError::PermissionDenied;
  case ENOENT:
  case ENOTDIR:
  case ENOTSOCK:
  case ECONNREFUSED:
  case ECONNRESET:
  case EPIPE:
  case EHOSTUNREACH:
  case ENETUNREACH:
    return IpcClientError::Unavailable;
  case ETIMEDOUT:
    return IpcClientError::TimedOut;
  default:
    return IpcClientError::IoError;
  }
}

int remaining_timeout_ms(Clock::time_point deadline) noexcept {
  const auto now = Clock::now();
  if (now >= deadline) {
    return 0;
  }

  const auto remaining = deadline - now;
  const auto whole =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  std::int64_t milliseconds = whole.count();
  if (whole < remaining) {
    ++milliseconds;
  }
  return static_cast<int>(std::min<std::int64_t>(
      milliseconds,
      static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}

IpcClientError wait_for(int fd, short events,
                        Clock::time_point deadline) noexcept {
  while (true) {
    const int timeout_ms = remaining_timeout_ms(deadline);
    if (timeout_ms == 0) {
      return IpcClientError::TimedOut;
    }

    pollfd descriptor{fd, events, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready > 0) {
      if ((descriptor.revents & POLLNVAL) != 0) {
        return IpcClientError::IoError;
      }
      return IpcClientError::None;
    }
    if (ready == 0) {
      return IpcClientError::TimedOut;
    }
    if (errno != EINTR) {
      return classify_errno(errno);
    }
  }
}

UniqueFd create_client_socket(IpcClientError &error) noexcept {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd >= 0) {
    return UniqueFd{fd};
  }

  if (errno != EINVAL && errno != EPROTONOSUPPORT) {
    error = classify_errno(errno);
    return UniqueFd{};
  }

  fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    error = classify_errno(errno);
    return UniqueFd{};
  }

  UniqueFd socket{fd};
  const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
  const int status_flags = ::fcntl(fd, F_GETFL, 0);
  if (descriptor_flags < 0 || status_flags < 0 ||
      ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0 ||
      ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
    error = classify_errno(errno);
    return UniqueFd{};
  }
  return socket;
}

IpcClientError connect_to_server(int fd, std::string_view socket_path,
                                 Clock::time_point deadline) noexcept {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  address.sun_path[socket_path.size()] = '\0';
  const auto address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1U);

  const int result =
      ::connect(fd, reinterpret_cast<sockaddr *>(&address), address_size);
  if (result == 0) {
    return IpcClientError::None;
  }

  const int connect_error = errno;
  if (connect_error != EINPROGRESS && connect_error != EALREADY &&
      connect_error != EINTR && connect_error != EAGAIN) {
    return classify_errno(connect_error);
  }

  const IpcClientError poll_error = wait_for(fd, POLLOUT, deadline);
  if (poll_error != IpcClientError::None) {
    return poll_error;
  }

  int socket_error = 0;
  socklen_t error_size = sizeof(socket_error);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0) {
    return classify_errno(errno);
  }
  return socket_error == 0 ? IpcClientError::None
                           : classify_errno(socket_error);
}

IpcClientError send_request(int fd, std::string_view command,
                            Clock::time_point deadline) noexcept {
  std::string request{command};
  request.push_back('\n');

  std::size_t offset = 0;
  while (offset < request.size()) {
    if (Clock::now() >= deadline) {
      return IpcClientError::TimedOut;
    }
    const ssize_t sent = ::send(fd, request.data() + offset,
                                request.size() - offset, MSG_NOSIGNAL);
    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const IpcClientError poll_error = wait_for(fd, POLLOUT, deadline);
      if (poll_error != IpcClientError::None) {
        return poll_error;
      }
      continue;
    }
    return classify_errno(sent == 0 ? EIO : errno);
  }
  return IpcClientError::None;
}

IpcClientResult parse_response(std::string response) {
  if (response == "OK" ||
      (response.starts_with("OK ") && response.size() > 3U)) {
    return {IpcClientError::None, ServiceStatus::Unknown, std::move(response)};
  }
  if (response == "ERROR" ||
      (response.starts_with("ERROR ") && response.size() > 6U)) {
    return {IpcClientError::ServerRejected, ServiceStatus::Unknown,
            std::move(response)};
  }
  return failure(IpcClientError::ProtocolError);
}

IpcClientResult receive_response(int fd, Clock::time_point deadline) {
  std::string response;
  response.reserve(128U);
  bool saw_lf = false;
  std::array<char, 512> buffer{};

  while (true) {
    if (Clock::now() >= deadline) {
      return failure(IpcClientError::TimedOut);
    }

    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received > 0) {
      const auto count = static_cast<std::size_t>(received);
      for (std::size_t index = 0; index < count; ++index) {
        const unsigned char byte = static_cast<unsigned char>(buffer[index]);
        if (saw_lf) {
          return failure(IpcClientError::ProtocolError);
        }
        if (byte == static_cast<unsigned char>('\n')) {
          if (response.empty()) {
            return failure(IpcClientError::ProtocolError);
          }
          saw_lf = true;
          continue;
        }
        if (byte < 0x20U || byte > 0x7eU) {
          return failure(IpcClientError::ProtocolError);
        }
        if (response.size() == IpcClient::kMaxResponseBytes) {
          return failure(IpcClientError::ProtocolError);
        }
        response.push_back(static_cast<char>(byte));
      }
      continue;
    }
    if (received == 0) {
      if (!saw_lf) {
        return failure(IpcClientError::ProtocolError);
      }
      return parse_response(std::move(response));
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      const IpcClientError poll_error = wait_for(fd, POLLIN, deadline);
      if (poll_error != IpcClientError::None) {
        return failure(poll_error);
      }
      continue;
    }
    return failure(classify_errno(errno));
  }
}

} // namespace

std::vector<std::string> IpcClient::list_diagnostic_socket_paths() {
  std::vector<std::string> sockets;

  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(
           "/var/run",
           std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }

    std::error_code type_ec;
    if (entry.symlink_status(type_ec).type() !=
            std::filesystem::file_type::socket ||
        type_ec) {
      continue;
    }

    const std::string name = entry.path().filename().string();
    if (!name.starts_with("punto-") || !name.ends_with(".sock")) {
      continue;
    }

    const std::string path = entry.path().string();
    if (path != kSocketPath) {
      sockets.push_back(path);
    }
  }

  std::sort(sockets.begin(), sockets.end());
  sockets.erase(std::unique(sockets.begin(), sockets.end()), sockets.end());

  return sockets;
}

IpcClientResult
IpcClient::send_command_to_socket(const std::string &command,
                                  const std::string &socket_path) {
  if (command.empty() || command.size() > kMaxRequestBytes ||
      command.find('\r') != std::string::npos ||
      command.find('\n') != std::string::npos ||
      command.find('\0') != std::string::npos) {
    return failure(IpcClientError::InvalidRequest);
  }
  if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    return failure(IpcClientError::PathTooLong);
  }
  if (socket_path.empty() || socket_path.find('\0') != std::string::npos) {
    return failure(IpcClientError::InvalidRequest);
  }

  const Clock::time_point deadline =
      Clock::now() + std::chrono::milliseconds{kTimeoutMs};
  IpcClientError socket_error = IpcClientError::None;
  UniqueFd fd = create_client_socket(socket_error);
  if (fd.get() < 0) {
    return failure(socket_error);
  }

  IpcClientError operation_error =
      connect_to_server(fd.get(), socket_path, deadline);
  if (operation_error != IpcClientError::None) {
    return failure(operation_error);
  }
  operation_error = send_request(fd.get(), command, deadline);
  if (operation_error != IpcClientError::None) {
    return failure(operation_error);
  }
  return receive_response(fd.get(), deadline);
}

IpcClientResult IpcClient::send_command(const std::string &command) {
  return send_command_to_socket(command, kSocketPath);
}

IpcClientResult IpcClient::diagnose_socket(const std::string &socket_path) {
  IpcClientResult result = send_command_to_socket("GET_STATUS", socket_path);
  if (!result.ok()) {
    return result;
  }

  if (result.response == "OK ENABLED") {
    result.status = ServiceStatus::Enabled;
    return result;
  }
  if (result.response == "OK DISABLED") {
    result.status = ServiceStatus::Disabled;
    return result;
  }
  result.error = IpcClientError::ProtocolError;
  result.response.clear();
  return result;
}

#if defined(PUNTO_IPC_CLIENT_INTERNAL_TESTING)
IpcClientResult IpcClient::exchange_for_test(const std::string &command,
                                             const std::string &socket_path) {
  return send_command_to_socket(command, socket_path);
}
#endif

ServiceStatus IpcClient::get_status() {
  return diagnose_socket(kSocketPath).status;
}

bool IpcClient::reload_config(const std::string &config_path) {
  std::string cmd = "RELOAD";
  if (!config_path.empty()) {
    cmd += " ";
    cmd += config_path;
  }

  return send_command(cmd).ok();
}

bool IpcClient::is_service_available() {
  return diagnose_socket(kSocketPath).ok();
}

} // namespace punto
