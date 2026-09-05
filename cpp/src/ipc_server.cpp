/**
 * @file ipc_server.cpp
 * @brief Bounded Unix-domain IPC transport for the Punto daemon.
 */

#define PUNTO_IPC_INTERNAL_TESTING 1
#include "punto/ipc_server.hpp"
#include "punto/runtime_file.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace punto {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

#if defined(PUNTO_IPC_MAILBOX_TESTING)
std::atomic<IpcMailboxProducerTestHook> g_mailbox_producer_test_hook{nullptr};
std::atomic<IpcMailboxProducerTestHook> g_mailbox_admitted_test_hook{nullptr};
#endif

constexpr std::size_t kMaximumPayloadBytes = 254;
constexpr auto kFrameTimeout = 250ms;
constexpr auto kShutdownTimeout = 3000ms;
constexpr auto kSocketLeaseTimeout = 100ms;
constexpr int kIdlePollTimeoutMs = 500;
constexpr std::array<std::chrono::milliseconds, 4> kAcceptBackoffs{
    50ms, 100ms, 250ms, 500ms};

std::string format_response(const IpcResult &result) {
  std::string response = result.success ? "OK" : "ERROR";
  if (!result.message.empty()) {
    response.push_back(' ');
    response.append(result.message);
  }
  response.push_back('\n');
  return response;
}

bool is_read_only(IpcVerb verb) noexcept {
  return verb == IpcVerb::GetStatus || verb == IpcVerb::Stats;
}

IpcCommandSink
make_mailbox_sink(const std::shared_ptr<IpcCommandMailbox> &mailbox,
                  IpcEndpointMode endpoint_mode) {
  return [mailbox, endpoint_mode](IpcRequest request,
                                  IpcResponseCompletion complete) noexcept {
    if (!mailbox) {
      return IpcEnqueueResult::Failed;
    }
    if (endpoint_mode == IpcEndpointMode::DiagnosticReadOnly &&
        !is_read_only(request.verb)) {
      try {
        complete({false, "Read-only diagnostic endpoint"});
        return IpcEnqueueResult::Accepted;
      } catch (...) {
        return IpcEnqueueResult::Failed;
      }
    }
    return mailbox->try_enqueue(std::move(request), std::move(complete));
  };
}

int ceil_milliseconds(Clock::duration duration) noexcept {
  if (duration <= Clock::duration::zero()) {
    return 0;
  }

  const auto whole =
      std::chrono::duration_cast<std::chrono::milliseconds>(duration);
  auto count = whole.count();
  if (whole < duration) {
    ++count;
  }
  return static_cast<int>(std::min<std::int64_t>(
      count, static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}

bool is_accept_transient(int error) noexcept {
  return error == EINTR || error == EAGAIN || error == EWOULDBLOCK ||
         error == ECONNABORTED;
}

bool is_accept_resource_error(int error) noexcept {
  return error == EMFILE || error == ENFILE || error == ENOMEM;
}

class SteadyIpcClock final : public IpcClock {
public:
  [[nodiscard]] Clock::time_point now() const noexcept override {
    return Clock::now();
  }
};

class PosixIpcTransportIo final : public IpcTransportIo {
public:
  PosixIpcTransportIo() noexcept {
#if defined(__linux__)
    if (::pipe2(wakeup_fds_.data(), O_NONBLOCK | O_CLOEXEC) == 0) {
      return;
    }
#endif
    if (::pipe(wakeup_fds_.data()) != 0) {
      wakeup_fds_ = {-1, -1};
      return;
    }

    for (const int fd : wakeup_fds_) {
      const int status_flags = ::fcntl(fd, F_GETFL, 0);
      const int descriptor_flags = ::fcntl(fd, F_GETFD, 0);
      if (status_flags < 0 || descriptor_flags < 0 ||
          ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
          ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        close_wakeup_pipe();
        return;
      }
    }
  }

  ~PosixIpcTransportIo() override { close_wakeup_pipe(); }

  [[nodiscard]] bool valid() const noexcept {
    return wakeup_fds_[0] >= 0 && wakeup_fds_[1] >= 0;
  }

  IpcPollResult poll(std::span<pollfd> descriptors, int timeout_ms) override {
    std::vector<pollfd> all;
    all.reserve(descriptors.size() + 1U);
    all.insert(all.end(), descriptors.begin(), descriptors.end());
    all.push_back({wakeup_fds_[0], POLLIN, 0});

    const int result =
        ::poll(all.data(), static_cast<nfds_t>(all.size()), timeout_ms);
    const int saved_errno = errno;
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      descriptors[index].revents = all[index].revents;
    }

    if (result < 0) {
      errno = saved_errno;
      return IpcPollResult::Error;
    }
    if (result == 0) {
      return IpcPollResult::Timeout;
    }

    if ((all.back().revents & POLLIN) != 0) {
      drain_wakeup_pipe();
      return IpcPollResult::Wakeup;
    }
    return IpcPollResult::Ready;
  }

  void wake() noexcept override {
    if (wakeup_fds_[1] < 0) {
      return;
    }

    constexpr std::byte marker{1};
    while (true) {
      const ssize_t written = ::write(wakeup_fds_[1], &marker, sizeof(marker));
      if (written == static_cast<ssize_t>(sizeof(marker))) {
        return;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      return;
    }
  }

  int accept_client(int listener_fd) override {
#if defined(__linux__)
    const int accepted_with_flags =
        ::accept4(listener_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted_with_flags >= 0 || errno != ENOSYS) {
      return accepted_with_flags;
    }
#endif
    const int accepted = ::accept(listener_fd, nullptr, nullptr);
    if (accepted < 0) {
      return -1;
    }

    const int status_flags = ::fcntl(accepted, F_GETFL, 0);
    const int descriptor_flags = ::fcntl(accepted, F_GETFD, 0);
    if (status_flags < 0 || descriptor_flags < 0 ||
        ::fcntl(accepted, F_SETFL, status_flags | O_NONBLOCK) != 0 ||
        ::fcntl(accepted, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
      const int saved_errno = errno;
      (void)::close(accepted);
      errno = saved_errno;
      return -1;
    }
    return accepted;
  }

  ssize_t receive(int fd, std::span<char> destination) override {
    return ::recv(fd, destination.data(), destination.size(), 0);
  }

  ssize_t send(int fd, std::span<const char> source) override {
    return ::send(fd, source.data(), source.size(), MSG_NOSIGNAL);
  }

  int close_fd(int fd) override { return ::close(fd); }

  int unlink_path(std::string_view path) override {
    const std::string terminated{path};
    while (::unlink(terminated.c_str()) != 0) {
      if (errno != EINTR) {
        return -1;
      }
    }
    return 0;
  }

  std::optional<RuntimeArtifactFileIdentity>
  path_identity(std::string_view path) override {
    const std::string terminated{path};
    struct stat status {};
    if (::lstat(terminated.c_str(), &status) != 0) {
      return std::nullopt;
    }
    return RuntimeArtifactFileIdentity{status.st_dev, status.st_ino};
  }

private:
  void drain_wakeup_pipe() noexcept {
    std::array<std::byte, 64> buffer{};
    while (true) {
      const ssize_t count =
          ::read(wakeup_fds_[0], buffer.data(), buffer.size());
      if (count > 0) {
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      return;
    }
  }

  void close_wakeup_pipe() noexcept {
    for (int &fd : wakeup_fds_) {
      if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
      }
    }
  }

  std::array<int, 2> wakeup_fds_{-1, -1};
};

class PosixRuntimeArtifactSecurity final : public RuntimeArtifactSecurity {
public:
  Result secure_socket(int listener_fd, std::string_view path,
                       RuntimeArtifactIdentity intended_identity) override {
    const std::string terminated{path};
    if (terminated.empty() ||
        (intended_identity.mode & static_cast<mode_t>(~0777)) != 0) {
      return Result::ChmodFailed;
    }

    sockaddr_un bound_address{};
    socklen_t bound_length = sizeof(bound_address);
    if (::getsockname(listener_fd, reinterpret_cast<sockaddr *>(&bound_address),
                      &bound_length) != 0 ||
        bound_address.sun_family != AF_UNIX ||
        std::string_view{bound_address.sun_path} != path) {
      return Result::ChmodFailed;
    }

    const int path_fd =
        ::open(terminated.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (path_fd < 0) {
      return Result::ChmodFailed;
    }
    struct PathFdGuard {
      int fd;
      ~PathFdGuard() { (void)::close(fd); }
    } guard{path_fd};

    struct stat before {};
    if (::fstat(path_fd, &before) != 0 || !S_ISSOCK(before.st_mode)) {
      return Result::ChmodFailed;
    }
    if (::fchownat(path_fd, "", intended_identity.uid, intended_identity.gid,
                   AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) != 0) {
      return Result::ChownFailed;
    }

    const std::string descriptor_path =
        "/proc/self/fd/" + std::to_string(path_fd);
    if (::chmod(descriptor_path.c_str(), intended_identity.mode) != 0) {
      return Result::ChmodFailed;
    }

    struct stat secured {};
    struct stat current_path {};
    if (::fstat(path_fd, &secured) != 0 || !S_ISSOCK(secured.st_mode) ||
        ::lstat(terminated.c_str(), &current_path) != 0 ||
        current_path.st_dev != secured.st_dev ||
        current_path.st_ino != secured.st_ino ||
        (secured.st_mode & static_cast<mode_t>(0777)) !=
            intended_identity.mode) {
      return Result::ChmodFailed;
    }
    if (secured.st_uid != intended_identity.uid ||
        secured.st_gid != intended_identity.gid) {
      return Result::ChownFailed;
    }
    return Result::Secured;
  }
};

struct CreatedSocket {
  int fd = -1;
  std::string path;
  std::optional<RuntimeArtifactFileIdentity> identity;
};

class SocketLease;

struct CreatedServerSocket {
  CreatedSocket socket;
  std::unique_ptr<SocketLease> lease;
};

enum class SocketLeaseResult { Acquired, Contended, Rejected };

class SocketLease {
public:
  SocketLease() = default;
  ~SocketLease() {
    if (lease_fd_ >= 0) {
      (void)::close(lease_fd_);
    }
    if (directory_fd_ >= 0) {
      (void)::close(directory_fd_);
    }
  }

  SocketLease(const SocketLease &) = delete;
  SocketLease &operator=(const SocketLease &) = delete;

  SocketLeaseResult acquire(const std::filesystem::path &socket_path) {
    std::filesystem::path parent = socket_path.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    directory_fd_ = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd_ < 0) {
      return SocketLeaseResult::Rejected;
    }

    struct stat directory_status {};
    if (::fstat(directory_fd_, &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid() ||
        (directory_status.st_mode & static_cast<mode_t>(S_IWGRP | S_IWOTH)) !=
            0) {
      return SocketLeaseResult::Rejected;
    }

    const std::string lease_name =
        "." + socket_path.filename().string() + ".lock";
    if (lease_name == "..lock") {
      return SocketLeaseResult::Rejected;
    }
    lease_fd_ = ::openat(directory_fd_, lease_name.c_str(),
                         O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lease_fd_ < 0) {
      return SocketLeaseResult::Rejected;
    }

    struct stat lease_status {};
    struct stat lease_path_status {};
    if (::fstat(lease_fd_, &lease_status) != 0 ||
        !S_ISREG(lease_status.st_mode) || lease_status.st_nlink != 1 ||
        lease_status.st_uid != ::geteuid() ||
        (lease_status.st_mode & static_cast<mode_t>(0077)) != 0 ||
        ::fstatat(directory_fd_, lease_name.c_str(), &lease_path_status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        lease_path_status.st_dev != lease_status.st_dev ||
        lease_path_status.st_ino != lease_status.st_ino) {
      return SocketLeaseResult::Rejected;
    }

    const Clock::time_point deadline = Clock::now() + kSocketLeaseTimeout;
    while (::flock(lease_fd_, LOCK_EX | LOCK_NB) != 0) {
      const int error = errno;
      if (error != EINTR && error != EAGAIN && error != EWOULDBLOCK) {
        return SocketLeaseResult::Rejected;
      }
      if (Clock::now() >= deadline) {
        return SocketLeaseResult::Contended;
      }
      std::this_thread::sleep_for(1ms);
    }

    struct stat locked_status {};
    struct stat locked_path_status {};
    const bool valid =
        ::fstat(lease_fd_, &locked_status) == 0 &&
        S_ISREG(locked_status.st_mode) && locked_status.st_nlink == 1 &&
        ::fstatat(directory_fd_, lease_name.c_str(), &locked_path_status,
                  AT_SYMLINK_NOFOLLOW) == 0 &&
        locked_path_status.st_dev == locked_status.st_dev &&
        locked_path_status.st_ino == locked_status.st_ino;
    return valid ? SocketLeaseResult::Acquired : SocketLeaseResult::Rejected;
  }

private:
  int directory_fd_ = -1;
  int lease_fd_ = -1;
};

struct SocketCreationAttempt {
  CreatedServerSocket created;
  bool fallback_allowed = false;
};

std::optional<RuntimeArtifactFileIdentity>
socket_path_identity(const std::string &path) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode)) {
    return std::nullopt;
  }
  return RuntimeArtifactFileIdentity{status.st_dev, status.st_ino};
}

bool unlink_socket_if_same(const std::string &path,
                           RuntimeArtifactFileIdentity expected) {
  const auto current = socket_path_identity(path);
  if (!current.has_value() || *current != expected) {
    return false;
  }
  while (::unlink(path.c_str()) != 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

enum class SocketProbeResult { Active, Stale, Indeterminate };

SocketProbeResult probe_socket_bounded(const std::string &path) {
  const int fd =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return SocketProbeResult::Indeterminate;
  }
  struct ProbeFdGuard {
    int fd;
    ~ProbeFdGuard() { (void)::close(fd); }
  } guard{fd};

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    return SocketProbeResult::Indeterminate;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);

  if (::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) == 0) {
    return SocketProbeResult::Active;
  }
  int connect_error = errno;
  if (connect_error == ECONNREFUSED || connect_error == ENOENT) {
    return SocketProbeResult::Stale;
  }
  if (connect_error != EAGAIN && connect_error != EINPROGRESS &&
      connect_error != EALREADY && connect_error != EINTR) {
    return SocketProbeResult::Indeterminate;
  }

  const Clock::time_point deadline = Clock::now() + 50ms;
  while (Clock::now() < deadline) {
    const int timeout_ms = ceil_milliseconds(deadline - Clock::now());
    pollfd descriptor{fd, POLLOUT, 0};
    const int result = ::poll(&descriptor, 1, timeout_ms);
    if (result == 0) {
      return SocketProbeResult::Indeterminate;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return SocketProbeResult::Indeterminate;
    }

    socklen_t error_size = sizeof(connect_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error, &error_size) !=
        0) {
      return SocketProbeResult::Indeterminate;
    }
    if (connect_error == 0) {
      return SocketProbeResult::Active;
    }
    if (connect_error == ECONNREFUSED || connect_error == ENOENT) {
      return SocketProbeResult::Stale;
    }
    return SocketProbeResult::Indeterminate;
  }
  return SocketProbeResult::Indeterminate;
}

bool resolve_default_group(RuntimeArtifactIdentity &identity) {
  if (identity.gid != static_cast<gid_t>(-1)) {
    return true;
  }

  const RuntimeFileSecurity security = default_runtime_file_security();
  if (security.group_gid == static_cast<gid_t>(-1)) {
    return false;
  }
  identity.gid = security.group_gid;
  return true;
}

CreatedSocket create_bound_socket(const std::string &path,
                                  RuntimeArtifactIdentity identity,
                                  RuntimeArtifactSecurity &security,
                                  int &bind_error) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
    bind_error = ENAMETOOLONG;
    return {};
  }

  const int fd =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    bind_error = errno;
    return {};
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  if (::bind(fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    bind_error = errno;
    (void)::close(fd);
    return {};
  }

  const auto bound_identity = socket_path_identity(path);
  if (!bound_identity.has_value()) {
    bind_error = EIO;
    (void)::close(fd);
    return {};
  }

  struct BoundSocketGuard {
    int fd;
    std::string path;
    RuntimeArtifactFileIdentity identity;
    bool released = false;
    ~BoundSocketGuard() {
      if (released) {
        return;
      }
      (void)::close(fd);
      (void)unlink_socket_if_same(path, identity);
    }
  } guard{fd, path, *bound_identity};

  if (security.secure_socket(fd, path, identity) !=
      RuntimeArtifactSecurity::Result::Secured) {
    bind_error = EACCES;
    return {};
  }

  if (::listen(fd, SOMAXCONN) != 0) {
    bind_error = errno;
    return {};
  }

  const auto secured_identity = socket_path_identity(path);
  if (!secured_identity.has_value() || *secured_identity != *bound_identity) {
    bind_error = EIO;
    return {};
  }

  bind_error = 0;
  guard.released = true;
  return {fd, path, bound_identity};
}

CreatedSocket create_or_reclaim_socket(const std::string &path,
                                       RuntimeArtifactIdentity identity,
                                       RuntimeArtifactSecurity &security,
                                       int &bind_error) {
  CreatedSocket created =
      create_bound_socket(path, identity, security, bind_error);
  if (created.fd >= 0 || bind_error != EADDRINUSE) {
    return created;
  }

  const auto collision_identity = socket_path_identity(path);
  if (!collision_identity.has_value() ||
      probe_socket_bounded(path) != SocketProbeResult::Stale) {
    return {};
  }
  if (!unlink_socket_if_same(path, *collision_identity)) {
    bind_error = EBUSY;
    return {};
  }
  return create_bound_socket(path, identity, security, bind_error);
}

SocketCreationAttempt
create_socket_with_lease(const std::string &path,
                         RuntimeArtifactIdentity identity,
                         RuntimeArtifactSecurity &security) {
  auto lease = std::make_unique<SocketLease>();
  const SocketLeaseResult lease_result = lease->acquire(path);
  if (lease_result != SocketLeaseResult::Acquired) {
    SocketCreationAttempt failed;
    failed.fallback_allowed = lease_result == SocketLeaseResult::Contended;
    return failed;
  }
  int bind_error = 0;
  CreatedSocket socket =
      create_or_reclaim_socket(path, identity, security, bind_error);
  if (socket.fd < 0) {
    SocketCreationAttempt failed;
    failed.fallback_allowed = bind_error == EADDRINUSE;
    return failed;
  }
  SocketCreationAttempt succeeded;
  succeeded.created.socket = std::move(socket);
  succeeded.created.lease = std::move(lease);
  return succeeded;
}

CreatedServerSocket create_server_socket(IpcServerOptions options,
                                         RuntimeArtifactSecurity &security) {
  if (!resolve_default_group(options.socket_identity)) {
    return {};
  }

  SocketCreationAttempt primary_attempt = create_socket_with_lease(
      options.primary_socket_path, options.socket_identity, security);
  if (primary_attempt.created.socket.fd >= 0) {
    return std::move(primary_attempt.created);
  }

  if (!options.allow_fallback_sockets || !primary_attempt.fallback_allowed) {
    return {};
  }

  const std::filesystem::path primary{options.primary_socket_path};
  const std::filesystem::path fallback =
      primary.parent_path() /
      (primary.stem().string() + "-" + std::to_string(::getpid()) +
       primary.extension().string());
  SocketCreationAttempt fallback_attempt = create_socket_with_lease(
      fallback.string(), options.socket_identity, security);
  return std::move(fallback_attempt.created);
}

} // namespace

struct IpcCommandMailbox::Impl {
  static std::size_t storage_size(std::size_t capacity) {
    if (capacity == std::numeric_limits<std::size_t>::max()) {
      throw std::length_error("IPC mailbox capacity is too large");
    }
    return capacity + 1U;
  }

  explicit Impl(std::size_t requested_capacity)
      : usable_capacity{std::max<std::size_t>(requested_capacity, 1U)},
        slots(storage_size(usable_capacity)) {}

  struct BusyGuard {
    explicit BusyGuard(std::atomic_flag &owned_flag) noexcept
        : flag{owned_flag} {}
    ~BusyGuard() { flag.clear(std::memory_order_release); }

    BusyGuard(const BusyGuard &) = delete;
    BusyGuard &operator=(const BusyGuard &) = delete;

    std::atomic_flag &flag;
  };

  const std::size_t usable_capacity;
  std::vector<std::optional<IpcPendingCommand>> slots;
  std::atomic<std::size_t> write_position{0};
  std::atomic<std::size_t> read_position{0};
  std::atomic<bool> open{true};
  std::atomic_flag producer_busy = ATOMIC_FLAG_INIT;
  std::atomic_flag consumer_busy = ATOMIC_FLAG_INIT;
};

static_assert(std::is_nothrow_move_constructible_v<IpcPendingCommand>);

IpcCommandMailbox::IpcCommandMailbox(std::size_t capacity)
    : impl_{std::make_unique<Impl>(capacity)} {}

IpcCommandMailbox::~IpcCommandMailbox() = default;

IpcEnqueueResult
IpcCommandMailbox::try_enqueue(IpcRequest request,
                               IpcResponseCompletion complete) noexcept {
  if (!impl_ || !impl_->open.load(std::memory_order_acquire)) {
    return IpcEnqueueResult::AdmissionClosed;
  }
  if (impl_->producer_busy.test_and_set(std::memory_order_acquire)) {
    return IpcEnqueueResult::Failed;
  }
  Impl::BusyGuard producer_guard{impl_->producer_busy};

#if defined(PUNTO_IPC_MAILBOX_TESTING)
  if (const auto hook =
          g_mailbox_producer_test_hook.load(std::memory_order_acquire);
      hook != nullptr) {
    hook();
  }
#endif

  const std::size_t write =
      impl_->write_position.load(std::memory_order_relaxed);
  const std::size_t next = (write + 1U) % impl_->slots.size();
  if (!impl_->open.load(std::memory_order_acquire)) {
    return IpcEnqueueResult::AdmissionClosed;
  }
  if (next == impl_->read_position.load(std::memory_order_acquire)) {
    return IpcEnqueueResult::Failed;
  }
  try {
    impl_->slots[write].emplace(
        IpcPendingCommand{std::move(request), std::move(complete)});
  } catch (...) {
    return IpcEnqueueResult::Failed;
  }
  impl_->write_position.store(next, std::memory_order_release);
#if defined(PUNTO_IPC_MAILBOX_TESTING)
  if (const auto hook = g_mailbox_admitted_test_hook.load(std::memory_order_acquire);
      hook != nullptr) {
    hook();
  }
#endif
  return IpcEnqueueResult::Accepted;
}

std::optional<IpcPendingCommand> IpcCommandMailbox::try_dequeue() noexcept {
  if (!impl_ || impl_->consumer_busy.test_and_set(std::memory_order_acquire)) {
    return std::nullopt;
  }
  Impl::BusyGuard consumer_guard{impl_->consumer_busy};

  const std::size_t read = impl_->read_position.load(std::memory_order_relaxed);
  if (read == impl_->write_position.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  std::optional<IpcPendingCommand> command{std::move(impl_->slots[read])};
  impl_->slots[read].reset();
  impl_->read_position.store((read + 1U) % impl_->slots.size(),
                             std::memory_order_release);
  return command;
}

bool IpcCommandMailbox::close(std::chrono::milliseconds timeout) noexcept {
  if (!impl_) {
    return true;
  }
  impl_->open.store(false, std::memory_order_release);
  const auto started = Clock::now();
  const auto bounded_timeout = std::max(timeout, std::chrono::milliseconds{0});
  while (impl_->producer_busy.test(std::memory_order_acquire)) {
    if (Clock::now() - started >= bounded_timeout) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

#if defined(PUNTO_IPC_MAILBOX_TESTING)
void set_ipc_mailbox_producer_test_hook(
    IpcMailboxProducerTestHook hook) noexcept {
  g_mailbox_producer_test_hook.store(hook, std::memory_order_release);
}
void set_ipc_mailbox_admitted_test_hook(
    IpcMailboxProducerTestHook hook) noexcept {
  g_mailbox_admitted_test_hook.store(hook, std::memory_order_release);
}
#endif

bool IpcCommandMailbox::is_open() const noexcept {
  return impl_ && impl_->open.load(std::memory_order_acquire);
}

std::size_t IpcCommandMailbox::size() const noexcept {
  if (!impl_) {
    return 0;
  }
  const std::size_t read = impl_->read_position.load(std::memory_order_acquire);
  const std::size_t write =
      impl_->write_position.load(std::memory_order_acquire);
  if (write >= read) {
    return write - read;
  }
  return impl_->slots.size() - read + write;
}

std::size_t IpcCommandMailbox::capacity() const noexcept {
  return impl_ ? impl_->usable_capacity : 0;
}

bool IpcCommandMailbox::has_pending_mutation() const noexcept {
  if (!impl_) return false;
  if (impl_->consumer_busy.test_and_set(std::memory_order_acquire)) return true;
  Impl::BusyGuard consumer_guard{impl_->consumer_busy};
  const auto write = impl_->write_position.load(std::memory_order_acquire);
  for (auto read = impl_->read_position.load(std::memory_order_relaxed);
       read != write; read = (read + 1U) % impl_->slots.size()) {
    const auto &slot = impl_->slots[read];
    if (!slot || !is_read_only(slot->request.verb)) return true;
  }
  return false;
}

IpcFramePolicy::IpcFramePolicy(Clock::time_point accepted_at)
    : accepted_at_{accepted_at} {}

IpcFramePolicy::Clock::time_point IpcFramePolicy::accepted_at() const noexcept {
  return accepted_at_;
}

IpcFramePolicy::Clock::time_point IpcFramePolicy::deadline() const noexcept {
  return accepted_at_ + kFrameTimeout;
}

IpcFramePolicy::DeadlineResult
IpcFramePolicy::on_time(Clock::time_point now) const noexcept {
  return now < deadline() ? DeadlineResult::OnTime : DeadlineResult::Expired;
}

IpcFramePolicy::FeedResult
IpcFramePolicy::feed(std::span<const std::byte> bytes, Clock::time_point now) {
  if (state_ != FeedResult::NeedMore) {
    return state_;
  }
  if (on_time(now) == DeadlineResult::Expired) {
    state_ = FeedResult::ProtocolError;
    request_.reset();
    return state_;
  }

  for (const std::byte byte : bytes) {
    const char character =
        static_cast<char>(std::to_integer<unsigned char>(byte));
    if (character == '\0') {
      state_ = FeedResult::ProtocolError;
      request_.reset();
      return state_;
    }
    if (character == '\n') {
      if (!payload_.empty() && payload_.back() == '\r') {
        payload_.pop_back();
      }
      state_ = FeedResult::Complete;
      parse_request();
      return state_;
    }
    if (!payload_.empty() && payload_.back() == '\r') {
      state_ = FeedResult::ProtocolError;
      request_.reset();
      return state_;
    }
    if (payload_.size() == kMaximumPayloadBytes) {
      state_ = FeedResult::ProtocolError;
      request_.reset();
      return state_;
    }
    payload_.push_back(character);
  }
  return state_;
}

IpcFramePolicy::FeedResult IpcFramePolicy::on_eof(Clock::time_point now) {
  if (state_ != FeedResult::NeedMore) {
    return state_;
  }
  (void)now;
  state_ = FeedResult::ProtocolError;
  request_.reset();
  return state_;
}

const std::optional<IpcRequest> &IpcFramePolicy::request() const noexcept {
  return request_;
}

void IpcFramePolicy::parse_request() {
  if (payload_ == "GET_STATUS") {
    request_ = IpcRequest{IpcVerb::GetStatus, {}};
    return;
  }
  if (payload_ == "SET_STATUS 0") {
    request_ = IpcRequest{IpcVerb::SetStatus, "0"};
    return;
  }
  if (payload_ == "SET_STATUS 1") {
    request_ = IpcRequest{IpcVerb::SetStatus, "1"};
    return;
  }
  if (payload_ == "RELOAD") {
    request_ = IpcRequest{IpcVerb::Reload, {}};
    return;
  }
  constexpr std::string_view reload_prefix = "RELOAD ";
  if (payload_.starts_with(reload_prefix) &&
      payload_.size() > reload_prefix.size() &&
      payload_[reload_prefix.size()] != ' ' &&
      payload_[reload_prefix.size()] != '\t') {
    request_ =
        IpcRequest{IpcVerb::Reload, payload_.substr(reload_prefix.size())};
    return;
  }
  if (payload_ == "STATS") {
    request_ = IpcRequest{IpcVerb::Stats, {}};
    return;
  }
  if (payload_ == "SHUTDOWN") {
    request_ = IpcRequest{IpcVerb::Shutdown, {}};
  }
}

struct IpcTransport::Impl : public std::enable_shared_from_this<Impl> {
  enum class ClientPhase { Reading, AwaitingResponse, WritingResponse };

  struct ClientState {
    std::uint64_t id;
    int fd;
    IpcFramePolicy frame;
    ClientPhase phase = ClientPhase::Reading;
    std::string response;
    std::size_t response_offset = 0;
  };

  struct Dispatch {
    std::uint64_t client_id;
    IpcRequest request;
    bool close_after_dispatch;
  };

  Impl(int listener, std::string path, IpcCommandSink &sink,
       IpcClock &time_source, IpcTransportIo &transport_io,
       IpcFramePolicyFactory factory, FatalCallback on_fatal,
       std::optional<RuntimeArtifactFileIdentity> path_identity,
       std::size_t client_limit)
      : listener_fd{listener}, owned_path{std::move(path)}, command_sink{sink},
        clock{time_source}, io{transport_io},
        policy_factory{std::move(factory)}, fatal_callback{std::move(on_fatal)},
        owned_path_identity{path_identity.has_value()
                                ? path_identity
                                : io.path_identity(owned_path)},
        max_clients{client_limit} {}

  ~Impl() = default;

  std::vector<ClientState>::iterator
  find_client_by_id_locked(std::uint64_t client_id) {
    return std::find_if(clients.begin(), clients.end(),
                        [client_id](const ClientState &client) {
                          return client.id == client_id;
                        });
  }

  std::vector<ClientState>::iterator find_client_by_fd_locked(int fd) {
    return std::find_if(
        clients.begin(), clients.end(),
        [fd](const ClientState &client) { return client.fd == fd; });
  }

  void close_client_locked(std::vector<ClientState>::iterator client) {
    (void)io.close_fd(client->fd);
    clients.erase(client);
  }

  void close_client_by_id_locked(std::uint64_t client_id) {
    const auto client = find_client_by_id_locked(client_id);
    if (client != clients.end()) {
      close_client_locked(client);
    }
  }

  void close_all_clients_locked() {
    for (const ClientState &client : clients) {
      (void)io.close_fd(client.fd);
    }
    clients.clear();
  }

  void close_listener_locked() {
    if (listener_fd >= 0) {
      (void)io.close_fd(listener_fd);
      listener_fd = -1;
    }
  }

  bool unlink_owned_path_locked() {
    if (owned_path.empty()) {
      return true;
    }
    if (!owned_path_identity.has_value()) {
      owned_path.clear();
      return true;
    }
    errno = 0;
    const auto current_identity = io.path_identity(owned_path);
    const int identity_error = errno;
    if (!current_identity.has_value()) {
      if (identity_error != ENOENT) {
        return false;
      }
      owned_path.clear();
      owned_path_identity.reset();
      return true;
    }
    if (*owned_path_identity != *current_identity) {
      owned_path.clear();
      owned_path_identity.reset();
      return true;
    }
    if (io.unlink_path(owned_path) != 0 && errno != ENOENT) {
      return false;
    }
    owned_path.clear();
    owned_path_identity.reset();
    return true;
  }

  void mark_fatal_locked(IpcFatalReason reason) {
    if (fatal) {
      return;
    }
    fatal = true;
    running = false;
    if (reason == IpcFatalReason::ShutdownTimeout) {
      shutdown_result = IpcStopResult::TimedOut;
    }
    pending_fatal = reason;
  }

  void latch_fatal_locked(IpcFatalReason reason) {
    if (fatal) {
      return;
    }
    mark_fatal_locked(reason);
    close_all_clients_locked();
    close_listener_locked();
    (void)unlink_owned_path_locked();
  }

  void deliver_pending_fatal() {
    IpcFatalReason reason = IpcFatalReason::InternalFailure;
    FatalCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!pending_fatal.has_value()) {
        return;
      }
      reason = *pending_fatal;
      pending_fatal.reset();
      callback = std::move(fatal_callback);
    }
    if (callback) {
      try {
        callback(reason);
      } catch (...) {
        // Fatal transport cleanup is already complete. A reporting callback
        // must not turn a controlled daemon failure into std::terminate().
        (void)0;
      }
    }
  }

  void fail_completion_noexcept() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!active || fatal) {
          return;
        }
        latch_fatal_locked(IpcFatalReason::InternalFailure);
        io.wake();
      }
      deliver_pending_fatal();
    } catch (...) {
      // The fatal latch is set before best-effort descriptor/path cleanup.
      (void)0;
    }
  }

  void complete(std::uint64_t client_id, const IpcResult &result) noexcept {
    try {
      std::string response = format_response(result);
      std::lock_guard<std::mutex> lock(mutex);
      if (!active || fatal) {
        return;
      }
      const auto client = find_client_by_id_locked(client_id);
      if (client == clients.end() ||
          client->phase != ClientPhase::AwaitingResponse) {
        return;
      }
      client->response = std::move(response);
      client->response_offset = 0;
      client->phase = ClientPhase::WritingResponse;
      io.wake();
    } catch (...) {
      fail_completion_noexcept();
    }
  }

  void expire_readers_locked(Clock::time_point now) {
    for (auto client = clients.begin(); client != clients.end();) {
      if (client->phase == ClientPhase::Reading &&
          client->frame.on_time(now) ==
              IpcFramePolicy::DeadlineResult::Expired) {
        (void)io.close_fd(client->fd);
        client = clients.erase(client);
      } else {
        ++client;
      }
    }
  }

  void finish_clean_shutdown_if_ready_locked() {
    if (!shutting_down || !clients.empty() || fatal) {
      return;
    }
    close_listener_locked();
    if (!unlink_owned_path_locked()) {
      mark_fatal_locked(IpcFatalReason::InternalFailure);
      return;
    }
    shutdown_result = IpcStopResult::Clean;
    running = false;
  }

  void prepare_shutdown_locked(Clock::time_point now) {
    if (!shutting_down || fatal) {
      return;
    }

    close_listener_locked();
    accept_retry_at.reset();
    for (auto client = clients.begin(); client != clients.end();) {
      if (client->phase == ClientPhase::Reading) {
        (void)io.close_fd(client->fd);
        client = clients.erase(client);
      } else {
        ++client;
      }
    }

    if (!shutdown_started_at.has_value()) {
      return;
    }
    if (now > *shutdown_started_at + kShutdownTimeout) {
      latch_fatal_locked(IpcFatalReason::ShutdownTimeout);
      return;
    }
    finish_clean_shutdown_if_ready_locked();
  }

  int poll_timeout_locked(Clock::time_point now) const {
    std::optional<Clock::time_point> nearest;
    const auto consider = [&nearest](Clock::time_point candidate) {
      if (!nearest.has_value() || candidate < *nearest) {
        nearest = candidate;
      }
    };

    for (const ClientState &client : clients) {
      if (client.phase == ClientPhase::Reading) {
        consider(client.frame.deadline());
      }
    }
    if (accept_retry_at.has_value()) {
      consider(*accept_retry_at);
    }
    if (shutting_down && shutdown_started_at.has_value() && !clients.empty()) {
      consider(*shutdown_started_at + kShutdownTimeout);
    }
    if (!nearest.has_value()) {
      return kIdlePollTimeoutMs;
    }

    const int timeout = ceil_milliseconds(*nearest - now);
    if (timeout == 0 && shutting_down && shutdown_started_at.has_value() &&
        now == *shutdown_started_at + kShutdownTimeout) {
      return 1;
    }
    return timeout;
  }

  std::vector<pollfd> descriptors_locked(Clock::time_point now) {
    std::vector<pollfd> descriptors;
    descriptors.reserve(clients.size() + 1U);
    if (listener_fd >= 0 && !shutting_down &&
        (!accept_retry_at.has_value() || now >= *accept_retry_at)) {
      descriptors.push_back({listener_fd, POLLIN, 0});
    }
    for (const ClientState &client : clients) {
      short events = 0;
      if (client.phase == ClientPhase::Reading) {
        events = POLLIN;
      } else if (client.phase == ClientPhase::WritingResponse) {
        events = POLLOUT;
      }
      descriptors.push_back({client.fd, events, 0});
    }
    return descriptors;
  }

  void handle_accept_error_locked(int error, Clock::time_point now) {
    if (is_accept_transient(error)) {
      accept_retry_at.reset();
      accept_backoff_index = 0;
      return;
    }
    if (is_accept_resource_error(error)) {
      const std::size_t index =
          std::min(accept_backoff_index, kAcceptBackoffs.size() - 1U);
      accept_retry_at = now + kAcceptBackoffs[index];
      if (accept_backoff_index + 1U < kAcceptBackoffs.size()) {
        ++accept_backoff_index;
      }
      return;
    }
    latch_fatal_locked(IpcFatalReason::AcceptFailure);
  }

  void accept_ready_client_locked(Clock::time_point now) {
    const int accepted = io.accept_client(listener_fd);
    if (accepted < 0) {
      handle_accept_error_locked(errno, now);
      return;
    }

    accept_retry_at.reset();
    accept_backoff_index = 0;
    if (clients.size() >= max_clients) {
      (void)io.close_fd(accepted);
      return;
    }
    const Clock::time_point accepted_at = clock.now();
    clients.push_back(ClientState{next_client_id++,
                                  accepted,
                                  policy_factory(accepted_at),
                                  ClientPhase::Reading,
                                  {},
                                  0});
  }

  std::optional<Dispatch> read_client_locked(std::uint64_t client_id,
                                             bool terminal_event,
                                             Clock::time_point) {
    auto client = find_client_by_id_locked(client_id);
    if (client == clients.end() || client->phase != ClientPhase::Reading) {
      return std::nullopt;
    }

    std::array<char, 256> buffer{};
    while (true) {
      const ssize_t received = io.receive(client->fd, buffer);
      const Clock::time_point received_at = clock.now();
      if (received > 0) {
        const auto count = static_cast<std::size_t>(received);
        const auto result = client->frame.feed(
            std::as_bytes(std::span{buffer.data(), count}), received_at);
        if (result == IpcFramePolicy::FeedResult::ProtocolError) {
          close_client_locked(client);
          return std::nullopt;
        }
        if (result == IpcFramePolicy::FeedResult::Complete) {
          const auto &request = client->frame.request();
          if (!request.has_value()) {
            client->response = "ERROR Unknown command\n";
            client->response_offset = 0;
            client->phase = ClientPhase::WritingResponse;
            return std::nullopt;
          }
          client->phase = ClientPhase::AwaitingResponse;
          return Dispatch{client->id, *request, terminal_event};
        }
        continue;
      }
      if (received == 0) {
        (void)client->frame.on_eof(received_at);
        close_client_locked(client);
        return std::nullopt;
      }

      const int error = errno;
      if (error == EINTR) {
        continue;
      }
      if (error == EAGAIN || error == EWOULDBLOCK) {
        if (terminal_event) {
          close_client_locked(client);
        }
        return std::nullopt;
      }
      close_client_locked(client);
      return std::nullopt;
    }
  }

  void flush_client_locked(std::uint64_t client_id) {
    auto client = find_client_by_id_locked(client_id);
    if (client == clients.end() ||
        client->phase != ClientPhase::WritingResponse) {
      return;
    }

    while (client->response_offset < client->response.size()) {
      const std::span<const char> remaining{
          client->response.data() + client->response_offset,
          client->response.size() - client->response_offset};
      const ssize_t sent = io.send(client->fd, remaining);
      if (sent > 0) {
        client->response_offset += static_cast<std::size_t>(sent);
        continue;
      }
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      }
      close_client_locked(client);
      return;
    }
    close_client_locked(client);
  }

  void flush_responses_locked() {
    std::vector<std::uint64_t> ready;
    ready.reserve(clients.size());
    for (const ClientState &client : clients) {
      if (client.phase == ClientPhase::WritingResponse) {
        ready.push_back(client.id);
      }
    }
    for (const std::uint64_t client_id : ready) {
      flush_client_locked(client_id);
    }
  }

  mutable std::mutex mutex;
  int listener_fd;
  std::string owned_path;
  IpcCommandSink &command_sink;
  IpcClock &clock;
  IpcTransportIo &io;
  IpcFramePolicyFactory policy_factory;
  FatalCallback fatal_callback;
  std::optional<RuntimeArtifactFileIdentity> owned_path_identity;
  const std::size_t max_clients;
  std::vector<ClientState> clients;
  std::optional<Clock::time_point> accept_retry_at;
  std::optional<Clock::time_point> shutdown_started_at;
  std::optional<IpcFatalReason> pending_fatal;
  std::size_t accept_backoff_index = 0;
  std::uint64_t next_client_id = 1;
  bool active = true;
  bool running = true;
  bool shutting_down = false;
  bool fatal = false;
  IpcStopResult shutdown_result = IpcStopResult::NotStopping;
};

IpcTransport::IpcTransport(
    int listener_fd, std::string owned_socket_path,
    IpcCommandSink &command_sink, IpcClock &clock, IpcTransportIo &io,
    IpcFramePolicyFactory policy_factory, FatalCallback fatal_callback,
    std::optional<RuntimeArtifactFileIdentity> owned_path_identity,
    std::size_t max_clients)
    : impl_{std::make_shared<Impl>(
          listener_fd, std::move(owned_socket_path), command_sink, clock, io,
          std::move(policy_factory), std::move(fatal_callback),
          owned_path_identity, max_clients)} {}

IpcTransport::~IpcTransport() {
  if (!impl_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->active = false;
    impl_->running = false;
    impl_->close_all_clients_locked();
    impl_->close_listener_locked();
    (void)impl_->unlink_owned_path_locked();
  }
  impl_.reset();
}

void IpcTransport::service_once() {
  const std::shared_ptr<Impl> state = impl_;
  if (!state) {
    return;
  }

  std::vector<pollfd> descriptors;
  int timeout_ms = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || !state->running) {
      return;
    }
    const Clock::time_point now = state->clock.now();
    state->prepare_shutdown_locked(now);
    state->expire_readers_locked(now);
    state->finish_clean_shutdown_if_ready_locked();
    if (!state->running) {
      // A shutdown timeout may have latched a fatal callback.
    } else {
      if (now >= state->accept_retry_at.value_or(Clock::time_point::max())) {
        state->accept_retry_at.reset();
      }
      descriptors = state->descriptors_locked(now);
      timeout_ms = state->poll_timeout_locked(now);
    }
  }
  state->deliver_pending_fatal();
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || !state->running) {
      return;
    }
  }

  const IpcPollResult poll_result = state->io.poll(descriptors, timeout_ms);
  const int poll_error = errno;
  std::vector<Impl::Dispatch> dispatches;

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || !state->running) {
      return;
    }

    const Clock::time_point now = state->clock.now();
    if (poll_result == IpcPollResult::Error) {
      if (poll_error != EINTR) {
        state->latch_fatal_locked(IpcFatalReason::PollFailure);
      }
    } else {
      state->expire_readers_locked(now);

      std::map<int, short> events;
      for (const pollfd &descriptor : descriptors) {
        if (descriptor.revents != 0) {
          events[descriptor.fd] = descriptor.revents;
        }
      }

      if (state->listener_fd >= 0) {
        const auto listener_event = events.find(state->listener_fd);
        if (listener_event != events.end()) {
          constexpr short fatal_listener_events =
              static_cast<short>(POLLERR | POLLHUP | POLLNVAL);
          if ((listener_event->second & fatal_listener_events) != 0) {
            state->latch_fatal_locked(IpcFatalReason::ListenerFailure);
          } else if (!state->shutting_down &&
                     (listener_event->second & POLLIN) != 0) {
            state->accept_ready_client_locked(now);
          }
        }
      }

      if (state->running) {
        std::vector<std::pair<std::uint64_t, short>> client_events;
        client_events.reserve(state->clients.size());
        for (const Impl::ClientState &client : state->clients) {
          const auto event = events.find(client.fd);
          if (event != events.end()) {
            client_events.emplace_back(client.id, event->second);
          }
        }

        constexpr short terminal_client_events =
            static_cast<short>(POLLERR | POLLHUP | POLLNVAL);
        for (const auto &[client_id, revents] : client_events) {
          auto client = state->find_client_by_id_locked(client_id);
          if (client == state->clients.end()) {
            continue;
          }

          const bool terminal = (revents & terminal_client_events) != 0;
          if (!state->shutting_down &&
              client->phase == Impl::ClientPhase::Reading &&
              (revents & POLLIN) != 0) {
            auto dispatch = state->read_client_locked(client_id, terminal, now);
            if (dispatch.has_value()) {
              dispatches.push_back(std::move(*dispatch));
            } else if (terminal) {
              client = state->find_client_by_id_locked(client_id);
              if (client != state->clients.end() &&
                  client->phase == Impl::ClientPhase::WritingResponse) {
                state->flush_client_locked(client_id);
              }
            }
            continue;
          }
          if (client->phase == Impl::ClientPhase::WritingResponse &&
              (revents & POLLOUT) != 0) {
            state->flush_client_locked(client_id);
            if (terminal) {
              state->close_client_by_id_locked(client_id);
            }
            continue;
          }
          if (terminal) {
            state->close_client_by_id_locked(client_id);
          }
        }
      }
      if (state->running) {
        state->prepare_shutdown_locked(state->clock.now());
      }
    }
  }

  state->deliver_pending_fatal();

  for (Impl::Dispatch &dispatch : dispatches) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->active || state->fatal) {
        break;
      }
    }
    const std::weak_ptr<Impl> weak_state{state};
    const std::uint64_t client_id = dispatch.client_id;
    IpcResponseCompletion complete = [weak_state,
                                      client_id](IpcResult result) noexcept {
      if (const auto locked = weak_state.lock()) {
        locked->complete(client_id, std::move(result));
      }
    };

    IpcEnqueueResult result = IpcEnqueueResult::Failed;
    try {
      result =
          state->command_sink(std::move(dispatch.request), std::move(complete));
    } catch (...) {
      result = IpcEnqueueResult::Failed;
    }

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->active || state->fatal) {
        continue;
      }
      if (result == IpcEnqueueResult::Failed) {
        state->latch_fatal_locked(IpcFatalReason::EnqueueFailure);
      } else if (result == IpcEnqueueResult::AdmissionClosed ||
                 dispatch.close_after_dispatch) {
        state->close_client_by_id_locked(client_id);
      }
    }
    state->deliver_pending_fatal();
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active || state->fatal) {
      return;
    }
    state->flush_responses_locked();
    const Clock::time_point now = state->clock.now();
    state->prepare_shutdown_locked(now);
    state->finish_clean_shutdown_if_ready_locked();
  }
  state->deliver_pending_fatal();
}

void IpcTransport::begin_shutdown() {
  const std::shared_ptr<Impl> state = impl_;
  if (!state) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->active || state->fatal || state->shutting_down) {
    return;
  }
  state->shutting_down = true;
  state->shutdown_started_at = state->clock.now();
  state->shutdown_result = IpcStopResult::InProgress;
  state->io.wake();
}

void IpcTransport::fail(IpcFatalReason reason) noexcept {
  const std::shared_ptr<Impl> state = impl_;
  if (!state) {
    return;
  }
  try {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->active) {
        return;
      }
      state->latch_fatal_locked(reason);
    }
    state->deliver_pending_fatal();
  } catch (...) {
    // POSIX cleanup is non-throwing. This boundary also protects callers from
    // injected test doubles or allocation failures while reporting a fatal.
    (void)0;
  }
}

bool IpcTransport::is_running() const noexcept {
  const std::shared_ptr<Impl> state = impl_;
  if (!state) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->active && state->running;
}

bool IpcTransport::fatal_latched() const noexcept {
  const std::shared_ptr<Impl> state = impl_;
  if (!state) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->fatal;
}

IpcStopResult IpcTransport::stop_result() const noexcept {
  const std::shared_ptr<Impl> state = impl_;
  if (!state) {
    return IpcStopResult::Clean;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->shutdown_result;
}

struct IpcServer::Impl {
  Impl(std::shared_ptr<IpcCommandMailbox> mailbox,
       IpcServerOptions server_options)
      : command_mailbox{std::move(mailbox)},
        command_sink{
            make_mailbox_sink(command_mailbox, server_options.endpoint_mode)},
        options{std::move(server_options)},
        owned_security{std::make_unique<PosixRuntimeArtifactSecurity>()},
        security{owned_security.get()} {}

  Impl(std::shared_ptr<IpcCommandMailbox> mailbox,
       IpcServerOptions server_options,
       RuntimeArtifactSecurity &injected_security)
      : command_mailbox{std::move(mailbox)},
        command_sink{
            make_mailbox_sink(command_mailbox, server_options.endpoint_mode)},
        options{std::move(server_options)}, security{&injected_security} {}

  std::shared_ptr<IpcCommandMailbox> command_mailbox;
  IpcCommandSink command_sink;
  IpcServerOptions options;
  std::unique_ptr<RuntimeArtifactSecurity> owned_security;
  RuntimeArtifactSecurity *security;
  std::unique_ptr<SteadyIpcClock> clock;
  std::unique_ptr<PosixIpcTransportIo> io;
  std::unique_ptr<SocketLease> runtime_socket_lease;
  std::unique_ptr<IpcTransport> transport;
  std::jthread server_thread;
  mutable std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_changed;
  IpcServerState lifecycle = IpcServerState::Stopped;
  std::thread::id poller_thread_id;
  bool join_in_progress = false;
  std::optional<IpcFatalReason> fatal_reason;

  void cleanup_runtime_locked() {
    transport.reset();
    runtime_socket_lease.reset();
    io.reset();
    clock.reset();
  }

  void signal_transport_fatal(IpcFatalReason reason) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (lifecycle != IpcServerState::Fatal) {
      lifecycle = IpcServerState::Fatal;
      fatal_reason = reason;
    }
    lifecycle_changed.notify_all();
  }
};

IpcServer::IpcServer(std::shared_ptr<IpcCommandMailbox> command_mailbox,
                     IpcServerOptions options)
    : impl_{std::make_shared<Impl>(std::move(command_mailbox),
                                   std::move(options))} {}

IpcServer::IpcServer(std::shared_ptr<IpcCommandMailbox> command_mailbox,
                     IpcServerOptions options,
                     RuntimeArtifactSecurity &security)
    : impl_{std::make_shared<Impl>(std::move(command_mailbox),
                                   std::move(options), security)} {}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start() {
  const std::shared_ptr<Impl> state = impl_;
  std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
  if (state->poller_thread_id == std::this_thread::get_id()) {
    return state->lifecycle == IpcServerState::Running;
  }
  state->lifecycle_changed.wait(lock, [&state]() {
    return state->lifecycle != IpcServerState::Stopping &&
           !state->join_in_progress;
  });
  if (state->lifecycle == IpcServerState::Fatal) {
    return false;
  }
  if (state->lifecycle == IpcServerState::Running) {
    return true;
  }
  if (!state->command_mailbox || !state->command_mailbox->is_open() ||
      state->security == nullptr) {
    return false;
  }

  state->lifecycle = IpcServerState::Starting;
  try {
    CreatedServerSocket created =
        create_server_socket(state->options, *state->security);
    if (created.socket.fd < 0 || !created.socket.identity.has_value() ||
        !created.lease) {
      state->cleanup_runtime_locked();
      state->lifecycle = IpcServerState::Stopped;
      state->lifecycle_changed.notify_all();
      return false;
    }

    struct PendingSocket {
      CreatedSocket socket;
      bool released = false;
      ~PendingSocket() {
        if (released || socket.fd < 0 || !socket.identity.has_value()) {
          return;
        }
        (void)::close(socket.fd);
        (void)unlink_socket_if_same(socket.path, *socket.identity);
      }
    } pending{std::move(created.socket)};

    state->clock = std::make_unique<SteadyIpcClock>();
    state->io = std::make_unique<PosixIpcTransportIo>();
    if (!state->io->valid()) {
      state->cleanup_runtime_locked();
      state->lifecycle = IpcServerState::Stopped;
      state->lifecycle_changed.notify_all();
      return false;
    }

    IpcFramePolicyFactory policy_factory{[](Clock::time_point accepted_at) {
      return IpcFramePolicy{accepted_at};
    }};
    const std::weak_ptr<Impl> weak_state{state};
    state->transport = std::make_unique<IpcTransport>(
        pending.socket.fd, pending.socket.path, state->command_sink,
        *state->clock, *state->io, std::move(policy_factory),
        [weak_state](IpcFatalReason reason) noexcept {
          if (const auto locked = weak_state.lock()) {
            locked->signal_transport_fatal(reason);
          }
        },
        pending.socket.identity, state->options.max_clients);
    pending.released = true;
    state->runtime_socket_lease = std::move(created.lease);

    std::jthread poller([state](std::stop_token stop_token) {
      {
        std::lock_guard<std::mutex> state_lock(state->lifecycle_mutex);
        state->poller_thread_id = std::this_thread::get_id();
        state->lifecycle_changed.notify_all();
      }

      try {
        while (state->transport->is_running()) {
          if (stop_token.stop_requested()) {
            state->transport->begin_shutdown();
          }
          state->transport->service_once();
        }
      } catch (...) {
        state->transport->fail(IpcFatalReason::InternalFailure);
      }

      {
        std::lock_guard<std::mutex> state_lock(state->lifecycle_mutex);
        if (state->transport->fatal_latched() &&
            state->lifecycle != IpcServerState::Fatal) {
          state->lifecycle = IpcServerState::Fatal;
          state->fatal_reason = IpcFatalReason::InternalFailure;
        }
        state->lifecycle_changed.notify_all();
      }

      {
        std::lock_guard<std::mutex> state_lock(state->lifecycle_mutex);
        state->poller_thread_id = {};
        state->lifecycle_changed.notify_all();
      }
    });
    state->server_thread = std::move(poller);
    state->lifecycle = IpcServerState::Running;
    state->lifecycle_changed.notify_all();
    return true;
  } catch (...) {
    state->cleanup_runtime_locked();
    state->lifecycle = IpcServerState::Fatal;
    state->fatal_reason = IpcFatalReason::InternalFailure;
    state->lifecycle_changed.notify_all();
    return false;
  }
}

void IpcServer::stop() {
  const std::shared_ptr<Impl> state = impl_;
  std::jthread joined_thread;
  IpcServerState terminal_state = IpcServerState::Stopped;
  {
    std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
    if (state->poller_thread_id == std::this_thread::get_id()) {
      if (state->lifecycle == IpcServerState::Running) {
        state->lifecycle = IpcServerState::Stopping;
        if (state->transport) {
          state->transport->begin_shutdown();
        }
        if (state->server_thread.joinable()) {
          state->server_thread.request_stop();
        }
      }
      return;
    }

    state->lifecycle_changed.wait(lock, [&state]() {
      return state->lifecycle != IpcServerState::Starting;
    });
    if (state->lifecycle == IpcServerState::Stopped) {
      state->cleanup_runtime_locked();
      return;
    }
    if (state->join_in_progress) {
      state->lifecycle_changed.wait(
          lock, [&state]() { return !state->join_in_progress; });
      return;
    }

    terminal_state = state->lifecycle == IpcServerState::Fatal
                         ? IpcServerState::Fatal
                         : IpcServerState::Stopped;
    if (state->lifecycle == IpcServerState::Running) {
      state->lifecycle = IpcServerState::Stopping;
    }
    if (state->transport) {
      state->transport->begin_shutdown();
    }
    if (state->server_thread.joinable()) {
      state->server_thread.request_stop();
      joined_thread = std::move(state->server_thread);
      state->join_in_progress = true;
    } else if (state->poller_thread_id != std::thread::id{}) {
      state->lifecycle_changed.wait(lock, [&state]() {
        return state->poller_thread_id == std::thread::id{};
      });
    }
  }

  if (joined_thread.joinable()) {
    joined_thread.join();
  }

  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  state->cleanup_runtime_locked();
  state->join_in_progress = false;
  if (state->lifecycle != IpcServerState::Fatal) {
    state->lifecycle = terminal_state;
  }
  state->lifecycle_changed.notify_all();
}

bool IpcServer::is_running() const noexcept {
  const std::shared_ptr<Impl> state = impl_;
  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  return state->lifecycle == IpcServerState::Running;
}

IpcServerState IpcServer::lifecycle_state() const noexcept {
  const std::shared_ptr<Impl> state = impl_;
  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  return state->lifecycle;
}

std::optional<IpcFatalReason> IpcServer::fatal_reason() const noexcept {
  const std::shared_ptr<Impl> state = impl_;
  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  return state->fatal_reason;
}

} // namespace punto
