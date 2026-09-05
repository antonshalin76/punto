/**
 * @file x11_session.hpp
 * @brief Safe discovery and access for the active local X11 session.
 */

#pragma once

#include <xcb/xcb.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace punto {

class ClipboardManager;

struct X11SessionInfo {
  std::string session_id;
  std::string username;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::vector<std::uint32_t> supplementary_groups;
  std::string display;
  std::string xauthority_path;
  std::string home_dir;
  std::string xdg_runtime_dir;
  std::string xdg_config_home;
  std::string wayland_display;
  int observed_keyboard_layout = -1;
};

namespace x11_detail {

enum class ProbeStatus {
  Healthy,
  SessionAbsent,
  Failed,
};

enum class XcbOperationResult {
  Success,
  TimedOut,
  ConnectionFailed,
  ProtocolError,
};

struct ProbeResult {
  ProbeStatus status = ProbeStatus::Failed;
  X11SessionInfo info;
};

inline constexpr std::size_t kMaxEnvironmentBytes = 64U * 1024U;
inline constexpr std::size_t kMaxProcCandidates = 4096U;
inline constexpr std::size_t kMaxPasswdBufferBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxXauthorityBytes = 1024U * 1024U;

struct XauthorityMetadata {
  std::uint32_t owner_uid = 0;
  std::uint32_t mode = 0;
  std::int64_t size = -1;
};

[[nodiscard]] bool is_valid_local_display(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_wayland_display(std::string_view value) noexcept;
[[nodiscard]] bool xauthority_metadata_is_trusted(
    const XauthorityMetadata &metadata,
    std::uint32_t expected_uid) noexcept;
[[nodiscard]] std::optional<std::chrono::milliseconds>
retry_delay_after_failure(std::size_t failure_count) noexcept;

} // namespace x11_detail

/**
 * Owns one XCB connection and applies absolute deadlines to reply-bearing and
 * checked-void requests. Any timeout, transport error, or protocol error
 * closes the connection before returning.
 */
class BoundedXcbConnection {
public:
  BoundedXcbConnection() noexcept = default;
  ~BoundedXcbConnection();

  BoundedXcbConnection(const BoundedXcbConnection &) = delete;
  BoundedXcbConnection &operator=(const BoundedXcbConnection &) = delete;
  BoundedXcbConnection(BoundedXcbConnection &&other) noexcept;
  BoundedXcbConnection &operator=(BoundedXcbConnection &&other) noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] xcb_connection_t *get() const noexcept;
  [[nodiscard]] int screen_number() const noexcept;

  /** Caller owns the successful reply and must free it with std::free(). */
  [[nodiscard]] void *
  wait_for_reply(std::uint32_t sequence,
                 std::chrono::steady_clock::time_point deadline,
                 x11_detail::XcbOperationResult &result) noexcept;

  [[nodiscard]] bool
  check_request(xcb_void_cookie_t cookie,
                std::chrono::steady_clock::time_point deadline,
                x11_detail::XcbOperationResult &result) noexcept;

  void close() noexcept;

private:
  friend class ClipboardManager;
  friend class X11Session;
  explicit BoundedXcbConnection(xcb_connection_t *connection,
                                int screen_number) noexcept;
  [[nodiscard]] void *poll_reply(
      std::uint32_t sequence, std::chrono::steady_clock::time_point deadline,
      bool allow_null_reply, x11_detail::XcbOperationResult &result) noexcept;

  xcb_connection_t *connection_ = nullptr;
  int screen_number_ = -1;
};

class X11Session {
  struct WriteGate;

public:
  enum class RefreshResult {
    HealthyUnchanged,
    HealthyUpdated,
    SessionAbsent,
    Failed,
  };

  struct KeyboardObservation {
    std::uint64_t request_id = 0;
    std::uint64_t session_generation = 0;
    int group = -1;
    std::uint32_t focus_window = 0;
    std::uint8_t locked_mods = 0;
  };

  using ProbeFunction = std::function<x11_detail::ProbeResult()>;
  using RetryWaitFunction = std::function<bool(
      std::chrono::milliseconds, const std::atomic<bool> &cancel_requested)>;

  /**
   * Pins one committed session generation for an entire desktop write
   * transaction. Revocation waits for all acquired leases before it commits.
   */
  class WriteLease {
  public:
    WriteLease(WriteLease &&) noexcept = default;
    WriteLease &operator=(WriteLease &&) noexcept = delete;
    WriteLease(const WriteLease &) = delete;
    WriteLease &operator=(const WriteLease &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const X11SessionInfo &info() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] BoundedXcbConnection open_bounded_connection(
        std::chrono::steady_clock::time_point deadline) const;
    [[nodiscard]] BoundedXcbConnection open_bounded_connection(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{
            250}) const;

  private:
    friend class X11Session;
    WriteLease(std::shared_ptr<WriteGate> gate,
               std::unique_lock<std::recursive_mutex> lock,
               X11SessionInfo info, std::uint64_t generation) noexcept;

    std::shared_ptr<WriteGate> gate_;
    std::unique_lock<std::recursive_mutex> lock_;
    X11SessionInfo info_;
    std::uint64_t generation_ = 0;
  };

  explicit X11Session(ProbeFunction probe_function = {},
                      RetryWaitFunction retry_wait_function = {});
  ~X11Session();

  X11Session(const X11Session &) = delete;
  X11Session &operator=(const X11Session &) = delete;

  [[nodiscard]] bool initialize();
  [[nodiscard]] RefreshResult refresh();

  bool start_background_refresh();
  [[nodiscard]] std::optional<RefreshResult> poll_refresh_result();
  [[nodiscard]] bool
  start_background_keyboard_observation(std::uint64_t request_id);
  [[nodiscard]] std::optional<KeyboardObservation> poll_keyboard_observation();

  /**
   * Requests cancellation and waits no longer than timeout. A stuck system
   * discovery call is detached with shared-only state, so destruction remains
   * bounded and cannot access this object afterwards.
   */
  [[nodiscard]] bool shutdown_background_refresh(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{
          3000}) noexcept;

  void reset() noexcept;
  [[nodiscard]] bool is_valid() const noexcept;
  [[nodiscard]] X11SessionInfo info() const;
  [[nodiscard]] bool is_wayland_session() const;

  [[nodiscard]] std::optional<WriteLease> acquire_write_lease() const;

  [[nodiscard]] int get_current_keyboard_layout() const;

private:
  struct BackgroundState {
    enum class Kind { Discovery, Keyboard };
    Kind kind = Kind::Discovery;
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> cancel_requested{false};
    bool done = false;
    std::uint64_t generation = 0;
    x11_detail::ProbeResult probe;
    KeyboardObservation keyboard;
    X11SessionInfo keyboard_session;
    ProbeFunction probe_function;
    RetryWaitFunction retry_wait_function;
    std::shared_ptr<WriteGate> write_gate;
    std::shared_ptr<std::atomic<std::uint64_t>> generation_clock;
  };

  [[nodiscard]] static x11_detail::ProbeResult probe_once();
  [[nodiscard]] static BoundedXcbConnection open_bounded_connection_for(
      const X11SessionInfo &info,
      std::chrono::steady_clock::time_point deadline) noexcept;
  static void
  run_background_probe(const std::shared_ptr<BackgroundState> &state) noexcept;
  [[nodiscard]] RefreshResult commit_probe(std::uint64_t generation,
                                           x11_detail::ProbeResult probe);

  mutable std::mutex mu_;
  X11SessionInfo info_;
  std::atomic<bool> initialized_{false};

  mutable std::mutex refresh_mutex_;
  std::shared_ptr<BackgroundState> pending_refresh_;
  std::thread refresh_thread_;
  std::shared_ptr<std::atomic<std::uint64_t>> generation_clock_;
  ProbeFunction probe_function_;
  RetryWaitFunction retry_wait_function_;
  std::shared_ptr<WriteGate> write_gate_;
};

} // namespace punto
