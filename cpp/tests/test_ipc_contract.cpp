#define PUNTO_IPC_INTERNAL_TESTING 1
#include "punto/ipc_server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using FeedResult = punto::IpcFramePolicy::FeedResult;
using DeadlineResult = punto::IpcFramePolicy::DeadlineResult;

std::atomic<bool> g_mailbox_hook_entered{false};
std::atomic<bool> g_release_mailbox_hook{false};

void hold_mailbox_producer() noexcept {
  g_mailbox_hook_entered.store(true, std::memory_order_release);
  while (!g_release_mailbox_hook.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error(message);
}

void expect(bool condition, const std::string &message) {
  if (!condition) {
    fail(message);
  }
}

int poll_timeout_until(Clock::time_point deadline) {
  const Clock::time_point now = Clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
}

bool read_exact_until(int fd, std::span<std::byte> destination,
                      Clock::time_point deadline) {
  std::size_t offset = 0;
  while (offset < destination.size()) {
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, poll_timeout_until(deadline));
    if (ready == 0) {
      return false;
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    const ssize_t received =
        ::read(fd, destination.data() + offset, destination.size() - offset);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

template <typename Value>
bool read_value_until(int fd, Value &value, Clock::time_point deadline) {
  return read_exact_until(
      fd, std::as_writable_bytes(std::span<Value>{&value, 1}), deadline);
}

bool write_exact_until(int fd, std::span<const std::byte> source,
                       Clock::time_point deadline) {
  std::size_t offset = 0;
  while (offset < source.size()) {
    pollfd descriptor{fd, POLLOUT, 0};
    const int ready = ::poll(&descriptor, 1, poll_timeout_until(deadline));
    if (ready == 0) {
      return false;
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    const ssize_t written =
        ::write(fd, source.data() + offset, source.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

template <typename Value>
bool write_value_until(int fd, const Value &value, Clock::time_point deadline) {
  return write_exact_until(fd, std::as_bytes(std::span<const Value>{&value, 1}),
                           deadline);
}

bool waitpid_until(pid_t child, int &status, Clock::time_point deadline) {
  while (Clock::now() < deadline) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      return true;
    }
    if (waited < 0 && errno != EINTR) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  (void)::kill(child, SIGKILL);
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  return false;
}

void close_descriptors_except(std::span<const int> retained) noexcept {
  for (int fd = 3; fd < 4096; ++fd) {
    if (std::find(retained.begin(), retained.end(), fd) == retained.end()) {
      (void)::close(fd);
    }
  }
}

std::span<const std::byte> bytes_of(std::string_view value) {
  return std::as_bytes(std::span{value.data(), value.size()});
}

const punto::IpcRequest &
expect_typed_request(const punto::IpcFramePolicy &policy, punto::IpcVerb verb,
                     std::string_view argument, std::string_view context) {
  const auto &request = policy.request();
  expect(request.has_value(), std::string{context} + ": no typed request");
  expect(request->verb == verb, std::string{context} + ": wrong typed verb");
  expect(request->argument == argument,
         std::string{context} + ": wrong typed argument");
  return *request;
}

void test_frame_policy_deadline_boundary_uses_fresh_instances() {
  const Clock::time_point accepted_at{};

  punto::IpcFramePolicy before_boundary(accepted_at);
  expect(before_boundary.accepted_at() == accepted_at,
         "policy did not retain accepted_at");
  expect(before_boundary.deadline() == accepted_at + 250ms,
         "deadline is not accepted_at + 250 ms");
  expect(before_boundary.on_time(accepted_at + 249ms) == DeadlineResult::OnTime,
         "fresh policy expired at t+249 ms");
  expect(before_boundary.feed(bytes_of("GET_STATUS\n"), accepted_at + 249ms) ==
             FeedResult::Complete,
         "frame completed at t+249 ms was rejected");
  expect_typed_request(before_boundary, punto::IpcVerb::GetStatus, "",
                       "t+249 ms frame");

  punto::IpcFramePolicy at_boundary(accepted_at);
  expect(at_boundary.on_time(accepted_at + 250ms) == DeadlineResult::Expired,
         "fresh policy remained on time at t+250 ms");
  expect(at_boundary.feed(bytes_of("GET_STATUS\n"), accepted_at + 250ms) ==
             FeedResult::ProtocolError,
         "frame completed at the exact expiry boundary was accepted");
  expect(!at_boundary.request().has_value(),
         "expired frame produced a typed request");
}

struct AcceptedFrame {
  std::string name;
  std::vector<std::string> chunks;
  punto::IpcVerb verb;
  std::string argument;
};

void test_documented_command_grammar() {
  const std::vector<AcceptedFrame> cases{
      {"GET_STATUS", {"GET_", "STATUS\n"}, punto::IpcVerb::GetStatus, ""},
      {"GET_STATUS CRLF",
       {"GET_STATUS\r", "\n"},
       punto::IpcVerb::GetStatus,
       ""},
      {"SET_STATUS 0", {"SET_STATUS 0\n"}, punto::IpcVerb::SetStatus, "0"},
      {"SET_STATUS 1", {"SET_STATUS ", "1\n"}, punto::IpcVerb::SetStatus, "1"},
      {"RELOAD", {"RELOAD\n"}, punto::IpcVerb::Reload, ""},
      {"RELOAD allowed path",
       {"RELOAD /etc/punto/config.yaml\n"},
       punto::IpcVerb::Reload,
       "/etc/punto/config.yaml"},
      {"STATS", {"STATS\n"}, punto::IpcVerb::Stats, ""},
      {"SHUTDOWN", {"SHUT", "DOWN\n"}, punto::IpcVerb::Shutdown, ""},
  };

  for (const AcceptedFrame &test_case : cases) {
    const Clock::time_point accepted_at{};
    punto::IpcFramePolicy policy(accepted_at);
    for (std::size_t index = 0; index < test_case.chunks.size(); ++index) {
      const FeedResult expected = index + 1 == test_case.chunks.size()
                                      ? FeedResult::Complete
                                      : FeedResult::NeedMore;
      expect(policy.feed(bytes_of(test_case.chunks[index]),
                         accepted_at + 1ms) == expected,
             test_case.name + ": unexpected feed result");
    }
    expect_typed_request(policy, test_case.verb, test_case.argument,
                         test_case.name);
  }
}

void test_framing_valid_unknown_commands_have_no_typed_request() {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"empty frame", "\n"},
      {"unknown verb", "ENABLE\n"},
      {"suffixed GET_STATUS", "GET_STATUS_JUNK\n"},
      {"suffixed SET_STATUS", "SET_STATUS_JUNK 0\n"},
      {"suffixed RELOAD", "RELOAD_JUNK /etc/punto/config.yaml\n"},
      {"suffixed STATS", "STATS_JUNK\n"},
      {"suffixed SHUTDOWN", "SHUTDOWN_JUNK\n"},
      {"argument on GET_STATUS", "GET_STATUS 1\n"},
      {"missing SET_STATUS argument", "SET_STATUS\n"},
      {"undocumented SET_STATUS true", "SET_STATUS true\n"},
      {"invalid SET_STATUS value", "SET_STATUS 2\n"},
      {"extra SET_STATUS argument", "SET_STATUS 0 extra\n"},
      {"argument on STATS", "STATS now\n"},
      {"argument on SHUTDOWN", "SHUTDOWN now\n"},
  };

  for (const auto &[name, frame] : cases) {
    punto::IpcFramePolicy policy(Clock::time_point{});
    expect(policy.feed(bytes_of(frame), Clock::time_point{} + 1ms) ==
               FeedResult::Complete,
           name + ": framing-valid unknown command was a protocol error");
    expect(!policy.request().has_value(),
           name + ": unknown command produced a typed request");
  }
}

void test_invalid_frame_matrix_and_fragmented_limit() {
  std::string nul = "RELOAD /etc/punto/config.yaml";
  nul.push_back('\0');
  nul.push_back('\n');

  const std::vector<std::pair<std::string, std::string>> cases{
      {"misplaced CR", "GET_\rSTATUS\n"},
      {"NUL payload", std::move(nul)},
  };

  for (const auto &[name, frame] : cases) {
    punto::IpcFramePolicy policy(Clock::time_point{});
    expect(policy.feed(bytes_of(frame), Clock::time_point{} + 1ms) ==
               FeedResult::ProtocolError,
           name + ": invalid frame was not rejected");
    expect(!policy.request().has_value(),
           name + ": invalid frame produced a typed request");
    expect(policy.feed(bytes_of("GET_STATUS\n"), Clock::time_point{} + 2ms) ==
               FeedResult::ProtocolError,
           name + ": protocol-error state was not terminal");
  }

  punto::IpcFramePolicy eof_policy(Clock::time_point{});
  expect(eof_policy.feed(bytes_of("GET_STATUS"), Clock::time_point{} + 1ms) ==
             FeedResult::NeedMore,
         "unterminated frame was not retained before EOF");
  expect(eof_policy.on_eof(Clock::time_point{} + 2ms) ==
             FeedResult::ProtocolError,
         "EOF before LF was not a protocol error");
  expect(!eof_policy.request().has_value(),
         "EOF-before-LF frame produced a typed request");

  std::string maximum = "RELOAD /etc/punto/";
  maximum.append(254U - maximum.size(), 'a');
  const std::string maximum_argument =
      maximum.substr(std::string{"RELOAD "}.size());
  punto::IpcFramePolicy maximum_policy(Clock::time_point{});
  expect(maximum_policy.feed(bytes_of(maximum.substr(0, 127)),
                             Clock::time_point{} + 1ms) == FeedResult::NeedMore,
         "first maximum-frame fragment was rejected");
  expect(maximum_policy.feed(bytes_of(maximum.substr(127)),
                             Clock::time_point{} + 2ms) == FeedResult::NeedMore,
         "254 payload bytes were rejected before LF");
  expect(maximum_policy.feed(bytes_of("\n"), Clock::time_point{} + 3ms) ==
             FeedResult::Complete,
         "254-byte payload plus LF was rejected");
  expect_typed_request(maximum_policy, punto::IpcVerb::Reload, maximum_argument,
                       "fragmented 254-byte payload");

  punto::IpcFramePolicy oversized_policy(Clock::time_point{});
  expect(oversized_policy.feed(bytes_of(maximum.substr(0, 127)),
                               Clock::time_point{} + 1ms) ==
             FeedResult::NeedMore,
         "oversize prefix fragment one failed too early");
  expect(oversized_policy.feed(bytes_of(maximum.substr(127)),
                               Clock::time_point{} + 2ms) ==
             FeedResult::NeedMore,
         "oversize prefix fragment two failed too early");
  expect(oversized_policy.feed(bytes_of("a"), Clock::time_point{} + 3ms) ==
             FeedResult::ProtocolError,
         "fragmented 255th payload byte was accepted");
  expect(!oversized_policy.request().has_value(),
         "fragmented oversized frame produced a typed request");
}

void test_lf_is_irreversible_across_recv_segmentation() {
  const Clock::time_point accepted_at{};

  punto::IpcFramePolicy same_chunk(accepted_at);
  expect(same_chunk.feed(bytes_of("SET_STATUS 0\nSET_STATUS 1\n"),
                         accepted_at + 1ms) == FeedResult::Complete,
         "same-chunk first frame was not completed");
  expect_typed_request(same_chunk, punto::IpcVerb::SetStatus, "0",
                       "same-chunk tail");

  punto::IpcFramePolicy separate_chunk(accepted_at);
  expect(separate_chunk.feed(bytes_of("SET_STATUS 0\n"), accepted_at + 1ms) ==
             FeedResult::Complete,
         "first separate frame was not completed");
  expect(separate_chunk.feed(bytes_of("SET_STATUS 1\n"), accepted_at + 2ms) ==
             FeedResult::Complete,
         "response-only policy did not remain complete");
  expect_typed_request(separate_chunk, punto::IpcVerb::SetStatus, "0",
                       "separately received tail");
}

void test_typed_mailbox_is_bounded_fifo_and_owner_drained() {
  punto::IpcCommandMailbox mailbox{2};
  std::vector<std::string> completions;
  const auto completion = [&completions](punto::IpcResult result) {
    completions.push_back(std::move(result.message));
  };
  expect(mailbox.capacity() == 2 && mailbox.size() == 0 && mailbox.is_open(),
         "mailbox did not expose its initial bounded state");
  expect(mailbox.try_enqueue({punto::IpcVerb::GetStatus, "first"},
                             completion) == punto::IpcEnqueueResult::Accepted,
         "mailbox rejected its first typed command");
  expect(mailbox.try_enqueue({punto::IpcVerb::Stats, "second"}, completion) ==
             punto::IpcEnqueueResult::Accepted,
         "mailbox rejected a command within capacity");
  expect(mailbox.try_enqueue({punto::IpcVerb::Reload, "excess"}, completion) ==
             punto::IpcEnqueueResult::Failed,
         "full mailbox was misclassified as a closed admission boundary");
  expect(completions.empty(),
         "mailbox invoked owner/completion code during enqueue");

  auto first = mailbox.try_dequeue();
  auto second = mailbox.try_dequeue();
  expect(first.has_value() && second.has_value() &&
             first->request.verb == punto::IpcVerb::GetStatus &&
             first->request.argument == "first" &&
             second->request.verb == punto::IpcVerb::Stats &&
             second->request.argument == "second",
         "owner drain did not preserve typed mailbox FIFO order");
  expect(!mailbox.try_dequeue().has_value() && mailbox.size() == 0,
         "drained mailbox did not become empty");
  first->complete({true, "one"});
  second->complete({true, "two"});
  expect(completions == std::vector<std::string>{"one", "two"},
         "owner-drained completion ownership was corrupted");

  expect(mailbox.close(), "mailbox close did not reach its admission barrier");
  expect(!mailbox.is_open() &&
             mailbox.try_enqueue({punto::IpcVerb::Stats, {}}, completion) ==
                 punto::IpcEnqueueResult::AdmissionClosed,
         "closed mailbox accepted a new command");
}

void test_mailbox_pending_mutations_preserve_fifo_and_wraparound() {
  using punto::IpcVerb;
  punto::IpcCommandMailbox mailbox{3};
  int completions = 0;
  const auto complete = [&completions](punto::IpcResult) { ++completions; };
  const std::array verbs{IpcVerb::GetStatus, IpcVerb::Stats, IpcVerb::SetStatus,
                         IpcVerb::Reload, IpcVerb::Shutdown,
                         static_cast<IpcVerb>(999)};
  for (const auto verb : verbs) {
    const bool mutating = verb != IpcVerb::GetStatus && verb != IpcVerb::Stats;
    expect(!mailbox.has_pending_mutation(), "empty mailbox cancelled a macro");
    for (const auto queued : {IpcVerb::GetStatus, IpcVerb::Stats, verb}) {
      expect(mailbox.try_enqueue({queued, {}}, complete) ==
                 punto::IpcEnqueueResult::Accepted, "matrix enqueue failed");
    }
    expect(mailbox.try_enqueue({IpcVerb::Reload, {}}, complete) ==
               punto::IpcEnqueueResult::Failed, "matrix exceeded capacity");
    const auto completed_before = completions;
    for (int repeat = 0; repeat < 10; ++repeat) {
      expect(mailbox.has_pending_mutation() == mutating,
             "published write behind reads was misclassified");
      expect(mailbox.size() == 3 && completions == completed_before,
             "observational scan drained or completed a command");
    }
    for (const auto expected : {IpcVerb::GetStatus, IpcVerb::Stats, verb}) {
      expect(mailbox.has_pending_mutation() == mutating,
             "mutation visibility changed before its dequeue");
      auto pending = mailbox.try_dequeue();
      expect(pending && pending->request.verb == expected,
             "scan disturbed wrapped FIFO order");
      pending->complete({true, {}});
    }
    expect(!mailbox.has_pending_mutation() && mailbox.size() == 0,
           "drained mutation remained visible");
    expect(completions == completed_before + 3, "completion ownership changed");
  }
  expect(mailbox.try_enqueue({IpcVerb::Reload, {}}, complete) ==
             punto::IpcEnqueueResult::Accepted, "close fixture enqueue failed");
  expect(mailbox.close() && mailbox.has_pending_mutation(),
         "closing admission hid an already admitted mutation");
  expect(mailbox.try_dequeue().has_value() && !mailbox.has_pending_mutation(),
         "closed mailbox did not retain owner drain semantics");
}

void test_mailbox_close_is_a_linearized_admission_barrier() {
  constexpr std::size_t capacity = 32'768;
  punto::IpcCommandMailbox mailbox{capacity};
  std::atomic<bool> start{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> late_accepted{false};
  std::atomic<std::size_t> post_close_attempts{0};

  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (!closed.load(std::memory_order_acquire)) {
      (void)mailbox.try_enqueue({punto::IpcVerb::Stats, {}},
                                [](punto::IpcResult) {});
    }
    for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
      const auto result = mailbox.try_enqueue({punto::IpcVerb::Stats, {}},
                                              [](punto::IpcResult) {});
      if (result == punto::IpcEnqueueResult::Accepted) {
        late_accepted.store(true, std::memory_order_release);
      }
      if (result == punto::IpcEnqueueResult::AdmissionClosed) {
        post_close_attempts.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  start.store(true, std::memory_order_release);
  while (mailbox.size() < 1'000) {
    std::this_thread::yield();
  }
  expect(mailbox.close(), "concurrent mailbox close did not linearize");
  closed.store(true, std::memory_order_release);
  producer.join();

  expect(!late_accepted.load(std::memory_order_acquire),
         "mailbox accepted a command after close returned");
  expect(post_close_attempts.load(std::memory_order_acquire) == 10'000,
         "post-close enqueue was not classified as AdmissionClosed");
}

void test_mailbox_close_is_bounded_when_a_producer_stalls() {
  punto::IpcCommandMailbox mailbox{2};
  g_mailbox_hook_entered.store(false, std::memory_order_release);
  g_release_mailbox_hook.store(false, std::memory_order_release);
  punto::set_ipc_mailbox_producer_test_hook(hold_mailbox_producer);

  punto::IpcEnqueueResult producer_result = punto::IpcEnqueueResult::Failed;
  std::thread producer{[&] {
    producer_result = mailbox.try_enqueue({punto::IpcVerb::Reload, {}},
                                          [](punto::IpcResult) {});
  }};

  const auto entry_deadline = Clock::now() + 500ms;
  while (!g_mailbox_hook_entered.load(std::memory_order_acquire) &&
         Clock::now() < entry_deadline) {
    std::this_thread::yield();
  }
  const bool entered = g_mailbox_hook_entered.load(std::memory_order_acquire);
  const bool unpublished_mutation = mailbox.has_pending_mutation();
  const auto started = Clock::now();
  const bool closed = mailbox.close(20ms);
  const auto elapsed = Clock::now() - started;

  g_release_mailbox_hook.store(true, std::memory_order_release);
  producer.join();
  punto::set_ipc_mailbox_producer_test_hook(nullptr);

  expect(entered, "mailbox producer test hook was not reached");
  expect(!unpublished_mutation, "scan observed an unpublished producer slot");
  expect(!closed, "stalled mailbox producer bypassed the close deadline");
  expect(elapsed >= 20ms && elapsed < 250ms,
         "mailbox close did not honor its bounded wait window");
  expect(!mailbox.is_open(), "timed-out mailbox close left admission open");
  expect(producer_result == punto::IpcEnqueueResult::AdmissionClosed,
         "stalled producer was admitted after close owned admission");
  expect(mailbox.close(20ms),
         "mailbox close did not recover after the producer released");
}

class TempSocketPath {
public:
  TempSocketPath() {
    char path[] = "/tmp/punto-ipc-contract-XXXXXX";
    char *created = ::mkdtemp(path);
    if (created == nullptr) {
      fail(std::string{"mkdtemp failed: "} + std::strerror(errno));
    }
    directory_ = created;
    socket_ = directory_ / "punto.sock";
  }

  ~TempSocketPath() {
    std::error_code ignored;
    (void)std::filesystem::remove_all(directory_, ignored);
  }

  TempSocketPath(const TempSocketPath &) = delete;
  TempSocketPath &operator=(const TempSocketPath &) = delete;

  [[nodiscard]] const std::string socket() const { return socket_.string(); }

private:
  std::filesystem::path directory_;
  std::filesystem::path socket_;
};

class Client {
public:
  explicit Client(const std::string &socket_path) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      fail(std::string{"socket failed: "} + std::strerror(errno));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
      fail("socket path is too long");
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (::connect(fd_, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) != 0) {
      const int saved_errno = errno;
      close();
      fail(std::string{"connect failed: "} + std::strerror(saved_errno));
    }

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
      const int saved_errno = errno;
      close();
      fail(std::string{"fcntl(O_NONBLOCK) failed: "} +
           std::strerror(saved_errno));
    }
  }

  ~Client() { close(); }

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  void close() noexcept {
    if (fd_ >= 0) {
      (void)::close(fd_);
      fd_ = -1;
    }
  }

  void finish_request() {
    if (::shutdown(fd_, SHUT_WR) != 0 && errno != ENOTCONN) {
      fail(std::string{"shutdown(SHUT_WR) failed: "} + std::strerror(errno));
    }
  }

  void send_all(std::string_view bytes,
                Clock::time_point deadline = Clock::now() + 1500ms) {
    while (!bytes.empty()) {
      const ssize_t written =
          ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (written > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(written));
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        wait_for(POLLOUT, deadline, "request write timed out");
        continue;
      }
      fail(std::string{"request write failed: "} + std::strerror(errno));
    }
  }

  [[nodiscard]] int fd() const noexcept { return fd_; }

private:
  void wait_for(short events, Clock::time_point deadline,
                const char *timeout_message) const {
    while (true) {
      const auto now = Clock::now();
      if (now >= deadline) {
        fail(timeout_message);
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int timeout_ms =
          static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
      pollfd descriptor{fd_, events, 0};
      const int result = ::poll(&descriptor, 1, timeout_ms);
      if (result > 0) {
        return;
      }
      if (result == 0) {
        fail(timeout_message);
      }
      if (errno != EINTR) {
        fail(std::string{"poll failed: "} + std::strerror(errno));
      }
    }
  }

  int fd_ = -1;
};

struct ReadResult {
  std::string bytes;
  bool closed = false;
  bool timed_out = false;
};

ReadResult read_until_closed(int fd, Clock::time_point deadline) {
  ReadResult result;
  while (true) {
    const auto now = Clock::now();
    if (now >= deadline) {
      result.timed_out = true;
      return result;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const int timeout_ms =
        static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready == 0) {
      result.timed_out = true;
      return result;
    }
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      fail(std::string{"response poll failed: "} + std::strerror(errno));
    }

    while (true) {
      char buffer[512];
      const ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
      if (received > 0) {
        result.bytes.append(buffer, static_cast<std::size_t>(received));
        continue;
      }
      if (received == 0) {
        result.closed = true;
        return result;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == ECONNRESET) {
        result.closed = true;
        return result;
      }
      fail(std::string{"response read failed: "} + std::strerror(errno));
    }
  }
}

ReadResult request_response(const std::string &socket_path,
                            std::string_view request,
                            std::chrono::milliseconds timeout = 1500ms) {
  Client client(socket_path);
  client.send_all(request);
  client.finish_request();
  return read_until_closed(client.fd(), Clock::now() + timeout);
}

void expect_exact_response(const ReadResult &result, std::string_view expected,
                           const std::string &context) {
  expect(!result.timed_out, context + ": response timed out");
  expect(result.closed, context + ": server did not close the connection");
  expect(result.bytes == expected, context + ": expected response '" +
                                       std::string{expected} + "', got '" +
                                       result.bytes + "'");
}

class DomainCommandSink {
public:
  punto::IpcEnqueueResult enqueue(punto::IpcRequest request,
                                  punto::IpcResponseCompletion complete) {
    punto::IpcResult response;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(request);
      switch (request.verb) {
      case punto::IpcVerb::GetStatus:
        response = {true, enabled_ ? "ENABLED" : "DISABLED"};
        break;
      case punto::IpcVerb::SetStatus:
        enabled_ = request.argument == "1";
        response = {true, enabled_ ? "ENABLED" : "DISABLED"};
        break;
      case punto::IpcVerb::Reload:
        response = {true, "reloaded"};
        break;
      case punto::IpcVerb::Stats:
        response = {true, "counter=1"};
        break;
      case punto::IpcVerb::Shutdown:
        response = {false, "Shutdown not allowed via IPC"};
        break;
      }
    }
    complete(std::move(response));
    return punto::IpcEnqueueResult::Accepted;
  }

  [[nodiscard]] std::vector<punto::IpcRequest> requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

private:
  mutable std::mutex mutex_;
  bool enabled_ = true;
  std::vector<punto::IpcRequest> requests_;
};

class MailboxDomainExecutor {
public:
  MailboxDomainExecutor(std::shared_ptr<punto::IpcCommandMailbox> mailbox,
                        DomainCommandSink &domain)
      : mailbox_{std::move(mailbox)}, domain_{domain},
        thread_{[this](std::stop_token stop_token) {
          while (!stop_token.stop_requested()) {
            bool drained = false;
            while (auto command = mailbox_->try_dequeue()) {
              drained = true;
              (void)domain_.enqueue(std::move(command->request),
                                    std::move(command->complete));
            }
            if (!drained) {
              std::this_thread::sleep_for(1ms);
            }
          }
        }} {}

  ~MailboxDomainExecutor() {
    thread_.request_stop();
    thread_.join();
  }

  MailboxDomainExecutor(const MailboxDomainExecutor &) = delete;
  MailboxDomainExecutor &operator=(const MailboxDomainExecutor &) = delete;

private:
  std::shared_ptr<punto::IpcCommandMailbox> mailbox_;
  DomainCommandSink &domain_;
  std::jthread thread_;
};

constexpr punto::RuntimeArtifactIdentity kTestSocketIdentity{
    static_cast<uid_t>(1234), static_cast<gid_t>(2345),
    static_cast<mode_t>(0660)};

class RecordingRuntimeArtifactSecurity final
    : public punto::RuntimeArtifactSecurity {
public:
  struct Call {
    int listener_fd;
    std::string path;
    punto::RuntimeArtifactIdentity intended_identity;
  };

  explicit RecordingRuntimeArtifactSecurity(
      Result result = Result::Secured) noexcept
      : result_{result} {}

  Result
  secure_socket(int listener_fd, std::string_view path,
                punto::RuntimeArtifactIdentity intended_identity) override {
    std::lock_guard<std::mutex> lock(mutex_);
    calls_.push_back({listener_fd, std::string{path}, intended_identity});
    return result_;
  }

  [[nodiscard]] std::vector<Call> calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

private:
  const Result result_;
  mutable std::mutex mutex_;
  std::vector<Call> calls_;
};

class ThrowingRuntimeArtifactSecurity final
    : public punto::RuntimeArtifactSecurity {
public:
  Result secure_socket(int, std::string_view,
                       punto::RuntimeArtifactIdentity) override {
    throw std::runtime_error("injected socket security failure");
  }
};

void expect_intended_socket_identity(
    const punto::RuntimeArtifactIdentity &actual, std::string_view context) {
  expect(actual.uid == kTestSocketIdentity.uid &&
             actual.gid == kTestSocketIdentity.gid &&
             actual.mode == kTestSocketIdentity.mode,
         std::string{context} +
             ": server did not pass the exact configured uid/gid/mode");
}

punto::IpcServerOptions server_options(const std::string &socket_path) {
  punto::IpcServerOptions options;
  options.primary_socket_path = socket_path;
  options.allow_fallback_sockets = false;
  options.socket_identity = kTestSocketIdentity;
  return options;
}

std::filesystem::path socket_lease_path(const std::string &socket_path) {
  const std::filesystem::path socket{socket_path};
  return socket.parent_path() / ("." + socket.filename().string() + ".lock");
}

std::string fallback_socket_path(const std::string &primary, pid_t pid) {
  const std::filesystem::path path{primary};
  return (path.parent_path() /
          (path.stem().string() + "-" + std::to_string(pid) +
           path.extension().string()))
      .string();
}

bool socket_accepts_connection(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || path.size() >= sizeof(sockaddr_un::sun_path)) {
    if (fd >= 0) {
      (void)::close(fd);
    }
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  int result = 0;
  do {
    result =
        ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  } while (result != 0 && errno == EINTR);
  (void)::close(fd);
  return result == 0;
}

class ServerFixture {
public:
  ServerFixture()
      : mailbox_{std::make_shared<punto::IpcCommandMailbox>()},
        executor_{mailbox_, domain_},
        security_{punto::RuntimeArtifactSecurity::Result::Secured},
        server_(mailbox_, server_options(socket_path_.socket()), security_) {
    expect(server_.start(), "IPC server failed to start");
  }

  ~ServerFixture() { server_.stop(); }

  ServerFixture(const ServerFixture &) = delete;
  ServerFixture &operator=(const ServerFixture &) = delete;

  [[nodiscard]] const std::string socket() const {
    return socket_path_.socket();
  }

  [[nodiscard]] std::vector<punto::IpcRequest> requests() const {
    return domain_.requests();
  }

  punto::IpcServer &server() { return server_; }

private:
  TempSocketPath socket_path_;
  DomainCommandSink domain_;
  std::shared_ptr<punto::IpcCommandMailbox> mailbox_;
  MailboxDomainExecutor executor_;
  RecordingRuntimeArtifactSecurity security_;
  punto::IpcServer server_;
};

void test_black_box_server_uses_typed_sink_and_responses() {
  ServerFixture fixture;
  expect_exact_response(request_response(fixture.socket(), "GET_STATUS\n"),
                        "OK ENABLED\n", "GET_STATUS");
  expect_exact_response(request_response(fixture.socket(), "SET_STATUS 0\n"),
                        "OK DISABLED\n", "SET_STATUS 0");
  expect_exact_response(request_response(fixture.socket(), "GET_STATUS\n"),
                        "OK DISABLED\n", "GET_STATUS after disable");
  expect_exact_response(request_response(fixture.socket(), "SET_STATUS 1\n"),
                        "OK ENABLED\n", "SET_STATUS 1");
  expect_exact_response(
      request_response(fixture.socket(), "RELOAD /etc/punto/config.yaml\n"),
      "OK reloaded\n", "RELOAD path");
  expect_exact_response(request_response(fixture.socket(), "STATS\n"),
                        "OK counter=1\n", "STATS");
  expect_exact_response(request_response(fixture.socket(), "SHUTDOWN\n"),
                        "ERROR Shutdown not allowed via IPC\n", "SHUTDOWN");

  const std::vector<punto::IpcRequest> requests = fixture.requests();
  expect(requests.size() == 7,
         "black-box server did not enqueue each typed command once");
  const std::vector<std::pair<punto::IpcVerb, std::string>> expected{
      {punto::IpcVerb::GetStatus, ""},
      {punto::IpcVerb::SetStatus, "0"},
      {punto::IpcVerb::GetStatus, ""},
      {punto::IpcVerb::SetStatus, "1"},
      {punto::IpcVerb::Reload, "/etc/punto/config.yaml"},
      {punto::IpcVerb::Stats, ""},
      {punto::IpcVerb::Shutdown, ""},
  };
  for (std::size_t index = 0; index < expected.size(); ++index) {
    expect(requests[index].verb == expected[index].first &&
               requests[index].argument == expected[index].second,
           "black-box typed command mismatch at index " +
               std::to_string(index));
  }

  const std::size_t before_unknown = fixture.requests().size();
  expect_exact_response(request_response(fixture.socket(), "\n"),
                        "ERROR Unknown command\n", "empty command");
  expect_exact_response(request_response(fixture.socket(), "GET_STATUS_JUNK\n"),
                        "ERROR Unknown command\n", "suffixed command");
  expect_exact_response(request_response(fixture.socket(), "SHUTDOWN_JUNK\n"),
                        "ERROR Unknown command\n", "suffixed SHUTDOWN command");
  expect(fixture.requests().size() == before_unknown,
         "framing-valid unknown command reached the typed sink");
}

void test_diagnostic_endpoint_rejects_mutations_before_owner_admission() {
  TempSocketPath socket_path;
  DomainCommandSink domain;
  auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
  MailboxDomainExecutor executor(mailbox, domain);
  RecordingRuntimeArtifactSecurity security;
  punto::IpcServerOptions options = server_options(socket_path.socket());
  options.endpoint_mode = punto::IpcEndpointMode::DiagnosticReadOnly;
  punto::IpcServer server(mailbox, options, security);
  expect(server.start(), "diagnostic server did not start");

  expect_exact_response(request_response(socket_path.socket(), "GET_STATUS\n"),
                        "OK ENABLED\n", "diagnostic GET_STATUS");
  expect_exact_response(request_response(socket_path.socket(), "STATS\n"),
                        "OK counter=1\n", "diagnostic STATS");

  constexpr std::array<std::string_view, 4> mutations{
      "SET_STATUS 0\n", "RELOAD\n", "RELOAD /etc/punto/config.yaml\n",
      "SHUTDOWN\n"};
  for (const std::string_view request : mutations) {
    expect_exact_response(request_response(socket_path.socket(), request),
                          "ERROR Read-only diagnostic endpoint\n",
                          "diagnostic endpoint mutation rejection");
  }

  const auto requests = domain.requests();
  expect(requests.size() == 2,
         "diagnostic mutations crossed the ingress authority boundary");
  expect(requests[0].verb == punto::IpcVerb::GetStatus &&
             requests[1].verb == punto::IpcVerb::Stats,
         "diagnostic endpoint admitted a non-read-only verb");

  // Endpoint authority is immutable. This second attempt models a later role
  // transition and proves an already-created diagnostic listener cannot gain
  // mutation authority through external control-plane state.
  expect_exact_response(
      request_response(socket_path.socket(), "SET_STATUS 1\n"),
      "ERROR Read-only diagnostic endpoint\n",
      "diagnostic endpoint remains read-only after role transition");
  expect(domain.requests().size() == 2,
         "role transition changed diagnostic ingress authority");
  server.stop();
}

void test_socket_security_is_hermetic_and_fail_closed() {
  constexpr std::array<punto::RuntimeArtifactSecurity::Result, 2> failures{
      punto::RuntimeArtifactSecurity::Result::ChmodFailed,
      punto::RuntimeArtifactSecurity::Result::ChownFailed,
  };

  for (const auto failure : failures) {
    TempSocketPath socket_path;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security{failure};
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);

    expect(!server.start(), "socket security failure did not fail start");
    expect(!server.is_running(),
           "server reported running after socket security failure");
    const auto calls = security.calls();
    expect(calls.size() == 1, "socket security was not attempted exactly once");
    expect(calls.front().path == socket_path.socket(),
           "socket security received the wrong owned path");
    expect_intended_socket_identity(calls.front().intended_identity,
                                    "failed socket security");

    expect(calls.front().listener_fd >= 0,
           "socket security did not receive a bound listener");
    errno = 0;
    expect(::fcntl(calls.front().listener_fd, F_GETFD) == -1 && errno == EBADF,
           "listener remained open after socket security failure");
    expect(!std::filesystem::exists(socket_path.socket()),
           "owned socket path remained after socket security failure");
    expect(mailbox->size() == 0,
           "command was accepted after socket security failure");
  }

  TempSocketPath socket_path;
  DomainCommandSink domain;
  auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
  MailboxDomainExecutor executor(mailbox, domain);
  RecordingRuntimeArtifactSecurity security;
  punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                          security);
  expect(server.start(), "successful socket security did not allow start");
  const auto calls = security.calls();
  expect(calls.size() == 1,
         "successful socket security was not applied exactly once");
  expect(calls.front().path == socket_path.socket(),
         "successful socket security received the wrong path");
  expect_intended_socket_identity(calls.front().intended_identity,
                                  "successful socket security");
  expect(calls.front().listener_fd >= 0,
         "successful socket security did not receive a bound listener");
  errno = 0;
  expect(::fcntl(calls.front().listener_fd, F_GETFD) != -1,
         "secured listener was closed before server stop");
  expect_exact_response(request_response(socket_path.socket(), "STATS\n"),
                        "OK counter=1\n", "secured listener accept");
  server.stop();
  expect(!std::filesystem::exists(socket_path.socket()),
         "owned socket path remained after secured server stop");
  const auto requests = domain.requests();
  expect(requests.size() == 1 && requests.front().verb == punto::IpcVerb::Stats,
         "secured listener did not accept exactly one typed command");
}

void create_regular_file(const std::string &path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        static_cast<mode_t>(0600));
  if (fd < 0) {
    fail(std::string{"regular-file fixture open failed: "} +
         std::strerror(errno));
  }
  expect(::close(fd) == 0, "regular-file fixture close failed");
}

void test_server_setup_exception_and_owned_inode_cleanup() {
  {
    TempSocketPath socket_path;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    ThrowingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);

    bool threw = false;
    bool started = false;
    try {
      started = server.start();
    } catch (...) {
      threw = true;
    }
    expect(!threw, "throwing socket security escaped IpcServer::start");
    expect(!started && !server.is_running(),
           "throwing socket security left the server running");
    expect(!std::filesystem::exists(socket_path.socket()),
           "throwing socket security retained the bound socket path");
  }

  {
    TempSocketPath socket_path;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    punto::IpcServerOptions options = server_options(socket_path.socket());
    options.socket_identity = punto::RuntimeArtifactIdentity{
        ::geteuid(), ::getegid(), static_cast<mode_t>(0620)};
    punto::IpcServer server(mailbox, options);
    expect(server.start(), "concrete socket security did not start");
    struct stat status {};
    expect(::lstat(socket_path.socket().c_str(), &status) == 0 &&
               S_ISSOCK(status.st_mode),
           "concrete security path is not a socket");
    expect(status.st_uid == ::geteuid() && status.st_gid == ::getegid() &&
               (status.st_mode & static_cast<mode_t>(0777)) ==
                   static_cast<mode_t>(0620),
           "concrete security did not apply exact uid/gid/mode");
    server.stop();
  }

  {
    TempSocketPath socket_path;
    create_regular_file(socket_path.socket());
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    expect(!server.start(), "non-socket collision was reclaimed");
    struct stat status {};
    expect(::lstat(socket_path.socket().c_str(), &status) == 0 &&
               S_ISREG(status.st_mode),
           "non-socket collision was removed or replaced");
  }

  {
    TempSocketPath socket_path;
    const std::filesystem::path target =
        std::filesystem::path{socket_path.socket()}.parent_path() / "target";
    create_regular_file(target.string());
    expect(::symlink(target.c_str(), socket_path.socket().c_str()) == 0,
           "symlink collision fixture creation failed");
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    expect(!server.start(), "symlink socket path was reclaimed");
    struct stat link_status {};
    expect(::lstat(socket_path.socket().c_str(), &link_status) == 0 &&
               S_ISLNK(link_status.st_mode),
           "symlink socket collision was removed or replaced");
    struct stat target_status {};
    expect(::lstat(target.c_str(), &target_status) == 0 &&
               S_ISREG(target_status.st_mode),
           "symlink collision target was changed");
    expect(::unlink(target.c_str()) == 0,
           "failed to remove symlink target fixture");
  }

  {
    TempSocketPath socket_path;
    const std::filesystem::path parent =
        std::filesystem::path{socket_path.socket()}.parent_path();
    const std::filesystem::path runtime_directory = parent / "runtime";
    const std::filesystem::path runtime_link = parent / "run";
    expect(::mkdir(runtime_directory.c_str(), static_cast<mode_t>(0700)) == 0,
           "runtime directory fixture creation failed");
    expect(::symlink(runtime_directory.c_str(), runtime_link.c_str()) == 0,
           "runtime directory symlink fixture creation failed");
    const std::string path = (runtime_link / "punto.sock").string();

    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(path), security);
    const bool started = server.start();
    if (started) {
      server.stop();
    }
    const bool cleaned = !std::filesystem::exists(path);
    (void)::unlink(runtime_link.c_str());
    (void)::rmdir(runtime_directory.c_str());
    expect(started, "protected symlinked runtime directory rejected startup");
    expect(cleaned,
           "symlinked runtime directory retained the owned socket path");
  }

  {
    TempSocketPath socket_path;
    const int stale = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    expect(stale >= 0, "stale socket fixture creation failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path.socket();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    expect(::bind(stale, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) == 0,
           "stale socket fixture bind failed");
    expect(::close(stale) == 0, "stale socket fixture close failed");

    DomainCommandSink domain;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    MailboxDomainExecutor executor(mailbox, domain);
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(path), security);
    expect(server.start(), "verified stale socket was not reclaimed");
    expect_exact_response(request_response(path, "GET_STATUS\n"),
                          "OK ENABLED\n", "reclaimed stale socket");
    server.stop();
  }

  {
    TempSocketPath socket_path;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    expect(server.start(), "replacement cleanup fixture did not start");

    struct stat owned_status {};
    expect(::lstat(socket_path.socket().c_str(), &owned_status) == 0 &&
               S_ISSOCK(owned_status.st_mode),
           "replacement cleanup fixture has no owned socket inode");
    expect(::unlink(socket_path.socket().c_str()) == 0,
           "failed to unlink owned socket fixture path");
    create_regular_file(socket_path.socket());

    server.stop();
    struct stat replacement_status {};
    expect(::lstat(socket_path.socket().c_str(), &replacement_status) == 0 &&
               S_ISREG(replacement_status.st_mode),
           "server stop unlinked a replacement inode");
    expect(replacement_status.st_dev != owned_status.st_dev ||
               replacement_status.st_ino != owned_status.st_ino,
           "replacement fixture unexpectedly reused the owned inode");
  }
}

void test_fallback_never_blindly_unlinks_collision() {
  TempSocketPath socket_path;
  auto primary_mailbox = std::make_shared<punto::IpcCommandMailbox>();
  RecordingRuntimeArtifactSecurity primary_security;
  punto::IpcServer primary(
      primary_mailbox, server_options(socket_path.socket()), primary_security);
  expect(primary.start(), "primary fallback-collision server did not start");

  const std::filesystem::path primary_path{socket_path.socket()};
  const std::filesystem::path fallback_path =
      primary_path.parent_path() /
      (primary_path.stem().string() + "-" + std::to_string(::getpid()) +
       primary_path.extension().string());
  create_regular_file(fallback_path.string());

  auto secondary_mailbox = std::make_shared<punto::IpcCommandMailbox>();
  RecordingRuntimeArtifactSecurity secondary_security;
  punto::IpcServerOptions options = server_options(socket_path.socket());
  options.allow_fallback_sockets = true;
  punto::IpcServer secondary(secondary_mailbox, options, secondary_security);
  const bool secondary_started = secondary.start();
  if (secondary_started) {
    secondary.stop();
  }
  struct stat fallback_status {};
  const bool fallback_preserved =
      ::lstat(fallback_path.c_str(), &fallback_status) == 0 &&
      S_ISREG(fallback_status.st_mode);
  (void)::unlink(fallback_path.c_str());
  primary.stop();
  expect(!secondary_started,
         "fallback server replaced a pre-existing non-socket path");
  expect(fallback_preserved, "fallback collision was unlinked or replaced");

  TempSocketPath poisoned_path;
  const std::filesystem::path lease = socket_lease_path(poisoned_path.socket());
  const std::filesystem::path lease_target =
      lease.parent_path() / "attacker-controlled-lease";
  create_regular_file(lease_target.string());
  expect(::symlink(lease_target.c_str(), lease.c_str()) == 0,
         "poisoned primary lease fixture creation failed");
  auto poisoned_mailbox = std::make_shared<punto::IpcCommandMailbox>();
  RecordingRuntimeArtifactSecurity poisoned_security;
  punto::IpcServerOptions poisoned_options =
      server_options(poisoned_path.socket());
  poisoned_options.allow_fallback_sockets = true;
  punto::IpcServer poisoned(poisoned_mailbox, poisoned_options,
                            poisoned_security);
  const bool poisoned_started = poisoned.start();
  if (poisoned_started) {
    poisoned.stop();
  }
  const std::string poisoned_fallback =
      fallback_socket_path(poisoned_path.socket(), ::getpid());
  struct stat lease_status {};
  expect(!poisoned_started,
         "invalid primary lease security was bypassed via fallback");
  expect(::lstat(lease.c_str(), &lease_status) == 0 &&
             S_ISLNK(lease_status.st_mode),
         "invalid primary lease artifact was modified");
  expect(!std::filesystem::exists(poisoned_fallback),
         "security-rejected primary created a fallback socket");
}

void test_saturated_listener_probe_is_bounded_and_non_destructive() {
  TempSocketPath socket_path;
  const int listener =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  expect(listener >= 0, "saturated probe listener socket failed");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = socket_path.socket();
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  expect(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) == 0,
         "saturated probe listener bind failed");
  expect(::listen(listener, 0) == 0, "saturated probe listener listen failed");

  std::vector<int> queued_clients;
  bool saturated = false;
  for (int attempt = 0; attempt < 128; ++attempt) {
    const int client =
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    expect(client >= 0, "saturated probe client socket failed");
    if (::connect(client, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) == 0) {
      queued_clients.push_back(client);
      continue;
    }
    if (errno == EAGAIN || errno == EINPROGRESS) {
      queued_clients.push_back(client);
      saturated = true;
      break;
    }
    const int saved_errno = errno;
    (void)::close(client);
    fail(std::string{"failed to saturate Unix listener: "} +
         std::strerror(saved_errno));
  }
  expect(saturated, "Unix listener backlog did not become saturated");

  struct stat before {};
  expect(::lstat(path.c_str(), &before) == 0 && S_ISSOCK(before.st_mode),
         "saturated listener path was not a socket");

  const pid_t child = ::fork();
  expect(child >= 0, "saturated probe fork failed");
  if (child == 0) {
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer contender(mailbox, server_options(path), security);
    const bool started = contender.start();
    if (started) {
      contender.stop();
      ::_exit(2);
    }
    ::_exit(0);
  }

  int child_status = 0;
  const Clock::time_point deadline = Clock::now() + 1500ms;
  pid_t waited = 0;
  while (Clock::now() < deadline) {
    waited = ::waitpid(child, &child_status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }
    ::usleep(5'000);
  }
  if (waited != child) {
    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }

  for (const int client : queued_clients) {
    (void)::close(client);
  }
  (void)::close(listener);

  expect(waited == child, "stale-socket probe blocked on a saturated listener");
  expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
         "contender reclaimed an active saturated listener");
  struct stat after {};
  expect(::lstat(path.c_str(), &after) == 0 && S_ISSOCK(after.st_mode) &&
             after.st_dev == before.st_dev && after.st_ino == before.st_ino,
         "bounded probe changed the active listener inode");
}

void test_runtime_socket_lease_serializes_successor_handoff() {
  TempSocketPath socket_path;
  int predecessor_go[2]{};
  int predecessor_ready[2]{};
  int predecessor_stop[2]{};
  int successor_go[2]{};
  int successor_attempting[2]{};
  int successor_acquired[2]{};
  int successor_inode[2]{};
  int successor_release[2]{};
  expect(::pipe(predecessor_go) == 0 && ::pipe(predecessor_ready) == 0 &&
             ::pipe(predecessor_stop) == 0 && ::pipe(successor_go) == 0 &&
             ::pipe(successor_attempting) == 0 &&
             ::pipe(successor_acquired) == 0 && ::pipe(successor_inode) == 0 &&
             ::pipe(successor_release) == 0,
         "successor handoff pipe creation failed");

  const pid_t predecessor = ::fork();
  expect(predecessor >= 0, "predecessor fork failed");
  if (predecessor == 0) {
    const std::array retained{predecessor_go[0], predecessor_ready[1],
                              predecessor_stop[0]};
    close_descriptors_except(retained);
    char marker = 0;
    if (!read_value_until(predecessor_go[0], marker, Clock::now() + 5s)) {
      ::_exit(2);
    }

    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    const char ready = server.start() ? 'R' : 'F';
    if (!write_value_until(predecessor_ready[1], ready, Clock::now() + 1s) ||
        ready != 'R') {
      ::_exit(3);
    }
    if (!read_value_until(predecessor_stop[0], marker, Clock::now() + 5s)) {
      ::_exit(4);
    }
    server.stop();
    ::_exit(0);
  }

  const pid_t successor = ::fork();
  expect(successor >= 0, "successor fork failed");
  if (successor == 0) {
    const std::array retained{successor_go[0], successor_attempting[1],
                              successor_acquired[1], successor_inode[1],
                              successor_release[0]};
    close_descriptors_except(retained);
    char marker = 0;
    if (!read_value_until(successor_go[0], marker, Clock::now() + 5s)) {
      ::_exit(5);
    }
    const char attempting = 'A';
    if (!write_value_until(successor_attempting[1], attempting,
                           Clock::now() + 1s)) {
      ::_exit(6);
    }
    const std::filesystem::path lease = socket_lease_path(socket_path.socket());
    const int lease_fd =
        ::open(lease.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lease_fd < 0 || ::flock(lease_fd, LOCK_EX) != 0) {
      ::_exit(7);
    }
    const char acquired = 'L';
    if (!write_value_until(successor_acquired[1], acquired,
                           Clock::now() + 1s)) {
      ::_exit(8);
    }
    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socket_path.socket();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (listener < 0 ||
        ::bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
      ::_exit(9);
    }
    struct stat identity {};
    if (::lstat(path.c_str(), &identity) != 0 ||
        !write_value_until(successor_inode[1], identity, Clock::now() + 1s)) {
      ::_exit(10);
    }
    char release = 0;
    if (!read_value_until(successor_release[0], release, Clock::now() + 5s)) {
      ::_exit(11);
    }
    (void)::close(listener);
    (void)::unlink(path.c_str());
    (void)::close(lease_fd);
    ::_exit(0);
  }

  const std::array parent_closes{predecessor_go[0],       predecessor_ready[1],
                                 predecessor_stop[0],     successor_go[0],
                                 successor_attempting[1], successor_acquired[1],
                                 successor_inode[1],      successor_release[0]};
  for (const int fd : parent_closes) {
    (void)::close(fd);
  }

  const Clock::time_point deadline = Clock::now() + 8s;
  const char start = 'S';
  char predecessor_marker = 0;
  const bool predecessor_started =
      write_value_until(predecessor_go[1], start, deadline) &&
      read_value_until(predecessor_ready[0], predecessor_marker, deadline) &&
      predecessor_marker == 'R';

  char attempting = 0;
  const bool successor_reached_lease =
      predecessor_started &&
      write_value_until(successor_go[1], start, deadline) &&
      read_value_until(successor_attempting[0], attempting, deadline) &&
      attempting == 'A';
  pollfd premature{successor_acquired[0], POLLIN, 0};
  const bool lease_blocked_successor =
      successor_reached_lease && ::poll(&premature, 1, 150) == 0;

  const char stop = 'X';
  const bool stop_sent = write_value_until(predecessor_stop[1], stop, deadline);
  char acquired = 0;
  struct stat successor_identity {};
  const bool successor_bound =
      stop_sent &&
      read_value_until(successor_acquired[0], acquired, deadline) &&
      acquired == 'L' &&
      read_value_until(successor_inode[0], successor_identity, deadline);
  struct stat current {};
  const bool successor_preserved =
      successor_bound && ::lstat(socket_path.socket().c_str(), &current) == 0 &&
      current.st_dev == successor_identity.st_dev &&
      current.st_ino == successor_identity.st_ino;
  const bool release_sent =
      write_value_until(successor_release[1], stop, deadline);

  for (const int fd :
       {predecessor_go[1], predecessor_ready[0], predecessor_stop[1],
        successor_go[1], successor_attempting[0], successor_acquired[0],
        successor_inode[0], successor_release[1]}) {
    (void)::close(fd);
  }
  int predecessor_status = 0;
  int successor_status = 0;
  const bool predecessor_reaped =
      waitpid_until(predecessor, predecessor_status, deadline);
  const bool successor_reaped =
      waitpid_until(successor, successor_status, deadline);

  expect(predecessor_started, "lease predecessor did not start");
  expect(successor_reached_lease,
         "successor did not reach the per-socket lease");
  expect(lease_blocked_successor,
         "successor acquired the per-socket lease before predecessor cleanup");
  expect(successor_bound && release_sent,
         "serialized successor failed to acquire and bind");
  expect(successor_preserved,
         "predecessor cleanup removed or replaced the successor inode");
  expect(predecessor_reaped && WIFEXITED(predecessor_status) &&
             WEXITSTATUS(predecessor_status) == 0,
         "predecessor child did not exit cleanly");
  expect(successor_reaped && WIFEXITED(successor_status) &&
             WEXITSTATUS(successor_status) == 0,
         "successor child did not exit cleanly");
}

void test_contended_runtime_socket_lease_is_bounded() {
  TempSocketPath socket_path;
  const std::filesystem::path lease = socket_lease_path(socket_path.socket());
  int ready_pipe[2]{};
  expect(::pipe(ready_pipe) == 0, "socket-lease ready pipe failed");

  const pid_t child = ::fork();
  expect(child >= 0, "socket-lease holder fork failed");
  if (child == 0) {
    (void)::close(ready_pipe[0]);
    const int lease_fd =
        ::open(lease.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lease_fd < 0 || ::flock(lease_fd, LOCK_EX) != 0) {
      ::_exit(2);
    }
    const char ready = 'R';
    if (!write_value_until(ready_pipe[1], ready, Clock::now() + 1s)) {
      ::_exit(3);
    }
    ::usleep(750'000);
    (void)::close(lease_fd);
    (void)::close(ready_pipe[1]);
    ::_exit(0);
  }

  (void)::close(ready_pipe[1]);
  char ready = 0;
  const bool holder_ready =
      read_value_until(ready_pipe[0], ready, Clock::now() + 1500ms);
  (void)::close(ready_pipe[0]);
  expect(holder_ready && ready == 'R',
         "socket-lease holder did not become ready");

  auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
  RecordingRuntimeArtifactSecurity security;
  punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                          security);
  const Clock::time_point started_at = Clock::now();
  const bool started = server.start();
  const auto elapsed = Clock::now() - started_at;
  if (started) {
    server.stop();
  }

  int child_status = 0;
  const bool child_reaped =
      waitpid_until(child, child_status, Clock::now() + 1500ms);
  expect(child_reaped && WIFEXITED(child_status) &&
             WEXITSTATUS(child_status) == 0,
         "socket-lease holder failed");
  expect(!started, "server bypassed a cooperating per-socket lease");
  expect(elapsed < 400ms, "contended per-socket lease blocked server startup");
  expect(!std::filesystem::exists(socket_path.socket()),
         "failed contended startup created a socket artifact");
}

void test_primary_and_fallback_servers_can_run_in_separate_processes() {
  TempSocketPath socket_path;
  int primary_go[2]{};
  int primary_ready[2]{};
  int primary_stop[2]{};
  int fallback_go[2]{};
  int fallback_ready[2]{};
  int fallback_stop[2]{};
  expect(::pipe(primary_go) == 0 && ::pipe(primary_ready) == 0 &&
             ::pipe(primary_stop) == 0 && ::pipe(fallback_go) == 0 &&
             ::pipe(fallback_ready) == 0 && ::pipe(fallback_stop) == 0,
         "primary/fallback coordination pipe creation failed");

  const pid_t primary = ::fork();
  expect(primary >= 0, "primary child fork failed");
  if (primary == 0) {
    const std::array retained{primary_go[0], primary_ready[1], primary_stop[0]};
    close_descriptors_except(retained);
    char marker = 0;
    if (!read_value_until(primary_go[0], marker, Clock::now() + 5s)) {
      ::_exit(2);
    }
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    const char ready = server.start() ? 'R' : 'F';
    std::jthread owner([mailbox](std::stop_token stop_token) {
      while (!stop_token.stop_requested() || mailbox->size() != 0) {
        auto pending = mailbox->try_dequeue();
        if (!pending.has_value()) {
          std::this_thread::yield();
          continue;
        }
        const bool stats = pending->request.verb == punto::IpcVerb::Stats;
        pending->complete({true, stats ? "peer=primary" : "ENABLED"});
      }
    });
    if (!write_value_until(primary_ready[1], ready, Clock::now() + 1s) ||
        ready != 'R') {
      ::_exit(3);
    }
    if (!read_value_until(primary_stop[0], marker, Clock::now() + 6s)) {
      ::_exit(4);
    }
    if (!mailbox->close()) {
      ::_exit(8);
    }
    server.stop();
    owner.request_stop();
    owner.join();
    ::_exit(0);
  }

  const pid_t fallback = ::fork();
  expect(fallback >= 0, "fallback child fork failed");
  if (fallback == 0) {
    const std::array retained{fallback_go[0], fallback_ready[1],
                              fallback_stop[0]};
    close_descriptors_except(retained);
    char marker = 0;
    if (!read_value_until(fallback_go[0], marker, Clock::now() + 5s)) {
      ::_exit(5);
    }
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServerOptions options = server_options(socket_path.socket());
    options.allow_fallback_sockets = true;
    punto::IpcServer server(mailbox, options, security);
    const char ready = server.start() ? 'R' : 'F';
    std::jthread owner([mailbox](std::stop_token stop_token) {
      while (!stop_token.stop_requested() || mailbox->size() != 0) {
        auto pending = mailbox->try_dequeue();
        if (!pending.has_value()) {
          std::this_thread::yield();
          continue;
        }
        const bool stats = pending->request.verb == punto::IpcVerb::Stats;
        pending->complete({true, stats ? "peer=fallback" : "DISABLED"});
      }
    });
    if (!write_value_until(fallback_ready[1], ready, Clock::now() + 1s) ||
        ready != 'R') {
      ::_exit(6);
    }
    if (!read_value_until(fallback_stop[0], marker, Clock::now() + 6s)) {
      ::_exit(7);
    }
    if (!mailbox->close()) {
      ::_exit(9);
    }
    server.stop();
    owner.request_stop();
    owner.join();
    ::_exit(0);
  }

  for (const int fd : {primary_go[0], primary_ready[1], primary_stop[0],
                       fallback_go[0], fallback_ready[1], fallback_stop[0]}) {
    (void)::close(fd);
  }

  const Clock::time_point deadline = Clock::now() + 8s;
  const char start = 'S';
  char primary_marker = 0;
  const bool primary_started =
      write_value_until(primary_go[1], start, deadline) &&
      read_value_until(primary_ready[0], primary_marker, deadline) &&
      primary_marker == 'R';
  char fallback_marker = 0;
  const bool fallback_started =
      primary_started && write_value_until(fallback_go[1], start, deadline) &&
      read_value_until(fallback_ready[0], fallback_marker, deadline) &&
      fallback_marker == 'R';

  const std::string fallback_path =
      fallback_socket_path(socket_path.socket(), fallback);
  struct stat primary_identity {};
  struct stat fallback_identity {};
  const bool both_paths_are_sockets =
      fallback_started &&
      ::lstat(socket_path.socket().c_str(), &primary_identity) == 0 &&
      S_ISSOCK(primary_identity.st_mode) &&
      ::lstat(fallback_path.c_str(), &fallback_identity) == 0 &&
      S_ISSOCK(fallback_identity.st_mode) &&
      (primary_identity.st_dev != fallback_identity.st_dev ||
       primary_identity.st_ino != fallback_identity.st_ino);
  const bool both_accept = both_paths_are_sockets &&
                           socket_accepts_connection(socket_path.socket()) &&
                           socket_accepts_connection(fallback_path);
  const ReadResult primary_stats =
      both_accept ? request_response(socket_path.socket(), "STATS\n")
                  : ReadResult{};
  const ReadResult fallback_stats =
      both_accept ? request_response(fallback_path, "STATS\n") : ReadResult{};

  const char stop = 'X';
  const bool primary_stop_sent =
      write_value_until(primary_stop[1], stop, deadline);
  const bool fallback_stop_sent =
      write_value_until(fallback_stop[1], stop, deadline);
  const bool stops_sent = primary_stop_sent && fallback_stop_sent;
  for (const int fd : {primary_go[1], primary_ready[0], primary_stop[1],
                       fallback_go[1], fallback_ready[0], fallback_stop[1]}) {
    (void)::close(fd);
  }

  int primary_status = 0;
  int fallback_status = 0;
  const bool primary_reaped = waitpid_until(primary, primary_status, deadline);
  const bool fallback_reaped =
      waitpid_until(fallback, fallback_status, deadline);

  expect(primary_started, "primary child server did not start");
  expect(fallback_started,
         "fallback child could not start while the primary lease was held");
  expect(both_paths_are_sockets,
         "primary and fallback did not own distinct socket artifacts");
  expect(both_accept, "primary and fallback listeners were not both usable");
  expect(primary_stats.bytes == "OK peer=primary\n" && primary_stats.closed,
         "primary did not serve its instance-local STATS response");
  expect(fallback_stats.bytes == "OK peer=fallback\n" && fallback_stats.closed,
         "fallback did not serve its instance-local STATS response");
  expect(stops_sent && primary_reaped && fallback_reaped,
         "primary/fallback children did not stop within the deadline");
  expect(WIFEXITED(primary_status) && WEXITSTATUS(primary_status) == 0 &&
             WIFEXITED(fallback_status) && WEXITSTATUS(fallback_status) == 0,
         "primary/fallback child exited with failure");
}

void test_slow_client_isolation_and_eventual_deadline_close() {
  ServerFixture fixture;
  Client stalled(fixture.socket());
  Client partial(fixture.socket());
  partial.send_all("GET_");

  Client healthy(fixture.socket());
  healthy.send_all("GET_STATUS\n");
  healthy.finish_request();
  expect_exact_response(read_until_closed(healthy.fd(), Clock::now() + 1500ms),
                        "OK ENABLED\n",
                        "healthy client behind stalled clients");

  const ReadResult stalled_result =
      read_until_closed(stalled.fd(), Clock::now() + 1500ms);
  expect(stalled_result.closed && stalled_result.bytes.empty(),
         "empty client was not eventually closed after its frame deadline");
  const ReadResult partial_result =
      read_until_closed(partial.fd(), Clock::now() + 1500ms);
  expect(partial_result.closed && partial_result.bytes.empty(),
         "partial client was not eventually closed after its frame deadline");
}

void test_full_server_mailbox_is_fatal_not_admission_closed() {
  TempSocketPath socket_path;
  auto mailbox = std::make_shared<punto::IpcCommandMailbox>(1);
  RecordingRuntimeArtifactSecurity security;
  punto::IpcServerOptions options = server_options(socket_path.socket());
  options.max_clients = 2;
  punto::IpcServer server(mailbox, options, security);
  expect(server.start(), "full-mailbox server did not start");

  Client first(socket_path.socket());
  first.send_all("GET_STATUS\n");
  const Clock::time_point queued_deadline = Clock::now() + 1500ms;
  while (mailbox->size() != 1 && Clock::now() < queued_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  expect(mailbox->size() == 1,
         "first command did not occupy the bounded mailbox");

  Client excess(socket_path.socket());
  excess.send_all("STATS\n");
  const Clock::time_point fatal_deadline = Clock::now() + 1500ms;
  while (!server.fatal_reason().has_value() && Clock::now() < fatal_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  expect(server.fatal_reason() == punto::IpcFatalReason::EnqueueFailure,
         "full mailbox did not latch EnqueueFailure");
  expect(!server.is_running(),
         "server remained running after bounded mailbox exhaustion");
  server.stop();
}

void test_real_server_stop_is_bounded_with_stalled_client() {
  ServerFixture fixture;
  Client stalled(fixture.socket());

  std::promise<void> stopped;
  std::future<void> stopped_future = stopped.get_future();
  std::thread stopper([&fixture, &stopped]() {
    fixture.server().stop();
    stopped.set_value();
  });

  const bool completed_in_time =
      stopped_future.wait_for(3250ms) == std::future_status::ready;
  if (!completed_in_time) {
    stalled.close();
    expect(stopped_future.wait_for(1000ms) == std::future_status::ready,
           "IPC stop did not recover after stalled client close");
  }
  stopper.join();

  expect(completed_in_time,
         "IPC stop exceeded 3000 ms plus scheduler tolerance");
  expect(!std::filesystem::exists(fixture.socket()),
         "IPC socket remained after bounded stop");
}

void test_server_stop_is_bounded_when_owner_callbacks_never_return() {
  TempSocketPath socket_path;
  const pid_t child = ::fork();
  expect(child >= 0, "blocking owner callback watchdog fork failed");
  if (child == 0) {
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    std::atomic<bool> owner_entered{false};
    std::thread owner([mailbox, &owner_entered]() {
      std::optional<punto::IpcPendingCommand> command;
      while (!command.has_value()) {
        command = mailbox->try_dequeue();
        if (!command.has_value()) {
          std::this_thread::yield();
        }
      }
      owner_entered.store(true);
      while (true) {
        ::pause();
      }
    });
    RecordingRuntimeArtifactSecurity security;
    auto server = std::make_unique<punto::IpcServer>(
        mailbox, server_options(socket_path.socket()), security);
    if (!server->start()) {
      ::_exit(2);
    }
    std::unique_ptr<Client> held_client;
    try {
      held_client = std::make_unique<Client>(socket_path.socket());
      held_client->send_all("GET_STATUS\n");
      const Clock::time_point entered_deadline = Clock::now() + 1500ms;
      while (!owner_entered.load() && Clock::now() < entered_deadline) {
        ::usleep(1'000);
      }
      if (!owner_entered.load()) {
        ::_exit(3);
      }
    } catch (...) {
      ::_exit(4);
    }
    const Clock::time_point started_at = Clock::now();
    server->stop();
    server.reset();
    const auto elapsed = Clock::now() - started_at;
    (void)owner;
    ::_exit(elapsed <= 3250ms ? 0 : 5);
  }

  int child_status = 0;
  pid_t waited = 0;
  const Clock::time_point deadline = Clock::now() + 4500ms;
  while (Clock::now() < deadline) {
    waited = ::waitpid(child, &child_status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }
    ::usleep(5'000);
  }
  if (waited != child) {
    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  expect(waited == child && WIFEXITED(child_status) &&
             WEXITSTATUS(child_status) == 0,
         "IpcServer::stop joined a permanently blocked owner callback");
}

void test_server_stop_is_bounded_when_fatal_observer_never_returns() {
  TempSocketPath socket_path;
  const pid_t child = ::fork();
  expect(child >= 0, "blocking fatal observer watchdog fork failed");
  if (child == 0) {
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    auto server = std::make_unique<punto::IpcServer>(
        mailbox, server_options(socket_path.socket()), security);
    if (!server->start()) {
      ::_exit(2);
    }

    std::optional<punto::IpcPendingCommand> held_command;
    std::unique_ptr<Client> held_client;
    try {
      held_client = std::make_unique<Client>(socket_path.socket());
      held_client->send_all("GET_STATUS\n");
      const Clock::time_point command_deadline = Clock::now() + 1500ms;
      while (!held_command.has_value() && Clock::now() < command_deadline) {
        held_command = mailbox->try_dequeue();
        if (!held_command.has_value()) {
          ::usleep(1'000);
        }
      }
      if (!held_command.has_value()) {
        ::_exit(3);
      }
      // Keep the completion alive and unanswered across the shutdown barrier.
      // This causes the poller itself to latch ShutdownTimeout.
    } catch (...) {
      ::_exit(4);
    }

    std::atomic<bool> observer_entered{false};
    std::thread observer([&observer_entered, server_ptr = server.get()]() {
      while (!server_ptr->fatal_reason().has_value()) {
        std::this_thread::yield();
      }
      observer_entered.store(true);
      while (true) {
        ::pause();
      }
    });
    const Clock::time_point started_at = Clock::now();
    server->stop();
    const auto elapsed = Clock::now() - started_at;
    const Clock::time_point entered_deadline = Clock::now() + 100ms;
    while (!observer_entered.load() && Clock::now() < entered_deadline) {
      ::usleep(1'000);
    }
    if (!observer_entered.load() ||
        server->fatal_reason() != punto::IpcFatalReason::ShutdownTimeout) {
      ::_exit(5);
    }
    server.reset();
    (void)observer;
    ::_exit(elapsed <= 3250ms ? 0 : 6);
  }

  int child_status = 0;
  pid_t waited = 0;
  const Clock::time_point deadline = Clock::now() + 4500ms;
  while (Clock::now() < deadline) {
    waited = ::waitpid(child, &child_status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }
    ::usleep(5'000);
  }
  if (waited != child) {
    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  expect(waited == child && WIFEXITED(child_status) &&
             WEXITSTATUS(child_status) == 0,
         "IpcServer::stop joined a permanently blocked fatal observer");
}

void test_server_lifecycle_is_serialized_and_fatal_is_permanent() {
  {
    TempSocketPath socket_path;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    ThrowingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    expect(!server.start(), "throwing setup unexpectedly started");
    expect(server.lifecycle_state() == punto::IpcServerState::Fatal,
           "setup failure did not latch the explicit server FSM");
    expect(server.fatal_reason() == punto::IpcFatalReason::InternalFailure,
           "pull fatal notification lost the setup failure reason");
    expect(!server.start(), "process-lifetime fatal latch allowed restart");
    server.stop();
    expect(server.lifecycle_state() == punto::IpcServerState::Fatal,
           "stop erased the process-lifetime fatal latch");
  }

  {
    TempSocketPath socket_path;
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    punto::IpcServer server(mailbox, server_options(socket_path.socket()),
                            security);
    constexpr std::size_t thread_count = 4;
    constexpr int iterations = 8;
    std::latch release{thread_count};
    std::atomic<bool> lifecycle_threw{false};
    std::vector<std::jthread> callers;
    callers.reserve(thread_count);
    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {
      callers.emplace_back([&, thread_index]() {
        release.arrive_and_wait();
        try {
          for (int iteration = 0; iteration < iterations; ++iteration) {
            if ((thread_index + static_cast<std::size_t>(iteration)) % 2U ==
                0U) {
              (void)server.start();
            } else {
              server.stop();
            }
          }
        } catch (...) {
          lifecycle_threw.store(true);
        }
      });
    }
    callers.clear();
    server.stop();
    expect(!lifecycle_threw.load(),
           "concurrent start/stop escaped an exception");
    expect(server.lifecycle_state() == punto::IpcServerState::Stopped,
           "concurrent lifecycle calls did not converge to Stopped");
    expect(server.start(),
           "serialized server could not restart after an orderly stop");
    expect(server.lifecycle_state() == punto::IpcServerState::Running,
           "successful restart did not enter Running");
    server.stop();
  }
}

void test_server_destruction_with_late_owner_completion_is_safe() {
  TempSocketPath socket_path;
  const pid_t child = ::fork();
  expect(child >= 0, "late owner completion watchdog fork failed");
  if (child == 0) {
    auto mailbox = std::make_shared<punto::IpcCommandMailbox>();
    RecordingRuntimeArtifactSecurity security;
    auto server = std::make_unique<punto::IpcServer>(
        mailbox, server_options(socket_path.socket()), security);
    if (!server->start()) {
      ::_exit(3);
    }
    std::optional<punto::IpcPendingCommand> command;
    try {
      Client client(socket_path.socket());
      client.send_all("GET_STATUS\n");
      const Clock::time_point deadline = Clock::now() + 1500ms;
      while (!command.has_value() && Clock::now() < deadline) {
        command = mailbox->try_dequeue();
        std::this_thread::yield();
      }
      if (!command.has_value()) {
        ::_exit(4);
      }
    } catch (...) {
      ::_exit(5);
    }
    server.reset();
    command->complete({true, "late"});
    ::_exit(0);
  }

  int status = 0;
  pid_t waited = 0;
  const Clock::time_point deadline = Clock::now() + 4s;
  while (Clock::now() < deadline) {
    waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }
    ::usleep(5'000);
  }
  if (waited != child) {
    (void)::kill(child, SIGKILL);
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  }
  expect(waited == child,
         "late owner completion exceeded its bounded watchdog");
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "late owner completion crashed or accessed destroyed server state");
}

class ManualIpcClock final : public punto::IpcClock {
public:
  [[nodiscard]] Clock::time_point now() const noexcept override { return now_; }

  void set(Clock::time_point value) noexcept { now_ = value; }

  void advance(std::chrono::milliseconds duration) noexcept {
    now_ += duration;
  }

private:
  Clock::time_point now_{};
};

class ScriptedTransportIo final : public punto::IpcTransportIo {
public:
  struct PollEvent {
    int fd;
    short revents;
  };

  struct PollInterest {
    int fd;
    short events;
  };

  explicit ScriptedTransportIo(ManualIpcClock &clock) : clock_{clock} {}

  void queue_poll_events(std::vector<PollEvent> events,
                         std::function<void()> before_return = {}) {
    poll_actions_.push_back(PollAction{PollActionKind::Events, 0,
                                       std::move(events),
                                       std::move(before_return)});
  }

  void queue_poll_error(int error, std::function<void()> before_return = {}) {
    poll_actions_.push_back(
        PollAction{PollActionKind::Error, error, {}, std::move(before_return)});
  }

  std::future<void> queue_poll_blocked_until_wakeup() {
    std::lock_guard<std::mutex> lock(wakeup_mutex_);
    expect(!blocking_poll_queued_ && !blocking_poll_waiting_,
           "a blocking poll is already armed");
    expect(pending_wakeups_ == 0,
           "blocking poll was armed with an unconsumed wakeup");
    poll_entered_promise_ = std::promise<void>{};
    blocking_poll_queued_ = true;
    cancel_blocking_poll_ = false;
    poll_actions_.push_back(
        PollAction{PollActionKind::BlockUntilWakeup, 0, {}, {}});
    return poll_entered_promise_.get_future();
  }

  void queue_accept_fd(int fd) { queue_accept_fd(fd, {}); }

  void queue_accept_fd(int fd, std::function<void()> before_return) {
    accept_actions_.push_back(
        AcceptAction{fd, 0, true, std::move(before_return)});
  }

  void queue_accept_error(int error) {
    accept_actions_.push_back(AcceptAction{-1, error, true, {}});
  }

  void queue_receive(std::string bytes) { queue_receive(std::move(bytes), {}); }

  void queue_receive(std::string bytes, std::function<void()> before_return) {
    receive_actions_.push_back(
        ReceiveAction{std::move(bytes), 0, false, std::move(before_return)});
  }

  void queue_receive_error(int error) {
    receive_actions_.push_back(ReceiveAction{{}, error, false, {}});
  }

  void queue_receive_eof() {
    receive_actions_.push_back(ReceiveAction{{}, 0, true, {}});
  }

  void queue_send_limit(std::size_t bytes) { queue_send_limit(bytes, {}); }

  void queue_send_limit(std::size_t bytes,
                        std::function<void()> before_return) {
    send_actions_.push_back(SendAction{bytes, 0, std::move(before_return)});
  }

  void queue_send_error(int error) {
    send_actions_.push_back(SendAction{std::nullopt, error, {}});
  }

  punto::IpcPollResult poll(std::span<pollfd> descriptors,
                            int timeout_ms) override {
    std::vector<PollInterest> interests;
    interests.reserve(descriptors.size());
    for (pollfd &descriptor : descriptors) {
      interests.push_back({descriptor.fd, descriptor.events});
      descriptor.revents = 0;
    }
    poll_interests_.push_back(std::move(interests));
    poll_timeouts_.push_back(timeout_ms);

    if (poll_actions_.empty()) {
      expect(timeout_ms >= 0,
             "transport used an unbounded timeout with a controllable clock");
      clock_.advance(std::chrono::milliseconds{timeout_ms});
      poll_results_.push_back(punto::IpcPollResult::Timeout);
      ++timeout_poll_returns_;
      return punto::IpcPollResult::Timeout;
    }
    PollAction action = std::move(poll_actions_.front());
    poll_actions_.pop_front();
    if (action.kind == PollActionKind::Error) {
      if (action.before_return) {
        action.before_return();
      }
      errno = action.error;
      poll_results_.push_back(punto::IpcPollResult::Error);
      return punto::IpcPollResult::Error;
    }

    if (action.kind == PollActionKind::BlockUntilWakeup) {
      std::unique_lock<std::mutex> lock(wakeup_mutex_);
      blocking_poll_queued_ = false;
      blocking_poll_waiting_ = true;
      poll_entered_promise_.set_value();
      wakeup_condition_.wait(lock, [this]() {
        return pending_wakeups_ != 0 || cancel_blocking_poll_;
      });

      const bool woke_normally = pending_wakeups_ != 0;
      if (woke_normally) {
        --pending_wakeups_;
      }
      blocking_poll_waiting_ = false;
      if (woke_normally) {
        ++wakeup_poll_returns_;
        poll_results_.push_back(punto::IpcPollResult::Wakeup);
        return punto::IpcPollResult::Wakeup;
      }

      ++timeout_poll_returns_;
      poll_results_.push_back(punto::IpcPollResult::Timeout);
      return punto::IpcPollResult::Timeout;
    }

    constexpr short kUnconditionalEvents =
        static_cast<short>(POLLERR | POLLHUP | POLLNVAL);
    for (const PollEvent &event : action.events) {
      auto descriptor = std::find_if(descriptors.begin(), descriptors.end(),
                                     [&event](const pollfd &candidate) {
                                       return candidate.fd == event.fd;
                                     });
      if (descriptor != descriptors.end()) {
        const short requested =
            static_cast<short>(event.revents & descriptor->events);
        const short unconditional =
            static_cast<short>(event.revents & kUnconditionalEvents);
        descriptor->revents =
            static_cast<short>(descriptor->revents | requested | unconditional);
      }
    }
    if (action.before_return) {
      action.before_return();
    }
    const int ready = static_cast<int>(std::count_if(
        descriptors.begin(), descriptors.end(),
        [](const pollfd &descriptor) { return descriptor.revents != 0; }));
    const auto result = ready == 0 ? punto::IpcPollResult::Timeout
                                   : punto::IpcPollResult::Ready;
    poll_results_.push_back(result);
    if (result == punto::IpcPollResult::Timeout) {
      ++timeout_poll_returns_;
    }
    return result;
  }

  void wake() noexcept override {
    {
      std::lock_guard<std::mutex> lock(wakeup_mutex_);
      ++wakeup_calls_;
      if (blocking_poll_waiting_) {
        ++wakeups_while_polling_;
      }
      ++pending_wakeups_;
    }
    wakeup_condition_.notify_one();
  }

  int accept_client(int listener_fd) override {
    accept_listener_fds_.push_back(listener_fd);
    if (accept_actions_.empty()) {
      errno = EAGAIN;
      return -1;
    }
    const AcceptAction action = accept_actions_.front();
    accept_actions_.pop_front();
    if (action.scripted) {
      accept_observations_.push_back(
          {action.error, clock_.now().time_since_epoch()});
    }
    if (action.fd < 0) {
      if (action.before_return) {
        action.before_return();
      }
      errno = action.error;
      return -1;
    }
    if (action.before_return) {
      action.before_return();
    }
    return action.fd;
  }

  ssize_t receive(int fd, std::span<char> destination) override {
    receive_fds_.push_back(fd);
    if (receive_actions_.empty()) {
      errno = EAGAIN;
      return -1;
    }
    ReceiveAction action = std::move(receive_actions_.front());
    receive_actions_.pop_front();
    if (action.before_return) {
      action.before_return();
    }
    if (action.error != 0) {
      receive_errors_.push_back(action.error);
      errno = action.error;
      return -1;
    }
    if (action.eof) {
      return 0;
    }
    const std::size_t count = std::min(destination.size(), action.bytes.size());
    std::copy_n(action.bytes.data(), count, destination.data());
    return static_cast<ssize_t>(count);
  }

  ssize_t send(int fd, std::span<const char> source) override {
    send_fds_.push_back(fd);
    if (!send_actions_.empty()) {
      const SendAction action = send_actions_.front();
      send_actions_.pop_front();
      if (action.before_return) {
        action.before_return();
      }
      if (action.error != 0) {
        send_errors_.push_back(action.error);
        errno = action.error;
        return -1;
      }
      const std::size_t count = std::min(*action.limit, source.size());
      sent_[fd].append(source.data(), count);
      return static_cast<ssize_t>(count);
    }
    sent_[fd].append(source.data(), source.size());
    return static_cast<ssize_t>(source.size());
  }

  int close_fd(int fd) override {
    closed_fds_.push_back(fd);
    return 0;
  }

  int unlink_path(std::string_view path) override {
    unlinked_paths_.emplace_back(path);
    if (unlink_result_ != 0) {
      errno = unlink_error_;
    }
    return unlink_result_;
  }

  std::optional<punto::RuntimeArtifactFileIdentity>
  path_identity(std::string_view) override {
    return path_identity_;
  }

  void replace_path_identity(punto::RuntimeArtifactFileIdentity identity) {
    path_identity_ = identity;
  }

  void fail_unlink(int error) {
    unlink_result_ = -1;
    unlink_error_ = error;
  }

  [[nodiscard]] const std::vector<int> &closed_fds() const {
    return closed_fds_;
  }

  [[nodiscard]] const std::vector<std::string> &unlinked_paths() const {
    return unlinked_paths_;
  }

  [[nodiscard]] const std::vector<int> &receive_fds() const {
    return receive_fds_;
  }

  [[nodiscard]] std::size_t accept_call_count() const {
    return accept_listener_fds_.size();
  }

  [[nodiscard]] const std::vector<int> &receive_errors() const {
    return receive_errors_;
  }

  [[nodiscard]] const std::vector<int> &send_errors() const {
    return send_errors_;
  }

  [[nodiscard]] std::string sent_to(int fd) const {
    const auto it = sent_.find(fd);
    return it == sent_.end() ? std::string{} : it->second;
  }

  [[nodiscard]] const std::vector<std::pair<int, Clock::duration>> &
  accept_observations() const {
    return accept_observations_;
  }

  [[nodiscard]] int last_poll_timeout() const {
    expect(!poll_timeouts_.empty(), "transport has not called poll");
    return poll_timeouts_.back();
  }

  [[nodiscard]] bool last_poll_contains(int fd) const {
    expect(!poll_interests_.empty(), "transport has not called poll");
    const auto &interests = poll_interests_.back();
    return std::any_of(
        interests.begin(), interests.end(),
        [fd](const PollInterest &interest) { return interest.fd == fd; });
  }

  [[nodiscard]] bool last_poll_subscribed(int fd, short events) const {
    expect(!poll_interests_.empty(), "transport has not called poll");
    const auto &interests = poll_interests_.back();
    return std::any_of(interests.begin(), interests.end(),
                       [fd, events](const PollInterest &interest) {
                         return interest.fd == fd &&
                                static_cast<short>(interest.events & events) ==
                                    events;
                       });
  }

  [[nodiscard]] punto::IpcPollResult last_poll_result() const {
    expect(!poll_results_.empty(), "transport has not returned from poll");
    return poll_results_.back();
  }

  [[nodiscard]] std::size_t wakeup_call_count() const {
    std::lock_guard<std::mutex> lock(wakeup_mutex_);
    return wakeup_calls_;
  }

  [[nodiscard]] std::size_t wakeups_while_polling() const {
    std::lock_guard<std::mutex> lock(wakeup_mutex_);
    return wakeups_while_polling_;
  }

  [[nodiscard]] bool blocking_poll_waiting() const {
    std::lock_guard<std::mutex> lock(wakeup_mutex_);
    return blocking_poll_waiting_;
  }

  [[nodiscard]] std::size_t wakeup_poll_return_count() const {
    return wakeup_poll_returns_;
  }

  [[nodiscard]] std::size_t timeout_poll_return_count() const {
    return timeout_poll_returns_;
  }

  void cancel_blocking_poll_for_cleanup() {
    {
      std::lock_guard<std::mutex> lock(wakeup_mutex_);
      cancel_blocking_poll_ = true;
    }
    wakeup_condition_.notify_all();
  }

private:
  enum class PollActionKind { Events, Error, BlockUntilWakeup };

  struct PollAction {
    PollActionKind kind;
    int error;
    std::vector<PollEvent> events;
    std::function<void()> before_return;
  };

  struct AcceptAction {
    int fd;
    int error;
    bool scripted;
    std::function<void()> before_return;
  };

  struct ReceiveAction {
    std::string bytes;
    int error;
    bool eof;
    std::function<void()> before_return;
  };

  struct SendAction {
    std::optional<std::size_t> limit;
    int error;
    std::function<void()> before_return;
  };

  ManualIpcClock &clock_;
  std::deque<PollAction> poll_actions_;
  std::deque<AcceptAction> accept_actions_;
  std::deque<ReceiveAction> receive_actions_;
  std::deque<SendAction> send_actions_;
  std::vector<std::vector<PollInterest>> poll_interests_;
  std::vector<int> poll_timeouts_;
  std::vector<punto::IpcPollResult> poll_results_;
  std::vector<int> accept_listener_fds_;
  std::vector<std::pair<int, Clock::duration>> accept_observations_;
  std::vector<int> receive_fds_;
  std::vector<int> receive_errors_;
  std::vector<int> send_fds_;
  std::vector<int> send_errors_;
  std::map<int, std::string> sent_;
  std::vector<int> closed_fds_;
  std::vector<std::string> unlinked_paths_;
  mutable std::mutex wakeup_mutex_;
  std::condition_variable wakeup_condition_;
  std::promise<void> poll_entered_promise_;
  bool blocking_poll_queued_ = false;
  bool blocking_poll_waiting_ = false;
  bool cancel_blocking_poll_ = false;
  std::size_t pending_wakeups_ = 0;
  std::size_t wakeup_calls_ = 0;
  std::size_t wakeups_while_polling_ = 0;
  std::size_t wakeup_poll_returns_ = 0;
  std::size_t timeout_poll_returns_ = 0;
  std::optional<punto::RuntimeArtifactFileIdentity> path_identity_{
      punto::RuntimeArtifactFileIdentity{1, 2}};
  int unlink_result_ = 0;
  int unlink_error_ = 0;
};

class ScriptedCommandSink {
public:
  punto::IpcEnqueueResult enqueue(punto::IpcRequest request,
                                  punto::IpcResponseCompletion complete) {
    requests_.push_back(std::move(request));
    if (result_ != punto::IpcEnqueueResult::Accepted) {
      return result_;
    }
    if (automatic_response_.has_value()) {
      complete(*automatic_response_);
    } else {
      pending_.push_back(std::move(complete));
    }
    return result_;
  }

  void set_result(punto::IpcEnqueueResult result) { result_ = result; }

  void hold_responses() { automatic_response_.reset(); }

  void set_automatic_response(punto::IpcResult response) {
    automatic_response_ = std::move(response);
  }

  punto::IpcResponseCompletion take_pending_completion() {
    expect(!pending_.empty(), "no pending IPC response completion");
    auto complete = std::move(pending_.front());
    pending_.pop_front();
    return complete;
  }

  void respond_next(punto::IpcResult response) {
    auto complete = take_pending_completion();
    complete(std::move(response));
  }

  [[nodiscard]] const std::vector<punto::IpcRequest> &requests() const {
    return requests_;
  }

  [[nodiscard]] std::size_t pending_count() const { return pending_.size(); }

private:
  punto::IpcEnqueueResult result_ = punto::IpcEnqueueResult::Accepted;
  std::optional<punto::IpcResult> automatic_response_{
      punto::IpcResult{true, "ENABLED"}};
  std::vector<punto::IpcRequest> requests_;
  std::deque<punto::IpcResponseCompletion> pending_;
};

class TransportFixture {
public:
  static constexpr int kListenerFd = 10;
  static constexpr std::string_view kOwnedPath = "/tmp/punto-scripted.sock";

  TransportFixture()
      : io_{clock_},
        command_sink_{[this](punto::IpcRequest request,
                             punto::IpcResponseCompletion complete) {
          return sink_.enqueue(std::move(request), std::move(complete));
        }},
        policy_factory_{[this](Clock::time_point accepted_at) {
          policy_accept_times_.push_back(accepted_at);
          return punto::IpcFramePolicy{accepted_at};
        }},
        transport_{kListenerFd,
                   std::string{kOwnedPath},
                   command_sink_,
                   clock_,
                   io_,
                   policy_factory_,
                   [this](punto::IpcFatalReason reason) {
                     fatal_reasons_.push_back(reason);
                   }} {}

  void accept_client(int fd) {
    io_.queue_accept_fd(fd);
    io_.queue_poll_events({{kListenerFd, POLLIN}});
    transport_.service_once();
  }

  void feed_client(int fd, std::string bytes, short events = POLLIN) {
    io_.queue_receive(std::move(bytes));
    io_.queue_poll_events({{fd, events}});
    transport_.service_once();
  }

  void poll_client(int fd, short events) {
    io_.queue_poll_events({{fd, events}});
    transport_.service_once();
  }

  ManualIpcClock clock_;
  ScriptedTransportIo io_;
  ScriptedCommandSink sink_;
  punto::IpcCommandSink command_sink_;
  std::vector<Clock::time_point> policy_accept_times_;
  punto::IpcFramePolicyFactory policy_factory_;
  std::vector<punto::IpcFatalReason> fatal_reasons_;
  punto::IpcTransport transport_;
};

bool contains_fd(const std::vector<int> &fds, int fd) {
  return std::find(fds.begin(), fds.end(), fd) != fds.end();
}

std::size_t count_fd(const std::vector<int> &fds, int fd) {
  return static_cast<std::size_t>(std::count(fds.begin(), fds.end(), fd));
}

void expect_client_local_close(const TransportFixture &fixture, int client_fd,
                               std::string_view context) {
  expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
         std::string{context} + ": client was not closed exactly once");
  expect(!contains_fd(fixture.io_.closed_fds(), TransportFixture::kListenerFd),
         std::string{context} + ": listener was closed");
  expect(fixture.fatal_reasons_.empty(),
         std::string{context} + ": client-local failure latched fatal");
  expect(fixture.transport_.is_running(),
         std::string{context} + ": transport stopped");
}

void expect_fatal_cleanup(const TransportFixture &fixture,
                          punto::IpcFatalReason reason,
                          std::string_view context) {
  expect(fixture.transport_.fatal_latched(),
         std::string{context} + ": fatal was not latched");
  expect(!fixture.transport_.is_running(),
         std::string{context} + ": fatal transport still reports running");
  expect(fixture.fatal_reasons_.size() == 1 &&
             fixture.fatal_reasons_.front() == reason,
         std::string{context} + ": fatal callback was not exact-once/reasoned");
  expect(count_fd(fixture.io_.closed_fds(), TransportFixture::kListenerFd) == 1,
         std::string{context} + ": listener was not closed exactly once");
  expect(fixture.io_.unlinked_paths() == std::vector<std::string>{std::string{
                                             TransportFixture::kOwnedPath}},
         std::string{context} + ": owned socket cleanup was not exact");
}

void test_injected_transport_uses_accept_time_and_250ms_policy() {
  {
    TransportFixture fixture;
    const int client_fd = 100;
    fixture.clock_.set(Clock::time_point{} + 10ms);
    fixture.accept_client(client_fd);
    expect(fixture.policy_accept_times_ ==
               std::vector<Clock::time_point>{Clock::time_point{} + 10ms},
           "transport did not create one frame policy at accept time");

    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() == 250,
           "fresh accepted client did not drive an exact 250 ms poll timeout");
    expect(fixture.clock_.now() == Clock::time_point{} + 260ms,
           "fake poll timeout did not advance the controllable clock");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "poll timeout return did not expire the client at t+250 ms");
    expect(fixture.sink_.requests().empty(),
           "empty expired transport frame reached the typed sink");
    expect(fixture.fatal_reasons_.empty(),
           "client framing timeout incorrectly became transport-fatal");
  }

  {
    TransportFixture fixture;
    const int client_fd = 101;
    fixture.clock_.set(Clock::time_point{} + 10ms);
    fixture.accept_client(client_fd);

    fixture.clock_.set(Clock::time_point{} + 259ms);
    fixture.feed_client(client_fd, "GET_");
    expect(fixture.io_.last_poll_timeout() >= 0 &&
               fixture.io_.last_poll_timeout() <= 1,
           "partial client at t+249 ms did not cap poll at <= 1 ms");
    expect(!contains_fd(fixture.io_.closed_fds(), client_fd),
           "actual transport expired client at accepted_at + 249 ms");
    expect(fixture.sink_.requests().empty(),
           "partial frame was dispatched before LF");

    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() >= 0 &&
               fixture.io_.last_poll_timeout() <= 1,
           "pending deadline did not keep the timeout at <= 1 ms");
    expect(fixture.clock_.now() == Clock::time_point{} + 260ms,
           "deadline poll did not advance to accepted_at + 250 ms");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "actual transport did not expire client at accepted_at + 250 ms");
    expect(fixture.sink_.requests().empty(),
           "expired partial transport frame reached the typed sink");
    expect(fixture.fatal_reasons_.empty(),
           "partial client timeout incorrectly became transport-fatal");
  }
}

void test_transport_samples_time_after_accept_and_each_receive() {
  {
    TransportFixture fixture;
    const int client_fd = 103;
    fixture.clock_.set(Clock::time_point{} + 10ms);
    fixture.io_.queue_accept_fd(client_fd,
                                [&fixture]() { fixture.clock_.advance(7ms); });
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();
    expect(fixture.policy_accept_times_ ==
               std::vector<Clock::time_point>{Clock::time_point{} + 17ms},
           "frame policy sampled time before successful accept returned");
  }

  {
    TransportFixture fixture;
    const int client_fd = 104;
    fixture.accept_client(client_fd);
    fixture.clock_.set(Clock::time_point{} + 249ms);
    fixture.io_.queue_receive("GET_STATUS\n",
                              [&fixture]() { fixture.clock_.advance(1ms); });
    fixture.io_.queue_poll_events({{client_fd, POLLIN}});
    fixture.transport_.service_once();
    expect(fixture.sink_.requests().empty(),
           "frame received at the expiry boundary reached the typed sink");
    expect_client_local_close(fixture, client_fd,
                              "clock sampled after recv boundary");
  }

  {
    TransportFixture fixture;
    const int client_fd = 105;
    fixture.accept_client(client_fd);
    fixture.clock_.set(Clock::time_point{} + 248ms);
    fixture.feed_client(client_fd, "GET_");
    fixture.clock_.set(Clock::time_point{} + 249ms);
    fixture.io_.queue_receive("STATUS\n",
                              [&fixture]() { fixture.clock_.advance(1ms); });
    fixture.io_.queue_poll_events({{client_fd, POLLIN}});
    fixture.transport_.service_once();
    expect(fixture.sink_.requests().empty(),
           "fragment completed at expiry used the prior recv timestamp");
    expect_client_local_close(fixture, client_fd,
                              "fresh timestamp after fragmented recv");
  }
}

void test_invalid_transport_frames_never_enqueue() {
  {
    TransportFixture fixture;
    const int client_fd = 110;
    fixture.accept_client(client_fd);
    std::string nul = "GET_STATUS";
    nul.push_back('\0');
    nul.push_back('\n');
    fixture.feed_client(client_fd, std::move(nul));
    expect(fixture.sink_.requests().empty(),
           "NUL frame reached the typed sink");
    expect_client_local_close(fixture, client_fd, "NUL frame");
  }

  {
    TransportFixture fixture;
    const int client_fd = 111;
    fixture.accept_client(client_fd);
    std::string maximum(254, 'a');
    fixture.feed_client(client_fd, maximum.substr(0, 127));
    fixture.feed_client(client_fd, maximum.substr(127));
    fixture.feed_client(client_fd, "a");
    expect(fixture.sink_.requests().empty(),
           "fragmented 255-byte payload reached the typed sink");
    expect_client_local_close(fixture, client_fd, "fragmented oversized frame");
  }

  {
    TransportFixture fixture;
    const int client_fd = 112;
    fixture.accept_client(client_fd);
    fixture.feed_client(client_fd, "GET_STATUS");
    fixture.io_.queue_receive_eof();
    fixture.poll_client(client_fd, POLLIN);
    expect(fixture.sink_.requests().empty(),
           "EOF-before-LF frame reached the typed sink");
    expect_client_local_close(fixture, client_fd, "EOF-before-LF frame");
  }

  {
    TransportFixture fixture;
    const int client_fd = 113;
    fixture.accept_client(client_fd);
    fixture.feed_client(client_fd, "GET_\rSTATUS\n");
    expect(fixture.sink_.requests().empty(),
           "misplaced-CR frame reached the typed sink");
    expect_client_local_close(fixture, client_fd, "misplaced-CR frame");
  }
}

void test_transport_enqueues_once_and_ignores_separate_tail() {
  TransportFixture fixture;
  const int client_fd = 114;
  fixture.sink_.hold_responses();
  fixture.accept_client(client_fd);
  fixture.feed_client(client_fd, "SET_STATUS 0\n");
  fixture.feed_client(client_fd, "SET_STATUS 1\n");
  expect(fixture.sink_.requests().size() == 1,
         "separate post-LF recv dispatched a second command");
  expect(fixture.sink_.requests().front().verb == punto::IpcVerb::SetStatus &&
             fixture.sink_.requests().front().argument == "0",
         "separate tail replaced the first typed request");
}

void test_concurrent_client_cap_includes_awaiting_responses() {
  TransportFixture fixture;
  fixture.sink_.hold_responses();

  constexpr int first_fd = 600;
  for (std::size_t index = 0; index < punto::kIpcMaxConcurrentClients;
       ++index) {
    const int fd = first_fd + static_cast<int>(index);
    fixture.accept_client(fd);
    fixture.feed_client(fd, "GET_STATUS\n");
  }
  expect(fixture.sink_.pending_count() == punto::kIpcMaxConcurrentClients,
         "awaiting responses were not retained up to the client cap");

  const int excess_fd =
      first_fd + static_cast<int>(punto::kIpcMaxConcurrentClients);
  fixture.accept_client(excess_fd);
  expect(count_fd(fixture.io_.closed_fds(), excess_fd) == 1,
         "client beyond the concurrent cap was not closed exactly once");
  expect(fixture.transport_.is_running() && fixture.fatal_reasons_.empty(),
         "client-cap rejection became transport-fatal");

  fixture.sink_.respond_next({true, "ENABLED"});
  fixture.poll_client(first_fd, POLLOUT);
  expect(fixture.io_.sent_to(first_fd) == "OK ENABLED\n",
         "an admitted client stopped making progress at the client cap");
  expect(count_fd(fixture.io_.closed_fds(), first_fd) == 1,
         "completed admitted client was not closed exactly once");
}

void cross_thread_completion_wakeup_scenario() {
  TransportFixture fixture;
  const int first_client_fd = 115;
  const int second_client_fd = 116;
  fixture.sink_.hold_responses();
  fixture.accept_client(first_client_fd);
  fixture.feed_client(first_client_fd, "GET_STATUS\n");
  expect(fixture.sink_.requests().size() == 1 &&
             fixture.sink_.pending_count() == 1,
         "first command was not held before the wakeup test");

  auto complete = fixture.sink_.take_pending_completion();
  const Clock::time_point clock_before_poll = fixture.clock_.now();
  const std::size_t timeouts_before_poll =
      fixture.io_.timeout_poll_return_count();
  std::future<void> poll_entered =
      fixture.io_.queue_poll_blocked_until_wakeup();

  std::latch release_completion{1};
  std::promise<void> completion_finished_promise;
  std::future<void> completion_finished =
      completion_finished_promise.get_future();
  std::jthread completion_thread(
      [complete = std::move(complete), &release_completion,
       &completion_finished_promise]() mutable noexcept {
        try {
          release_completion.wait();
          complete({true, "ENABLED"});
          completion_finished_promise.set_value();
        } catch (...) {
          completion_finished_promise.set_exception(std::current_exception());
        }
      });

  std::promise<void> poll_finished_promise;
  std::future<void> poll_finished = poll_finished_promise.get_future();
  std::jthread poller([&fixture, &poll_finished_promise]() noexcept {
    try {
      fixture.transport_.service_once();
      poll_finished_promise.set_value();
    } catch (...) {
      poll_finished_promise.set_exception(std::current_exception());
    }
  });

  const bool poll_entered_in_time =
      poll_entered.wait_for(1500ms) == std::future_status::ready;
  const bool poll_observably_blocked =
      poll_entered_in_time && fixture.io_.blocking_poll_waiting();
  const bool poll_timeout_allows_blocking =
      poll_entered_in_time && fixture.io_.last_poll_timeout() != 0;
  release_completion.count_down();
  const bool completion_finished_in_time =
      completion_finished.wait_for(1500ms) == std::future_status::ready;
  const bool poll_finished_in_time =
      poll_finished.wait_for(1500ms) == std::future_status::ready;

  if (!poll_entered_in_time || !completion_finished_in_time ||
      !poll_finished_in_time) {
    fixture.io_.cancel_blocking_poll_for_cleanup();
  }
  const bool completion_cleanup_finished =
      completion_finished.wait_for(1500ms) == std::future_status::ready;
  const bool poll_cleanup_finished =
      poll_finished.wait_for(1500ms) == std::future_status::ready;
  if (completion_cleanup_finished) {
    completion_thread.join();
  }
  if (poll_cleanup_finished) {
    poller.join();
  }

  expect(completion_cleanup_finished && poll_cleanup_finished,
         "wakeup test threads did not terminate after bounded cleanup");
  expect(poll_entered_in_time,
         "transport poller did not enter the controlled blocking poll");
  poll_entered.get();
  expect(poll_observably_blocked && poll_timeout_allows_blocking,
         "poller was not actually blocked with a nonzero poll timeout");
  expect(completion_finished_in_time,
         "completion blocked while the transport poller was waiting");
  completion_finished.get();
  expect(poll_finished_in_time,
         "transport poll did not return after response completion");
  poll_finished.get();

  expect(fixture.io_.wakeup_call_count() == 1,
         "cross-thread completion did not issue exactly one wakeup");
  expect(fixture.io_.wakeups_while_polling() == 1,
         "wakeup was not issued while poll was observably blocked");
  expect(fixture.io_.wakeup_poll_return_count() == 1 &&
             fixture.io_.last_poll_result() == punto::IpcPollResult::Wakeup,
         "blocked poll did not return through the normal wakeup path");
  expect(fixture.io_.timeout_poll_return_count() == timeouts_before_poll,
         "blocked poll returned by timeout instead of wakeup");
  expect(fixture.clock_.now() == clock_before_poll,
         "wakeup path advanced the controllable clock like a timeout");
  expect(fixture.io_.sent_to(first_client_fd) == "OK ENABLED\n",
         "woken poller did not flush the exact completed response");
  expect(count_fd(fixture.io_.closed_fds(), first_client_fd) == 1,
         "woken poller did not close the first client exactly once");

  fixture.sink_.set_automatic_response({true, "counter=1"});
  fixture.accept_client(second_client_fd);
  fixture.feed_client(second_client_fd, "STATS\n");
  expect(fixture.sink_.requests().size() == 2 &&
             fixture.sink_.requests().back().verb == punto::IpcVerb::Stats,
         "second client was not dispatched after cross-thread completion");
  expect(fixture.io_.sent_to(second_client_fd) == "OK counter=1\n",
         "second client did not receive its exact response");
  expect(count_fd(fixture.io_.closed_fds(), second_client_fd) == 1,
         "second client was not closed exactly once after its response");
}

void test_cross_thread_completion_wakes_blocked_transport_poller() {
  const pid_t child = ::fork();
  if (child < 0) {
    fail(std::string{"wakeup watchdog fork failed: "} + std::strerror(errno));
  }
  if (child == 0) {
    try {
      cross_thread_completion_wakeup_scenario();
      ::_exit(0);
    } catch (const std::exception &error) {
      std::cerr << "wakeup child failed: " << error.what() << '\n'
                << std::flush;
      ::_exit(1);
    } catch (...) {
      ::_exit(2);
    }
  }

  const Clock::time_point deadline = Clock::now() + 7s;
  int status = 0;
  while (true) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
             "cross-thread wakeup child failed its deterministic oracle");
      return;
    }
    if (waited < 0) {
      if (errno == EINTR) {
        continue;
      }
      const int wait_error = errno;
      if (wait_error == ECHILD) {
        fail("wakeup watchdog lost ownership of its child");
      }
      (void)::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      fail(std::string{"wakeup watchdog waitpid failed: "} +
           std::strerror(wait_error));
    }
    if (Clock::now() >= deadline) {
      break;
    }
    ::usleep(5'000);
  }

  (void)::kill(child, SIGKILL);
  pid_t reaped = -1;
  do {
    reaped = ::waitpid(child, &status, 0);
  } while (reaped < 0 && errno == EINTR);
  expect(reaped == child, "wakeup watchdog did not reap timed-out child");
  fail("cross-thread wakeup scenario exceeded its 7 second watchdog");
}

void test_accept_transient_and_resource_backoff_matrix() {
  const std::vector<std::pair<std::string, int>> transient{
      {"EINTR", EINTR},
      {"EAGAIN", EAGAIN},
      {"EWOULDBLOCK", EWOULDBLOCK},
      {"ECONNABORTED", ECONNABORTED},
  };
  for (const auto &[name, error] : transient) {
    TransportFixture fixture;
    fixture.io_.queue_accept_error(error);
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();
    expect(fixture.transport_.is_running() && fixture.fatal_reasons_.empty(),
           "accept " + name + " was not transient");
    expect(
        !contains_fd(fixture.io_.closed_fds(), TransportFixture::kListenerFd),
        "accept " + name + " closed the listener");
  }

  {
    TransportFixture fixture;
    constexpr std::array<int, 4> resource_errors{EMFILE, ENFILE, ENOMEM,
                                                 EMFILE};
    constexpr std::array<int, 4> expected_timeouts{50, 100, 250, 500};
    constexpr std::array<Clock::duration, 4> expected_error_times{0ms, 50ms,
                                                                  150ms, 400ms};

    for (std::size_t index = 0; index < resource_errors.size(); ++index) {
      fixture.io_.queue_accept_error(resource_errors[index]);
      fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
      fixture.transport_.service_once();
      expect(fixture.io_.accept_observations().back().second ==
                 expected_error_times[index],
             "resource accept retry ran at the wrong instant");

      fixture.transport_.service_once();
      expect(fixture.io_.last_poll_timeout() == expected_timeouts[index],
             "resource accept retry did not program the exact backoff");
    }

    expect(fixture.clock_.now() == Clock::time_point{} + 900ms,
           "50/100/250/500 ms resource backoff did not total 900 ms");
    fixture.io_.queue_accept_error(EAGAIN);
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();
    const auto &observations = fixture.io_.accept_observations();
    expect(!observations.empty() && observations.back().first == EAGAIN &&
               observations.back().second == 900ms,
           "accept did not resume at the capped 500 ms deadline");
    expect(fixture.fatal_reasons_.empty(),
           "bounded resource backoff latched transport fatal");
  }

  {
    TransportFixture fixture;
    const int existing_client = 102;
    fixture.accept_client(existing_client);

    fixture.io_.queue_accept_error(EMFILE);
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();

    fixture.clock_.set(Clock::time_point{} + 49ms);
    const std::size_t accepts_before_49ms = fixture.io_.accept_call_count();
    fixture.io_.queue_receive("GET_");
    fixture.io_.queue_poll_events(
        {{TransportFixture::kListenerFd, POLLIN}, {existing_client, POLLIN}});
    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() >= 0 &&
               fixture.io_.last_poll_timeout() <= 1,
           "50 ms accept backoff did not cap poll at t+49 ms");
    expect(!fixture.io_.receive_fds().empty() &&
               fixture.io_.receive_fds().back() == existing_client,
           "existing client was not serviced during accept backoff");
    expect(fixture.sink_.requests().empty(),
           "partial existing client dispatched during accept backoff");
    expect(fixture.io_.accept_call_count() == accepts_before_49ms,
           "accept retried before the 50 ms backoff deadline");

    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() == 1 &&
               fixture.clock_.now() == Clock::time_point{} + 50ms,
           "fake poll did not finish the remaining accept backoff");

    fixture.io_.queue_accept_error(ENFILE);
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();
    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() == 100 &&
               fixture.clock_.now() == Clock::time_point{} + 150ms,
           "second resource retry did not use 100 ms backoff");

    fixture.io_.queue_accept_error(ENOMEM);
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();
    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() == 100,
           "client deadline did not shorten the 250 ms resource backoff");
    expect(fixture.clock_.now() == Clock::time_point{} + 250ms,
           "minimum client deadline was not used as the poll deadline");
    expect_client_local_close(fixture, existing_client,
                              "client deadline during accept backoff");

    fixture.transport_.service_once();
    expect(fixture.io_.last_poll_timeout() == 150 &&
               fixture.clock_.now() == Clock::time_point{} + 400ms,
           "resource retry deadline was lost after client expiry");
  }
}

void test_poll_listener_and_accept_fatal_matrix() {
  {
    TransportFixture fixture;
    fixture.io_.queue_poll_error(EINTR);
    fixture.transport_.service_once();
    expect(fixture.transport_.is_running() && fixture.fatal_reasons_.empty(),
           "poll EINTR was not retried as transient");
  }

  {
    TransportFixture fixture;
    fixture.io_.queue_poll_error(EIO);
    fixture.transport_.service_once();
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::PollFailure,
                         "poll EIO");
    fixture.transport_.service_once();
    expect(fixture.fatal_reasons_.size() == 1,
           "poll fatal callback was repeated after latch");
  }

  const std::vector<std::pair<std::string, short>> listener_events{
      {"listener POLLNVAL", POLLNVAL},
      {"listener POLLERR", POLLERR},
      {"listener POLLHUP", POLLHUP},
  };
  for (const auto &[name, event] : listener_events) {
    TransportFixture fixture;
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, event}});
    fixture.transport_.service_once();
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::ListenerFailure, name);
  }

  const std::vector<std::pair<std::string, int>> accept_fatal{
      {"EBADF", EBADF},
      {"EINVAL", EINVAL},
      {"ENOTSOCK", ENOTSOCK},
      {"unlisted EACCES", EACCES},
  };
  for (const auto &[name, error] : accept_fatal) {
    TransportFixture fixture;
    fixture.io_.queue_accept_error(error);
    fixture.io_.queue_poll_events({{TransportFixture::kListenerFd, POLLIN}});
    fixture.transport_.service_once();
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::AcceptFailure,
                         "accept " + name);
  }

  {
    TransportFixture fixture;
    fixture.io_.queue_poll_error(
        EBADF, [&fixture]() { fixture.transport_.begin_shutdown(); });
    fixture.transport_.service_once();
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::PollFailure,
                         "poll EBADF racing shutdown without owned close");
  }

  {
    TransportFixture fixture;
    fixture.io_.queue_poll_events(
        {{TransportFixture::kListenerFd, POLLHUP}},
        [&fixture]() { fixture.transport_.begin_shutdown(); });
    fixture.transport_.service_once();
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::ListenerFailure,
                         "listener HUP racing shutdown without owned close");
  }
}

void test_receive_error_and_hup_matrix() {
  const std::vector<std::pair<std::string, int>> transient{
      {"EINTR", EINTR},
      {"EAGAIN", EAGAIN},
      {"EWOULDBLOCK", EWOULDBLOCK},
  };
  int client_fd = 200;
  for (const auto &[name, error] : transient) {
    TransportFixture fixture;
    fixture.accept_client(client_fd);
    fixture.io_.queue_receive_error(error);
    fixture.poll_client(client_fd, POLLIN);
    expect(!contains_fd(fixture.io_.closed_fds(), client_fd),
           "receive " + name + " closed the client");
    expect(fixture.transport_.is_running() && fixture.fatal_reasons_.empty(),
           "receive " + name + " was not client-transient");
    ++client_fd;
  }

  const std::vector<std::pair<std::string, int>> client_local{
      {"ECONNRESET", ECONNRESET},
      {"unlisted EIO", EIO},
  };
  for (const auto &[name, error] : client_local) {
    TransportFixture fixture;
    fixture.accept_client(client_fd);
    fixture.io_.queue_receive_error(error);
    fixture.poll_client(client_fd, POLLIN);
    expect_client_local_close(fixture, client_fd, "receive " + name);
    ++client_fd;
  }

  {
    TransportFixture fixture;
    fixture.accept_client(client_fd);
    fixture.io_.queue_receive_eof();
    fixture.poll_client(client_fd, POLLIN);
    expect_client_local_close(fixture, client_fd, "client EOF");
    ++client_fd;
  }

  constexpr std::array<short, 2> terminal_events{static_cast<short>(POLLHUP),
                                                 static_cast<short>(POLLERR)};
  for (const short terminal_event : terminal_events) {
    TransportFixture fixture;
    fixture.accept_client(client_fd);
    fixture.poll_client(client_fd, terminal_event);
    expect_client_local_close(fixture, client_fd,
                              terminal_event == POLLHUP ? "client POLLHUP"
                                                        : "client POLLERR");
    expect(fixture.sink_.requests().empty(),
           "client terminal event without readable data dispatched");
    ++client_fd;
  }

  for (const short terminal_event : terminal_events) {
    TransportFixture fixture;
    fixture.accept_client(client_fd);
    fixture.io_.queue_receive("GET_STATUS\n");
    fixture.poll_client(client_fd, static_cast<short>(POLLIN | terminal_event));
    expect(fixture.sink_.requests().size() == 1,
           "readable client terminal event did not consume complete frame");
    expect_client_local_close(fixture, client_fd,
                              terminal_event == POLLHUP
                                  ? "client POLLIN|POLLHUP"
                                  : "client POLLIN|POLLERR");
    ++client_fd;
  }
}

void service_response_until(TransportFixture &fixture, int client_fd,
                            std::string_view expected) {
  for (int attempt = 0;
       attempt < 4 && fixture.io_.sent_to(client_fd) != expected; ++attempt) {
    fixture.poll_client(client_fd, POLLOUT);
    expect(fixture.io_.last_poll_subscribed(client_fd, POLLOUT),
           "pending response was not subscribed for POLLOUT");
  }
}

void test_response_partial_write_and_backpressure_matrix() {
  constexpr std::string_view expected_response = "OK ENABLED\n";
  const int client_fd = 300;

  {
    TransportFixture fixture;
    fixture.accept_client(client_fd);
    fixture.io_.queue_send_limit(3);
    fixture.io_.queue_send_error(EAGAIN);
    fixture.feed_client(client_fd, "GET_STATUS\n");
    expect(fixture.io_.sent_to(client_fd) != expected_response,
           "partial/EAGAIN response unexpectedly completed synchronously");
    service_response_until(fixture, client_fd, expected_response);
    expect(fixture.io_.sent_to(client_fd) == expected_response,
           "partial response did not resume after EAGAIN backpressure");
    expect(fixture.io_.send_errors() == std::vector<int>{EAGAIN},
           "response EAGAIN path was not exercised exactly once");
    expect(fixture.fatal_reasons_.empty(),
           "response backpressure latched transport fatal");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "flushed response did not close its client exactly once");

    fixture.poll_client(client_fd, POLLOUT);
    expect(!fixture.io_.last_poll_contains(client_fd),
           "flushed client remained in the following poll set");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "post-flush readiness closed the client more than once");
  }

  {
    TransportFixture fixture;
    fixture.accept_client(client_fd + 1);
    fixture.io_.queue_send_error(EINTR);
    fixture.feed_client(client_fd + 1, "GET_STATUS\n");
    service_response_until(fixture, client_fd + 1, expected_response);
    expect(fixture.io_.sent_to(client_fd + 1) == expected_response,
           "response write did not retry EINTR");
    expect(fixture.io_.send_errors() == std::vector<int>{EINTR},
           "response EINTR path was not exercised exactly once");
    expect(fixture.fatal_reasons_.empty(),
           "response EINTR latched transport fatal");
  }

  {
    TransportFixture fixture;
    fixture.accept_client(client_fd + 2);
    fixture.io_.queue_send_error(EWOULDBLOCK);
    fixture.feed_client(client_fd + 2, "GET_STATUS\n");
    service_response_until(fixture, client_fd + 2, expected_response);
    expect(fixture.io_.sent_to(client_fd + 2) == expected_response,
           "response did not resume after EWOULDBLOCK backpressure");
    expect(fixture.io_.send_errors() == std::vector<int>{EWOULDBLOCK},
           "response EWOULDBLOCK path was not exercised exactly once");
    expect(fixture.fatal_reasons_.empty(),
           "response EWOULDBLOCK latched transport fatal");
  }

  {
    TransportFixture fixture;
    const int mixed_fd = client_fd + 3;
    fixture.accept_client(mixed_fd);
    fixture.io_.queue_send_error(EAGAIN);
    fixture.feed_client(mixed_fd, "GET_STATUS\n");
    expect(!contains_fd(fixture.io_.closed_fds(), mixed_fd),
           "initial response backpressure closed the client");

    fixture.io_.queue_send_error(EAGAIN);
    fixture.poll_client(mixed_fd, static_cast<short>(POLLOUT | POLLHUP));
    expect(fixture.io_.send_errors() == std::vector<int>{EAGAIN, EAGAIN},
           "mixed POLLOUT|POLLHUP did not attempt the pending response");
    expect_client_local_close(fixture, mixed_fd,
                              "response POLLOUT|POLLHUP after EAGAIN");
  }

  const std::vector<std::pair<std::string, int>> local_errors{
      {"EPIPE", EPIPE},
      {"ECONNRESET", ECONNRESET},
      {"unlisted EIO", EIO},
  };
  int local_fd = client_fd + 4;
  for (const auto &[name, error] : local_errors) {
    TransportFixture fixture;
    fixture.accept_client(local_fd);
    fixture.io_.queue_send_error(error);
    fixture.feed_client(local_fd, "GET_STATUS\n");
    if (fixture.io_.send_errors().empty()) {
      fixture.poll_client(local_fd, POLLOUT);
    }
    expect(fixture.io_.send_errors() == std::vector<int>{error},
           "response " + name + " injection was not consumed");
    expect_client_local_close(fixture, local_fd, "response " + name);
    ++local_fd;
  }
}

void test_enqueue_failure_and_shutdown_admission_race() {
  {
    TransportFixture fixture;
    const int client_fd = 400;
    fixture.accept_client(client_fd);
    fixture.sink_.set_result(punto::IpcEnqueueResult::Failed);
    fixture.feed_client(client_fd, "GET_STATUS\n");
    expect(fixture.sink_.requests().size() == 1,
           "complete frame did not attempt exactly one enqueue");
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::EnqueueFailure,
                         "command enqueue failure");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "enqueue fatal did not close accepted client exactly once");
  }

  {
    TransportFixture fixture;
    const int client_fd = 401;
    fixture.accept_client(client_fd);
    fixture.sink_.set_result(punto::IpcEnqueueResult::AdmissionClosed);
    fixture.feed_client(client_fd, "GET_STATUS\n");
    expect(fixture.sink_.requests().size() == 1,
           "shutdown admission race did not attempt one typed enqueue");
    expect_client_local_close(fixture, client_fd,
                              "shutdown admission-closed race");
    expect(fixture.io_.sent_to(client_fd).empty(),
           "admission-closed race fabricated a response");
  }
}

void start_held_command(TransportFixture &fixture, int client_fd) {
  fixture.sink_.hold_responses();
  fixture.accept_client(client_fd);
  fixture.feed_client(client_fd, "GET_STATUS\n");
  expect(fixture.sink_.requests().size() == 1 &&
             fixture.sink_.pending_count() == 1,
         "held command was not admitted exactly once");
}

void expect_shutdown_cleanup(const TransportFixture &fixture, int client_fd,
                             std::string_view context) {
  expect(count_fd(fixture.io_.closed_fds(), TransportFixture::kListenerFd) == 1,
         std::string{context} + ": listener was not closed exactly once");
  expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
         std::string{context} + ": client was not closed exactly once");
  expect(fixture.io_.unlinked_paths() == std::vector<std::string>{std::string{
                                             TransportFixture::kOwnedPath}},
         std::string{context} + ": owned path cleanup was not exact");
}

void test_shutdown_3000ms_boundary_and_timeout_cleanup() {
  {
    TransportFixture fixture;
    const int client_fd = 500;
    start_held_command(fixture, client_fd);
    fixture.transport_.begin_shutdown();

    fixture.clock_.set(Clock::time_point{} + 2999ms);
    fixture.io_.queue_poll_events({});
    fixture.transport_.service_once();
    expect(fixture.transport_.stop_result() == punto::IpcStopResult::InProgress,
           "shutdown completed while a command remained active at t+2999");
    expect(fixture.fatal_reasons_.empty(),
           "shutdown latched fatal before its 3000 ms barrier");

    fixture.clock_.set(Clock::time_point{} + 3000ms);
    fixture.sink_.respond_next({true, "ENABLED"});
    fixture.io_.queue_poll_events({{client_fd, POLLOUT}});
    fixture.transport_.service_once();
    expect(fixture.transport_.stop_result() == punto::IpcStopResult::Clean,
           "command completing at t+3000 was not a clean bounded stop");
    expect(fixture.fatal_reasons_.empty(),
           "command completing at t+3000 latched fatal");
    expect_shutdown_cleanup(fixture, client_fd, "clean t+3000 shutdown");
  }

  {
    TransportFixture fixture;
    const int client_fd = 501;
    start_held_command(fixture, client_fd);
    fixture.transport_.begin_shutdown();

    fixture.clock_.set(Clock::time_point{} + 3000ms);
    fixture.io_.queue_poll_events({});
    fixture.transport_.service_once();
    expect(fixture.transport_.stop_result() == punto::IpcStopResult::InProgress,
           "shutdown timed out at, rather than after, t+3000");
    expect(fixture.fatal_reasons_.empty(),
           "shutdown latched fatal at the inclusive barrier");

    fixture.clock_.set(Clock::time_point{} + 3000ms + 1ns);
    fixture.io_.queue_poll_events({});
    fixture.transport_.service_once();
    expect(fixture.transport_.stop_result() == punto::IpcStopResult::TimedOut,
           "shutdown did not time out after t+3000");
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::ShutdownTimeout,
                         "shutdown timeout");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "shutdown timeout did not close client exactly once");
  }

  {
    TransportFixture fixture;
    const int client_fd = 502;
    start_held_command(fixture, client_fd);
    fixture.transport_.begin_shutdown();

    fixture.clock_.set(Clock::time_point{} + 3000ms);
    fixture.sink_.respond_next({true, "ENABLED"});
    fixture.io_.queue_send_limit(64,
                                 [&fixture]() { fixture.clock_.advance(1ms); });
    fixture.io_.queue_poll_events({{client_fd, POLLOUT}});
    fixture.transport_.service_once();
    expect(fixture.transport_.stop_result() == punto::IpcStopResult::TimedOut,
           "last response crossing t+3000 incorrectly produced Clean");
    expect_fatal_cleanup(fixture, punto::IpcFatalReason::ShutdownTimeout,
                         "shutdown response crossing the timeout boundary");
    expect(count_fd(fixture.io_.closed_fds(), client_fd) == 1,
           "cross-boundary response did not close client exactly once");
  }
}

void test_owned_socket_unlink_failure_cannot_report_clean_shutdown() {
  TransportFixture fixture;
  fixture.io_.fail_unlink(EACCES);
  fixture.transport_.begin_shutdown();
  fixture.transport_.service_once();

  expect(fixture.transport_.stop_result() != punto::IpcStopResult::Clean,
         "failed owned-socket unlink was reported as a clean shutdown");
  expect_fatal_cleanup(fixture, punto::IpcFatalReason::InternalFailure,
                       "owned socket unlink failure");
}

using TestFunction = void (*)();

struct TestCase {
  const char *name;
  TestFunction function;
};

} // namespace

int main() {
  (void)::signal(SIGPIPE, SIG_IGN);

  const std::vector<TestCase> tests{
      {"frame deadline boundary",
       test_frame_policy_deadline_boundary_uses_fresh_instances},
      {"documented command grammar", test_documented_command_grammar},
      {"unknown command grammar",
       test_framing_valid_unknown_commands_have_no_typed_request},
      {"invalid frame matrix", test_invalid_frame_matrix_and_fragmented_limit},
      {"LF irreversible", test_lf_is_irreversible_across_recv_segmentation},
      {"typed owner-drained mailbox",
       test_typed_mailbox_is_bounded_fifo_and_owner_drained},
      {"mailbox mutation visibility", test_mailbox_pending_mutations_preserve_fifo_and_wraparound},
      {"linearized mailbox close",
       test_mailbox_close_is_a_linearized_admission_barrier},
      {"bounded mailbox close",
       test_mailbox_close_is_bounded_when_a_producer_stalls},
      {"cross-thread response wakeup",
       test_cross_thread_completion_wakes_blocked_transport_poller},
      {"black-box typed server",
       test_black_box_server_uses_typed_sink_and_responses},
      {"read-only diagnostic endpoint",
       test_diagnostic_endpoint_rejects_mutations_before_owner_admission},
      {"socket security", test_socket_security_is_hermetic_and_fail_closed},
      {"socket setup and inode ownership",
       test_server_setup_exception_and_owned_inode_cleanup},
      {"fallback collision ownership",
       test_fallback_never_blindly_unlinks_collision},
      {"bounded saturated listener probe",
       test_saturated_listener_probe_is_bounded_and_non_destructive},
      {"bounded runtime socket lease",
       test_contended_runtime_socket_lease_is_bounded},
      {"serialized successor handoff",
       test_runtime_socket_lease_serializes_successor_handoff},
      {"primary and fallback coexist",
       test_primary_and_fallback_servers_can_run_in_separate_processes},
      {"slow client isolation",
       test_slow_client_isolation_and_eventual_deadline_close},
      {"full mailbox is fatal",
       test_full_server_mailbox_is_fatal_not_admission_closed},
      {"real bounded stop",
       test_real_server_stop_is_bounded_with_stalled_client},
      {"bounded stop with blocking owner callback",
       test_server_stop_is_bounded_when_owner_callbacks_never_return},
      {"bounded stop with blocking fatal observer",
       test_server_stop_is_bounded_when_fatal_observer_never_returns},
      {"serialized server lifecycle",
       test_server_lifecycle_is_serialized_and_fatal_is_permanent},
      {"late owner completion after destruction",
       test_server_destruction_with_late_owner_completion_is_safe},
      {"injected transport deadline",
       test_injected_transport_uses_accept_time_and_250ms_policy},
      {"fresh accept and receive clocks",
       test_transport_samples_time_after_accept_and_each_receive},
      {"invalid transport frames", test_invalid_transport_frames_never_enqueue},
      {"transport exactly-once enqueue",
       test_transport_enqueues_once_and_ignores_separate_tail},
      {"concurrent client cap",
       test_concurrent_client_cap_includes_awaiting_responses},
      {"accept classification",
       test_accept_transient_and_resource_backoff_matrix},
      {"poll/listener classification",
       test_poll_listener_and_accept_fatal_matrix},
      {"receive classification", test_receive_error_and_hup_matrix},
      {"response classification",
       test_response_partial_write_and_backpressure_matrix},
      {"enqueue classification",
       test_enqueue_failure_and_shutdown_admission_race},
      {"shutdown barrier", test_shutdown_3000ms_boundary_and_timeout_cleanup},
      {"unlink failure is fatal",
       test_owned_socket_unlink_failure_cannot_report_clean_shutdown},
  };

  int failures = 0;
  for (const TestCase &test : tests) {
    try {
      test.function();
      std::cout << "PASS: " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
    }
  }

  if (failures != 0) {
    std::cerr << failures << " IPC contract test(s) failed\n";
    return 1;
  }
  return 0;
}
