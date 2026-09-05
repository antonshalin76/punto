/**
 * @file ipc_server.hpp
 * @brief Bounded Unix-domain IPC transport for the Punto daemon.
 */

#pragma once

#include <poll.h>
#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace punto {

inline constexpr const char *kIpcSocketPath = "/var/run/punto.sock";
inline constexpr std::size_t kIpcMaxConcurrentClients = 32;

enum class IpcVerb { GetStatus, SetStatus, Reload, Stats, Shutdown };

struct IpcRequest {
  IpcVerb verb;
  std::string argument;
};

struct IpcResult {
  bool success = false;
  std::string message;
};

#if defined(PUNTO_IPC_INTERNAL_TESTING)
class IpcFramePolicy {
public:
  using Clock = std::chrono::steady_clock;

  enum class FeedResult { NeedMore, Complete, ProtocolError };
  enum class DeadlineResult { OnTime, Expired };

  explicit IpcFramePolicy(Clock::time_point accepted_at);

  [[nodiscard]] Clock::time_point accepted_at() const noexcept;
  [[nodiscard]] Clock::time_point deadline() const noexcept;
  [[nodiscard]] DeadlineResult on_time(Clock::time_point now) const noexcept;

  FeedResult feed(std::span<const std::byte> bytes, Clock::time_point now);
  FeedResult on_eof(Clock::time_point now);

  [[nodiscard]] const std::optional<IpcRequest> &request() const noexcept;

private:
  void parse_request();

  Clock::time_point accepted_at_;
  std::string payload_;
  std::optional<IpcRequest> request_;
  FeedResult state_ = FeedResult::NeedMore;
};
#endif

using IpcResponseCompletion = std::function<void(IpcResult)>;

enum class IpcEnqueueResult { Accepted, AdmissionClosed, Failed };

struct IpcPendingCommand {
  IpcRequest request;
  IpcResponseCompletion complete;
};

class IpcCommandMailbox {
public:
  explicit IpcCommandMailbox(std::size_t capacity = kIpcMaxConcurrentClients);
  ~IpcCommandMailbox();

  IpcCommandMailbox(const IpcCommandMailbox &) = delete;
  IpcCommandMailbox &operator=(const IpcCommandMailbox &) = delete;

  // One IpcServer producer and one owner-side consumer may use the mailbox
  // concurrently. Neither operation invokes owner code or waits for it.
  IpcEnqueueResult try_enqueue(IpcRequest request,
                               IpcResponseCompletion complete) noexcept;
  [[nodiscard]] std::optional<IpcPendingCommand> try_dequeue() noexcept;
  [[nodiscard]] bool
  close(std::chrono::milliseconds timeout = std::chrono::seconds{3}) noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  // Owner-side observation only; never drains or completes queued requests.
  [[nodiscard]] bool has_pending_mutation() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#if defined(PUNTO_IPC_MAILBOX_TESTING)
using IpcMailboxProducerTestHook = void (*)() noexcept;
void set_ipc_mailbox_producer_test_hook(
    IpcMailboxProducerTestHook hook) noexcept;
void set_ipc_mailbox_admitted_test_hook(
    IpcMailboxProducerTestHook hook) noexcept;
#endif

enum class IpcFatalReason {
  PollFailure,
  ListenerFailure,
  AcceptFailure,
  EnqueueFailure,
  ShutdownTimeout,
  InternalFailure,
};

#if defined(PUNTO_IPC_INTERNAL_TESTING)
// Private deterministic transport seam. Only ipc_server.cpp and its contract
// test define PUNTO_IPC_INTERNAL_TESTING; daemon-facing code cannot install
// arbitrary callbacks on the poller.
using IpcCommandSink =
    std::function<IpcEnqueueResult(IpcRequest, IpcResponseCompletion)>;
using IpcFramePolicyFactory =
    std::function<IpcFramePolicy(std::chrono::steady_clock::time_point)>;

class IpcClock {
public:
  virtual ~IpcClock() = default;
  [[nodiscard]] virtual std::chrono::steady_clock::time_point
  now() const noexcept = 0;
};

enum class IpcPollResult { Ready, Timeout, Wakeup, Error };

struct RuntimeArtifactFileIdentity {
  dev_t device;
  ino_t inode;

  friend bool operator==(const RuntimeArtifactFileIdentity &,
                         const RuntimeArtifactFileIdentity &) = default;
};

class IpcTransportIo {
public:
  virtual ~IpcTransportIo() = default;

  virtual IpcPollResult poll(std::span<pollfd> descriptors, int timeout_ms) = 0;
  virtual void wake() noexcept = 0;
  virtual int accept_client(int listener_fd) = 0;
  virtual ssize_t receive(int fd, std::span<char> destination) = 0;
  virtual ssize_t send(int fd, std::span<const char> source) = 0;
  virtual int close_fd(int fd) = 0;
  virtual int unlink_path(std::string_view path) = 0;
  [[nodiscard]] virtual std::optional<RuntimeArtifactFileIdentity>
  path_identity(std::string_view path) = 0;
};

enum class IpcStopResult { NotStopping, InProgress, Clean, TimedOut };

class IpcTransport {
public:
  // This low-level observer is a signal-only test/integration seam. It must
  // never wait for owner work; IpcServer installs only its internal FSM latch.
  using FatalCallback = std::function<void(IpcFatalReason)>;

  IpcTransport(int listener_fd, std::string owned_socket_path,
               IpcCommandSink &command_sink, IpcClock &clock,
               IpcTransportIo &io, IpcFramePolicyFactory policy_factory,
               FatalCallback fatal_callback,
               std::optional<RuntimeArtifactFileIdentity> owned_path_identity =
                   std::nullopt,
               std::size_t max_clients = kIpcMaxConcurrentClients);
  ~IpcTransport();

  IpcTransport(const IpcTransport &) = delete;
  IpcTransport &operator=(const IpcTransport &) = delete;

  void service_once();
  void begin_shutdown();
  void fail(IpcFatalReason reason) noexcept;

  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] bool fatal_latched() const noexcept;
  [[nodiscard]] IpcStopResult stop_result() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
#endif

struct RuntimeArtifactIdentity {
  uid_t uid;
  gid_t gid;
  mode_t mode;
};

enum class IpcEndpointMode {
  Control,
  DiagnosticReadOnly,
};

class RuntimeArtifactSecurity {
public:
  enum class Result { Secured, ChmodFailed, ChownFailed };

  virtual ~RuntimeArtifactSecurity() = default;
  virtual Result secure_socket(int listener_fd, std::string_view path,
                               RuntimeArtifactIdentity intended_identity) = 0;
};

struct IpcServerOptions {
  std::string primary_socket_path{kIpcSocketPath};
  bool allow_fallback_sockets = false;
  IpcEndpointMode endpoint_mode = IpcEndpointMode::Control;
  RuntimeArtifactIdentity socket_identity{
      static_cast<uid_t>(0), static_cast<gid_t>(-1), static_cast<mode_t>(0660)};
  std::size_t max_clients = kIpcMaxConcurrentClients;
};

enum class IpcServerState { Stopped, Starting, Running, Stopping, Fatal };

class IpcServer {
public:
  IpcServer(std::shared_ptr<IpcCommandMailbox> command_mailbox,
            IpcServerOptions options = {});
  IpcServer(std::shared_ptr<IpcCommandMailbox> command_mailbox,
            IpcServerOptions options, RuntimeArtifactSecurity &security);
  ~IpcServer();

  IpcServer(const IpcServer &) = delete;
  IpcServer &operator=(const IpcServer &) = delete;

  bool start();
  void stop();
  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] IpcServerState lifecycle_state() const noexcept;
  [[nodiscard]] std::optional<IpcFatalReason> fatal_reason() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace punto
